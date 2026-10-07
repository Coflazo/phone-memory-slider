#include "noise_ik.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <noise/protocol.h>

namespace pms::link {
namespace {

constexpr const char* protocol_name = "Noise_IK_25519_ChaChaPoly_SHA256";

void check(const int err, const char* what) {
    if (err != NOISE_ERROR_NONE) {
        throw std::runtime_error(std::string("noise-c: ") + what + " failed (" + std::to_string(err) + ")");
    }
}

using DhState = std::unique_ptr<NoiseDHState, decltype(&noise_dhstate_free)>;

DhState new_x25519() {
    NoiseDHState* state = nullptr;
    check(noise_dhstate_new_by_id(&state, NOISE_DH_CURVE25519), "new X25519 state");
    return DhState(state, &noise_dhstate_free);
}

// noise-c takes uint8_t buffers; std::byte storage may be accessed through them (unsigned char aliases anything).
std::uint8_t* raw(std::vector<std::byte>& bytes) noexcept { return reinterpret_cast<std::uint8_t*>(bytes.data()); }

#ifdef PMS_LINK_TEST_HOOKS
std::optional<KeyPair> fixed_responder_ephemeral;
std::optional<KeyPair> fixed_initiator_ephemeral;
#endif

}  // namespace

#ifdef PMS_LINK_TEST_HOOKS
void set_fixed_ephemeral_for_tests(std::optional<KeyPair> responder, std::optional<KeyPair> initiator) {
    fixed_responder_ephemeral = responder;
    fixed_initiator_ephemeral = initiator;
}
#endif

KeyPair generate_keypair() {
    const auto state = new_x25519();
    check(noise_dhstate_generate_keypair(state.get()), "generate key pair");
    KeyPair pair;
    check(noise_dhstate_get_keypair(state.get(), pair.private_key.data(), pair.private_key.size(), pair.public_key.data(),
                                    pair.public_key.size()),
          "read key pair");
    return pair;
}

namespace detail {

void secure_zero(void* data, const std::size_t size) noexcept { noise_clean(data, size); }

bool small_order_public_key(const Key32& public_key) {
    if (noise_is_zero(public_key.data(), public_key.size()) != 0) {
        return true;
    }
    // 2^254 is a clamped scalar and a multiple of the cofactor 8: it sends every small-order point to zero and no other.
    Key32 scalar{};
    scalar[31] = 0x40;
    const auto mine = new_x25519();
    const auto theirs = new_x25519();
    check(noise_dhstate_set_keypair_private(mine.get(), scalar.data(), scalar.size()), "set check scalar");
    check(noise_dhstate_set_public_key(theirs.get(), public_key.data(), public_key.size()), "set public key to check");
    Key32 shared{};
    check(noise_dhstate_calculate(mine.get(), theirs.get(), shared.data(), shared.size()), "check DH");
    return noise_is_zero(shared.data(), shared.size()) != 0;
}

Key32 x25519_public_key(const Key32& private_key) {
    const auto state = new_x25519();
    check(noise_dhstate_set_keypair_private(state.get(), private_key.data(), private_key.size()), "derive public key");
    Key32 public_key{};
    check(noise_dhstate_get_public_key(state.get(), public_key.data(), public_key.size()), "read public key");
    return public_key;
}

std::string sas_digits(const std::span<const std::uint8_t, 32> handshake_hash) {
    std::uint32_t value = (std::uint32_t{handshake_hash[0]} << 24U) | (std::uint32_t{handshake_hash[1]} << 16U) |
                          (std::uint32_t{handshake_hash[2]} << 8U) | std::uint32_t{handshake_hash[3]};
    value %= 1'000'000U;
    std::string digits(6, '0');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        *it = static_cast<char>('0' + value % 10U);
        value /= 10U;
    }
    return digits;
}

bool equal_constant_time(const std::span<const std::uint8_t> a, const std::span<const std::uint8_t> b) noexcept {
    return a.size() == b.size() && noise_is_equal(a.data(), b.data(), a.size()) == 1;
}

void NoiseIk::HandshakeFree::operator()(NoiseHandshakeState_s* state) const noexcept { noise_handshakestate_free(state); }
void NoiseIk::CipherFree::operator()(NoiseCipherState_s* state) const noexcept { noise_cipherstate_free(state); }

NoiseIk::NoiseIk(const Role role, const KeyPair& local, const std::span<const std::uint8_t> prologue, const Key32* remote_static) {
    NoiseHandshakeState* state = nullptr;
    check(noise_handshakestate_new_by_name(&state, protocol_name,
                                           role == Role::Initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER),
          "new handshake");
    handshake_.reset(state);

    const int key_err = noise_dhstate_set_keypair(noise_handshakestate_get_local_keypair_dh(state), local.private_key.data(),
                                                  local.private_key.size(), local.public_key.data(), local.public_key.size());
    if (key_err == NOISE_ERROR_INVALID_PUBLIC_KEY) {
        throw std::invalid_argument("static key pair: the public key does not belong to the private key");
    }
    check(key_err, "set static key pair");
    if (remote_static != nullptr) {
        check(noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(state), remote_static->data(),
                                           remote_static->size()),
              "set remote static key");
        remote_static_ = *remote_static;
    }
#ifdef PMS_LINK_TEST_HOOKS
    if (const auto& fixed = role == Role::Initiator ? fixed_initiator_ephemeral : fixed_responder_ephemeral) {
        check(noise_dhstate_set_keypair(noise_handshakestate_get_fixed_ephemeral_dh(state), fixed->private_key.data(),
                                        fixed->private_key.size(), fixed->public_key.data(), fixed->public_key.size()),
              "set fixed ephemeral key pair");
    }
#endif
    check(noise_handshakestate_set_prologue(state, prologue.data(), prologue.size()), "set prologue");
    check(noise_handshakestate_start(state), "start handshake");
}

