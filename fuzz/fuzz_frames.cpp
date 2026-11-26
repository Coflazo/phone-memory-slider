// FrameDecoder on any byte stream, fed in reads whose size the input picks, under either cap. Every frame it returns
// is within the cap, and the frames re-encoded are exactly the bytes they were read from.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "pms/link/frames.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const std::size_t cap = (data[0] & 1U) != 0 ? pms::link::max_preauth_frame : pms::link::max_frame;
    const std::size_t step = 1U + data[1];  // read size, 1 to 256 bytes
    const auto input = std::as_bytes(std::span{data + 2, size - 2});

    pms::link::FrameDecoder decoder{cap};
    std::vector<std::byte> reencoded;
    for (std::size_t at = 0; at < input.size(); at += step) {
        if (!decoder.feed(input.subspan(at, std::min(step, input.size() - at)))) {
            if (!decoder.failed() || decoder.next()) {
                std::abort();  // a failed decoder stays failed and returns nothing
            }
            break;
        }
        while (const auto frame = decoder.next()) {
            if (frame->size() < pms::link::min_frame || frame->size() > cap) {
                std::abort();
            }
            const auto encoded = pms::link::encode_frame(*frame);
            reencoded.insert(reencoded.end(), encoded.begin(), encoded.end());
        }
    }
    if (reencoded.size() > input.size() || !std::equal(reencoded.begin(), reencoded.end(), input.begin())) {
        std::abort();
    }
    return 0;
}
