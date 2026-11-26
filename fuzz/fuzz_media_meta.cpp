// The metadata parsers read the head of every gallery file: the input is tried as a JPEG prefix, as an EXIF TIFF block
// and as XMP text. What the JPEG finders return must lie inside the input.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "pms/media_meta.hpp"

namespace {

bool inside(const void* begin, const std::size_t size, const std::uint8_t* data, const std::size_t data_size) {
    if (size == 0) {
        return true;  // an empty view reads nothing, wherever it points
    }
    const auto at = reinterpret_cast<std::uintptr_t>(begin);
    const auto lo = reinterpret_cast<std::uintptr_t>(data);
    return at >= lo && at - lo <= data_size && size <= data_size - (at - lo);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span{data, size});
    (void)pms::parse_motion_photo_xmp(std::string_view(reinterpret_cast<const char*>(data), size));
    (void)pms::parse_exif(bytes);
    if (const auto xmp = pms::find_jpeg_xmp(bytes)) {
        if (!inside(xmp->data(), xmp->size(), data, size)) {
            std::abort();
        }
        (void)pms::parse_motion_photo_xmp(*xmp);
    }
    if (const auto tiff = pms::find_jpeg_exif(bytes)) {
        if (!inside(tiff->data(), tiff->size(), data, size)) {
            std::abort();
        }
        (void)pms::parse_exif(*tiff);
    }
    return 0;
}
