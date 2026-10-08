#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pms {

// video_length is untrusted: callers check it against the file size before seeking.
struct MotionPhotoInfo { std::uint64_t video_length{}; std::int64_t presentation_us{-1}; };

// Google Motion Photo v1 (Camera:MotionPhoto + Container:Directory) and legacy MicroVideo/MicroVideoOffset.
[[nodiscard]] std::optional<MotionPhotoInfo> parse_motion_photo_xmp(std::string_view xmp) noexcept;

// Finds the APP1 "http://ns.adobe.com/xap/1.0/\0" XMP packet in a JPEG prefix.
// The view points into `jpeg` and is cut short when the prefix ends inside the segment.
[[nodiscard]] std::optional<std::string_view> find_jpeg_xmp(std::span<const std::byte> jpeg) noexcept;

// Finds the APP1 "Exif\0\0" TIFF block in a JPEG prefix (same lifetime and truncation as above).
[[nodiscard]] std::optional<std::span<const std::byte>> find_jpeg_exif(std::span<const std::byte> jpeg) noexcept;

// Empty strings and captured_unix_ms == 0 mean "not present".
struct ExifFacts {
    bool has_camera{};  // Make or Model present
    std::string make;
    std::string model;
    std::string content_identifier;
    std::string burst_uuid;
    std::int64_t captured_unix_ms{};
};

// tiff = EXIF TIFF block (starts with II*\0 or MM\0*). Reads IFD0 Make/Model, ExifIFD DateTimeOriginal,
// and Apple MakerNote ("Apple iOS\0", tags 0x0011 ContentIdentifier, 0x000B BurstUUID). Never reads out of bounds.
[[nodiscard]] ExifFacts parse_exif(std::span<const std::byte> tiff) noexcept;

}  // namespace pms
