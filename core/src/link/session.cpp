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
// Message 1 = 32-byte ephemeral key + 48-byte encrypted static key (32 + tag) + payload + tag.
constexpr std::size_t message1_overhead = 32 + 32 + detail::tag_size + detail::tag_size;

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
std::optional<Message1> parse_message1(const std::span<const std::byte> payload) {
    if (payload.empty()) {
        return std::nullopt;
    }
    Message1 m;
    m.kind = static_cast<std::uint8_t>(payload[0]);
    auto rest = payload.subspan(1);
    if (m.kind == kind_first_pairing) {
        if (rest.size() < m.token.size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < m.token.size(); ++i) {
            m.token[i] = static_cast<std::uint8_t>(rest[i]);
        }
        rest = rest.subspan(m.token.size());
    } else if (m.kind != kind_reconnect) {
        return std::nullopt;
    }
    if (rest.size() < 2) {
        return std::nullopt;
    }
    const std::size_t length = (static_cast<std::size_t>(rest[0]) << 8U) | static_cast<std::size_t>(rest[1]);
    rest = rest.subspan(2);
    if (length > max_string || rest.size() != length) {
        return std::nullopt;
    }
    m.model.assign(reinterpret_cast<const char*>(rest.data()), rest.size());
    if (!valid_utf8(m.model)) {
        return std::nullopt;
    }
    return m;
}

std::vector<std::byte> message1_payload(const std::uint8_t kind, const std::array<std::uint8_t, 16>* token,
                                        const std::string_view model) {
    if (model.size() > max_string || !valid_utf8(model)) {
        throw std::invalid_argument("phone model must be UTF-8 of at most 1024 bytes");
    }
    std::vector<std::byte> out{static_cast<std::byte>(kind)};
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

// --- ResponderSession ---

ResponderSession::ResponderSession(KeyPair desktop, std::array<std::uint8_t, 16> service_uuid, PinStore& pins)
    : desktop_(desktop), service_uuid_(service_uuid), pins_(pins) {}

ResponderSession::~ResponderSession() = default;

void ResponderSession::open_pairing_window(const std::array<std::uint8_t, 16>& token, const std::uint32_t expiry_unix_s) {
    window_ = Window{token, expiry_unix_s};
}

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
    const auto payload = noise_->read_message(msg1);  // fails unless msg1 was encrypted to our static key
    const auto m = payload ? parse_message1(*payload) : std::nullopt;
    const auto phone_key = noise_->remote_static();
    if (!m || !phone_key) {
        return reject();
    }

    Pinned peer;
    if (m->kind == kind_first_pairing) {
        // The window and its expiry are not secret; the token is compared in constant time.
        if (!window_ || now_unix_s > window_->expiry_unix_s || !detail::equal_constant_time(m->token, window_->token)) {
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
        pins_.pin(peer);
        window_.reset();  // burned: one QR code, one pairing
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
    if (sent_) {
        throw std::logic_error("message 1 was already sent");
    }
    if (message1_overhead + payload.size() > max_preauth_frame) {
        throw std::length_error("message 1 must fit one 512-byte pre-handshake frame; shorten the model");
    }
    sent_ = true;
    auto msg1 = noise_->write_message(payload);
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