NoiseIk::~NoiseIk() = default;

std::optional<std::vector<std::byte>> NoiseIk::write_message(const std::span<const std::byte> payload) {
    if (!handshake_ || payload.size() > NOISE_MAX_PAYLOAD_LEN - max_handshake_overhead) {
        close();
        return std::nullopt;
    }
    std::vector<std::byte> out(payload.size() + max_handshake_overhead);
    NoiseBuffer message;
    noise_buffer_set_output(message, raw(out), out.size());
    // noise-c only reads the payload (it takes a const NoiseBuffer*), so no copy of it is made.
    NoiseBuffer body;
    noise_buffer_set_input(body, const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(payload.data())),
                           payload.size());
    // noise-c refuses a buffer with no data pointer, so an empty payload goes in as "no payload" (same bytes).
    if (noise_handshakestate_write_message(handshake_.get(), &message, payload.empty() ? nullptr : &body) != NOISE_ERROR_NONE ||
        !after_handshake_step()) {
        close();
        return std::nullopt;
    }
    out.resize(message.size);
    return out;
}

std::optional<std::vector<std::byte>> NoiseIk::read_message(const std::span<const std::byte> message) {
    if (!handshake_ || message.empty() || message.size() > NOISE_MAX_PAYLOAD_LEN) {
        close();
        return std::nullopt;
    }
    std::vector<std::byte> in(message.begin(), message.end());  // noise-c wipes the message buffer it reads
    std::vector<std::byte> payload(message.size());              // a payload is never longer than its message
    NoiseBuffer incoming;
    noise_buffer_set_input(incoming, raw(in), in.size());
    NoiseBuffer body;
    noise_buffer_set_output(body, raw(payload), payload.size());
    if (noise_handshakestate_read_message(handshake_.get(), &incoming, &body) != NOISE_ERROR_NONE) {
        secure_zero(payload.data(), payload.size());
        close();
        return std::nullopt;
    }
    if (const NoiseDHState* remote = noise_handshakestate_get_remote_public_key_dh(handshake_.get());
        remote != nullptr && noise_dhstate_has_public_key(remote) != 0) {
        Key32 key{};
        check(noise_dhstate_get_public_key(remote, key.data(), key.size()), "read remote static key");
        remote_static_ = key;
    }
    if (!after_handshake_step()) {
        secure_zero(payload.data(), payload.size());
        close();
        return std::nullopt;
    }
    payload.resize(body.size);
    return payload;
}

// After the last handshake message: keep the hash, split into the two transport ciphers, free the handshake.
bool NoiseIk::after_handshake_step() {
    if (noise_handshakestate_get_action(handshake_.get()) != NOISE_ACTION_SPLIT) {
        return true;
    }
    NoiseCipherState* send = nullptr;
    NoiseCipherState* receive = nullptr;
    if (noise_handshakestate_get_handshake_hash(handshake_.get(), handshake_hash_.data(), handshake_hash_.size()) != NOISE_ERROR_NONE ||
        noise_handshakestate_split(handshake_.get(), &send, &receive) != NOISE_ERROR_NONE) {
        return false;
    }
    send_.reset(send);
    receive_.reset(receive);
    handshake_.reset();
    return true;
}

bool NoiseIk::established() const noexcept { return send_ != nullptr && receive_ != nullptr; }

std::optional<Key32> NoiseIk::remote_static() const { return remote_static_; }

const std::array<std::uint8_t, 32>& NoiseIk::handshake_hash() const noexcept { return handshake_hash_; }

std::optional<std::vector<std::byte>> NoiseIk::seal(const std::span<const std::byte> plaintext) {
    if (!established() || plaintext.size() > NOISE_MAX_PAYLOAD_LEN - tag_size) {
        close();
        return std::nullopt;
    }
    std::vector<std::byte> buffer(plaintext.size() + tag_size);
    std::copy(plaintext.begin(), plaintext.end(), buffer.begin());
    NoiseBuffer inout;
    noise_buffer_set_inout(inout, raw(buffer), plaintext.size(), buffer.size());
    if (noise_cipherstate_encrypt(send_.get(), &inout) != NOISE_ERROR_NONE) {  // in place: no plaintext remains
        secure_zero(buffer.data(), buffer.size());
        close();
        return std::nullopt;
    }
    return buffer;
}

std::optional<std::vector<std::byte>> NoiseIk::open(const std::span<const std::byte> ciphertext) {
    if (!established() || ciphertext.size() < tag_size || ciphertext.size() > NOISE_MAX_PAYLOAD_LEN) {
        close();
        return std::nullopt;
    }
    std::vector<std::byte> buffer(ciphertext.begin(), ciphertext.end());
    NoiseBuffer inout;
    noise_buffer_set_input(inout, raw(buffer), buffer.size());
    if (noise_cipherstate_decrypt(receive_.get(), &inout) != NOISE_ERROR_NONE) {
        close();
        return std::nullopt;
    }
    buffer.resize(inout.size);  // decrypted in place
    return buffer;
}

void NoiseIk::close() noexcept {
    handshake_.reset();
    send_.reset();
    receive_.reset();
}

}  // namespace detail
}  // namespace pms::link
