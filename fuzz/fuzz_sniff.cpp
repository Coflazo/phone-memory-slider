// sniff_media types every file from its leading bytes. It must never read past them, and bytes past sniff_bytes must
// not change the answer.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "pms/media_format.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span{data, size});
    const auto result = pms::sniff_media(bytes);
    if (pms::to_string(result.format).empty()) {
        std::abort();
    }
    if (size > pms::sniff_bytes) {
        const auto head = pms::sniff_media(bytes.first(pms::sniff_bytes));
        if (head.format != result.format || head.animated != result.animated || head.video != result.video) {
            std::abort();
        }
    }
    return 0;
}
