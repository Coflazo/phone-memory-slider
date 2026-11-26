// ResponderSession::on_message1 on a fresh session and pairing window per input: message 1 is the one thing the
// desktop reads from an unauthenticated phone. The desktop key, UUID and token are the golden transcript's
// (core/tests/fixtures/golden_ik_transcript.bin), so the seed message 1 decrypts and its mutations reach the payload
// checks after it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "pms/link/session.hpp"

namespace {

using pms::link::Key32;

class MemoryPins final : public pms::link::PinStore {
public:
    [[nodiscard]] std::optional<pms::link::Pinned> find(const Key32& key) const override {
        const auto it = pins_.find(key);
        return it == pins_.end() ? std::nullopt : std::optional{it->second};
    }
    void pin(const pms::link::Pinned& p) override { pins_[p.phone_key] = p; }
    void unpin(const Key32& key) override { pins_.erase(key); }

private:
    std::map<Key32, pms::link::Pinned> pins_;
};

// Golden transcript fields 1 and 2: private key bytes 00..1F clamped, and its public key.
constexpr Key32 desktop_private{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x5f};
constexpr Key32 desktop_public{0x8f, 0x40, 0xc5, 0xad, 0xb6, 0x8f, 0x25, 0x62, 0x4a, 0xe5, 0xb2,
                               0x14, 0xea, 0x76, 0x7a, 0x6e, 0xc9, 0x4d, 0x82, 0x9d, 0x3d, 0x7b,
                               0x5e, 0x1a, 0xd1, 0xba, 0x6f, 0x3e, 0x21, 0x38, 0x28, 0x5f};
constexpr std::array<std::uint8_t, 16> uuid{0x8f, 0x4d, 0x9d, 0x6a, 0x0c, 0x84, 0x4a, 0x7f,
                                            0xa3, 0x6a, 0x6f, 0xc1, 0x2c, 0x4b, 0x34, 0xbf};
constexpr std::array<std::uint8_t, 16> token{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                             0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
constexpr std::uint32_t now_unix_s = 1000;  // the transcript's window expires at 1000: still open

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using pms::link::HandshakeResult;
    MemoryPins pins;
    pms::link::PairingWindow window(token, now_unix_s);
    pms::link::ResponderSession session({desktop_public, desktop_private}, uuid, pins);
    session.open_pairing_window(window);
    std::vector<std::byte> msg2;
    const auto result = session.on_message1(std::as_bytes(std::span{data, size}), now_unix_s, msg2);
    const bool established = result == HandshakeResult::Established;
    if (result == HandshakeResult::Continue || established == msg2.empty() ||
        window.burned() != (established && session.first_pairing())) {
        std::abort();
    }
    return 0;
}
