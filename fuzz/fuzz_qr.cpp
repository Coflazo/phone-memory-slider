// The scanned QR text is untrusted. base32_decode and decode_pairing accept only canonical text, so whatever they
// accept re-encodes to exactly the same text.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "pms/link/qr.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto bytes = pms::link::base32_decode(text)) {
        if (pms::link::base32_encode(*bytes) != text) {
            std::abort();
        }
    }
    const auto code = pms::link::decode_pairing(text, 0);  // the phone's call: no expiry check
    if (code) {
        if (pms::link::encode_pairing(*code) != text) {
            std::abort();
        }
        // The desktop's call with a clock: accepted exactly through the expiry second.
        if (!pms::link::decode_pairing(text, code->expiry_unix_s) ||
            (code->expiry_unix_s < UINT32_MAX && pms::link::decode_pairing(text, code->expiry_unix_s + 1))) {
            std::abort();
        }
    }
    return 0;
}
