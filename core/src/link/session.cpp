#include "pms/link/session.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "noise_ik.hpp"
#include "pms/link/frames.hpp"
#include "pms/link/messages.hpp"

namespace pms::link {
namespace {

constexpr std::uint8_t kind_first_pairing = 1;
constexpr std::uint8_t kind_reconnect = 2;

// "PMS/1" followed by the 16 service UUID bytes.
std::array<std::uint8_t, 21> prologue(const std::array<std::uint8_t, 16>& service_uuid) {
    std::array<std::uint8_t, 21> out{'P', 'M', 'S', '/', '1'};
    std::copy(service_uuid.begin(), service_uuid.end(), out.begin() + 5);
    return out;
}

struct Message1 {
    std::uint8_t kind{};
    std::array<std::uint8_t, 16> token{};
    std::string model;
};

// u8 kind | 16-byte token (kind 1 only) | u16 BE length | UTF-8 model of at most max_string bytes, nothing after.
// The token is copied out only once everything else is valid.
std::optional<Message1> parse_message1(const std::span<const std::byte> payload) {
    if (payload.empty()) {
        return std::nullopt;
    }
    Message1 m;
    m.kind = static_cast<std::uint8_t>(payload[0]);
    if (m.kind != kind_first_pairing && m.kind != kind_reconnect) {
        return std::nullopt;
    }
    const std::size_t header = 1 + (m.kind == kind_first_pairing ? m.token.size() : 0);
    if (payload.size() < header + 2) {
        return std::nullopt;
    }
    const std::size_t length = (static_cast<std::size_t>(payload[header]) << 8U) | static_cast<std::size_t>(payload[header + 1]);
    const auto model = payload.subspan(header + 2);
    if (length > max_string || model.size() != length) {
        return std::nullopt;
    }
    m.model.assign(reinterpret_cast<const char*>(model.data()), model.size());
    if (!valid_utf8(m.model)) {
        return std::nullopt;
    }
    if (m.kind == kind_first_pairing) {
        for (std::size_t i = 0; i < m.token.size(); ++i) {
            m.token[i] = static_cast<std::uint8_t>(payload[1 + i]);
        }
    }
    return m;
}

std::vector<std::byte> message1_payload(const std::uint8_t kind, const std::array<std::uint8_t, 16>* token,
                                        const std::string_view model) {
    if (model.size() > max_string || !valid_utf8(model)) {
        throw std::invalid_argument("phone model must be UTF-8 of at most 1024 bytes");
    }
    std::vector<std::byte> out;
    out.reserve(1 + (token != nullptr ? token->size() : 0) + 2 + model.size());  // no reallocation leaves a token copy behind
    out.push_back(static_cast<std::byte>(kind));
    if (token != nullptr) {
        for (const auto b : *token) {
            out.push_back(static_cast<std::byte>(b));
        }
    }
    out.push_back(static_cast<std::byte>(model.size() >> 8U));
    out.push_back(static_cast<std::byte>(model.size() & 0xFFU));
    for (const char ch : model) {
        out.push_back(static_cast<std::byte>(ch));
    }
    return out;
}

}  // namespace

// --- PairingWindow ---

PairingWindow::PairingWindow(const std::array<std::uint8_t, 16>& token, const std::uint32_t expiry_unix_s) noexcept
    : token_(token), expiry_unix_s_(expiry_unix_s) {}

PairingWindow::~PairingWindow() { detail::secure_zero(token_.data(), token_.size()); }

bool PairingWindow::burned() const noexcept { return state_.load() != Open; }

void PairingWindow::burn() noexcept { state_.store(Burned); }

bool PairingWindow::admits(const std::array<std::uint8_t, 16>& token, const std::uint32_t now_unix_s) const noexcept {
    const bool token_ok = detail::equal_constant_time(token, token_);  // always compared, whatever the state
    return token_ok && state_.load() == Open && now_unix_s <= expiry_unix_s_;
}

bool PairingWindow::take() noexcept {
    std::uint8_t open = Open;
    return state_.compare_exchange_strong(open, Committing);
}

void PairingWindow::release() noexcept {
    std::uint8_t committing = Committing;
    (void)state_.compare_exchange_strong(committing, Open);  // stays Burned if burn() was called meanwhile
}

// --- ResponderSession ---

ResponderSession::ResponderSession(KeyPair desktop, std::array<std::uint8_t, 16> service_uuid, PinStore& pins)
    : desktop_(desktop), service_uuid_(service_uuid), pins_(pins) {
    detail::secure_zero(desktop.private_key.data(), desktop.private_key.size());
}

ResponderSession::~ResponderSession() { detail::secure_zero(desktop_.private_key.data(), desktop_.private_key.size()); }

void ResponderSession::open_pairing_window(PairingWindow& window) noexcept { window_ = &window; }

HandshakeResult ResponderSession::on_message1(const std::span<const std::byte> msg1, const std::uint32_t now_unix_s,
                                              std::vector<std::byte>& msg2_out) {
    msg2_out.clear();
    if (used_) {
        return HandshakeResult::Rejected;  // one handshake per session; an established one stays as it is
    }
    used_ = true;
    if (msg1.size() > max_preauth_frame) {
        return HandshakeResult::Rejected;
    }
    const auto reject = [this] {
        noise_.reset();
        return HandshakeResult::Rejected;
    };

    const auto uuid_prologue = prologue(service_uuid_);
    noise_ = std::make_unique<detail::NoiseIk>(detail::Role::Responder, desktop_, uuid_prologue, nullptr);
    auto payload = noise_->read_message(msg1);  // fails unless msg1 was encrypted to our static key
    auto m = payload ? parse_message1(*payload) : std::nullopt;
    if (payload) {
        detail::secure_zero(payload->data(), payload->size());  // it holds the token
    }
    const auto phone_key = noise_->remote_static();
    if (!m || !phone_key || detail::small_order_public_key(*phone_key)) {
        return reject();
    }

    Pinned peer;
    if (m->kind == kind_first_pairing) {
        const bool admitted = window_ != nullptr && window_->admits(m->token, now_unix_s);
        detail::secure_zero(m->token.data(), m->token.size());
        if (!admitted) {
            return reject();
        }
        peer = Pinned{*phone_key, m->model, now_unix_s};
    } else {
        auto pinned = pins_.find(*phone_key);
        if (!pinned) {
            return reject();
        }
        peer = std::move(*pinned);
    }

    auto msg2 = noise_->write_message({});
    if (!msg2 || !noise_->established()) {
        return reject();
    }
    if (m->kind == kind_first_pairing) {
        // Take the window before pinning, so of two phones racing with one code only the first is ever pinned.
        if (!window_->take()) {
            return reject();
        }
        try {
            pins_.pin(peer);
        } catch (...) {
            window_->release();
            noise_.reset();
            throw;
        }
        window_->burn();
        first_pairing_ = true;
    }
    peer_ = std::move(peer);
    established_ = true;
    msg2_out = std::move(*msg2);
    return HandshakeResult::Established;
}

bool ResponderSession::first_pairing() const noexcept { return established_ && first_pairing_; }

std::string ResponderSession::sas() const {
    if (!established_) {
        throw std::logic_error("no handshake established");
    }
    return detail::sas_digits(noise_->handshake_hash());
}

const Pinned& ResponderSession::peer() const {
    if (!established_) {
        throw std::logic_error("no handshake established");
    }
    return peer_;
}

std::vector<std::byte> ResponderSession::seal(const std::span<const std::byte> plaintext) {
    if (plaintext.size() > max_plaintext) {
        throw std::length_error("plaintext does not fit one frame");
    }
    if (!established_ || !noise_->established()) {
        throw std::logic_error("session is not open");
    }
    auto sealed = noise_->seal(plaintext);
    if (!sealed) {
        throw std::runtime_error("transport encryption failed; session closed");  // nonce exhausted
    }
    return std::move(*sealed);
}

std::optional<std::vector<std::byte>> ResponderSession::open(const std::span<const std::byte> ct) {
    if (!established_ || !noise_->established()) {
        return std::nullopt;
    }
    return noise_->open(ct);  // a failure drops the keys, so the session stays closed
}

// --- InitiatorSession ---

InitiatorSession::InitiatorSession(KeyPair phone, Key32 desktop_static, std::array<std::uint8_t, 16> service_uuid) {
    const auto uuid_prologue = prologue(service_uuid);
    noise_ = std::make_unique<detail::NoiseIk>(detail::Role::Initiator, phone, uuid_prologue, &desktop_static);
    detail::secure_zero(phone.private_key.data(), phone.private_key.size());  // noise-c holds (and later wipes) its own copy
}

InitiatorSession::~InitiatorSession() = default;

std::vector<std::byte> InitiatorSession::message1_first_pairing(const std::array<std::uint8_t, 16>& token,
                                                                const std::string_view model) {
    return message1(message1_payload(kind_first_pairing, &token, model));
}

std::vector<std::byte> InitiatorSession::message1_reconnect(const std::string_view model) {
    return message1(message1_payload(kind_reconnect, nullptr, model));
}

std::vector<std::byte> InitiatorSession::message1(std::vector<std::byte> payload) {
    const bool repeated = sent_;
    const bool too_long = detail::max_handshake_overhead + payload.size() > max_preauth_frame;
    auto msg1 = repeated || too_long ? std::nullopt : noise_->write_message(payload);
    detail::secure_zero(payload.data(), payload.size());  // it holds the token
    if (repeated) {
        throw std::logic_error("message 1 was already sent");
    }
    if (too_long) {
        throw std::length_error("message 1 must fit one 512-byte pre-handshake frame; shorten the model");
    }
    sent_ = true;
    if (!msg1) {
        throw std::runtime_error("writing message 1 failed");
    }
    return std::move(*msg1);
}

bool InitiatorSession::on_message2(const std::span<const std::byte> msg2) {
    if (established_) {
        return false;  // one message 2 per session; an established one stays as it is
    }
    if (!sent_ || msg2.size() > max_preauth_frame) {
        noise_->close();
        return false;
    }
    const auto payload = noise_->read_message(msg2);
    if (!payload || !payload->empty() || !noise_->established()) {  // message 2 carries nothing
        noise_->close();
        return false;
    }
    established_ = true;
    return true;
}

std::string InitiatorSession::sas() const {
    if (!established_) {
        throw std::logic_error("no handshake established");
    }
    return detail::sas_digits(noise_->handshake_hash());
}

std::vector<std::byte> InitiatorSession::seal(const std::span<const std::byte> plaintext) {
    if (plaintext.size() > max_plaintext) {
        throw std::length_error("plaintext does not fit one frame");
    }
    if (!established_ || !noise_->established()) {
        throw std::logic_error("session is not open");
    }
    auto sealed = noise_->seal(plaintext);
    if (!sealed) {
        throw std::runtime_error("transport encryption failed; session closed");  // nonce exhausted
    }
    return std::move(*sealed);
}

std::optional<std::vector<std::byte>> InitiatorSession::open(const std::span<const std::byte> ct) {
    if (!established_ || !noise_->established()) {
        return std::nullopt;
    }
    return noise_->open(ct);  // a failure drops the keys, so the session stays closed
}

}  // namespace pms::link
