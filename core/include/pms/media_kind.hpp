#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pms/media_format.hpp"

namespace pms {

// classify() sees one file, so it never returns LivePhoto: the caller marks a group_media unit
// that holds a still and its motion file as LivePhoto.
enum class MediaKind : std::uint8_t { Photo, LivePhoto, MotionPhoto, Video, ScreenRecording, Screenshot, Animated, Raw, Sidecar, Unknown };
enum class FolderClass : std::uint8_t { Camera, Screenshots, Messaging, Downloads, Favorites, Other };

struct MediaFacts {
    std::string relative_path;          // '/'-separated path as the device shows it
    SniffResult sniff;
    std::uint32_t width{}, height{};
    double duration_s{};
    bool has_camera_exif{};
    bool motion_photo{};                // Motion Photo XMP found
};

struct ScreenSize { std::uint32_t width, height; };

// Folder segments only (never the file name), case-insensitive. Camera = DCIM as the first segment.
// Precedence: Screenshots/Screen recordings > Favorites > Messaging > Downloads > Camera > Other.
[[nodiscard]] FolderClass folder_class(std::string_view relative_path) noexcept;
[[nodiscard]] MediaKind classify(const MediaFacts& facts, std::span<const ScreenSize> screens) noexcept;
[[nodiscard]] bool auto_delete_eligible(MediaKind kind) noexcept;  // false for Raw, Sidecar, Unknown

// captured_unix_ms == 0 and empty strings mean "not known" (as in ExifFacts).
struct GroupingInput {
    std::string relative_path;
    MediaFormat format{};
    std::int64_t captured_unix_ms{};
    std::string content_identifier;
    std::string burst_uuid;
};

struct MediaGroups {
    std::vector<std::vector<std::size_t>> units;   // items deleted together; units[i][0] is the card shown
    std::vector<std::vector<std::size_t>> bursts;  // same BurstUUID, size >= 2
};

// Every item lands in exactly one unit. A Live Photo still (HEIF/JPEG) takes at most one motion file
// (QuickTime/MP4): first one with an equal content identifier and the same directory and stem, then one
// with an equal identifier under any name, then, when neither identifier contradicts, one with the same
// directory and case-insensitive stem whose capture time is unknown or within 3 s.
// IMG_1234.AAE, IMG_O1234.AAE and IMG_E1234.* join the unit of the original IMG_1234.* in their
// directory: the image, or the video when there is no image.
[[nodiscard]] MediaGroups group_media(std::span<const GroupingInput> items);

}  // namespace pms
