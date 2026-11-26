// decode() on any plaintext. Whatever it accepts, encode() must give back byte for byte: one canonical encoding per
// message, and the encoder never refuses what the decoder accepted (an exception here is a crash).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "pms/link/messages.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::as_bytes(std::span{data, size});
    const auto message = pms::link::decode(input);
    if (!message) {
        return 0;
    }
    const auto again = pms::link::encode(*message);
    if (!std::ranges::equal(again, input)) {
        std::abort();
    }
    return 0;
}
