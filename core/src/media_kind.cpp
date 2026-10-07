#include "pms/media_kind.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace pms {
namespace {

using namespace std::string_view_literals;

constexpr std::int64_t live_pair_window_ms = 3000;

[[nodiscard]] constexpr char lower(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(const std::string_view a, const std::string_view b) noexcept {
    return std::ranges::equal(a, b, {}, lower, lower);
}

[[nodiscard]] bool istarts_with(const std::string_view text, const std::string_view prefix) noexcept {
    return iequals(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] std::string lowercase(const std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

struct PathParts {
    std::string_view dir;   // without the trailing '/'
    std::string_view name;
    std::string_view stem;  // name before its last '.'
    std::string_view ext;   // name after its last '.'
};

[[nodiscard]] PathParts split_path(const std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    return {slash == std::string_view::npos ? ""sv : path.substr(0, slash), name, name.substr(0, dot),
            dot == std::string_view::npos ? ""sv : name.substr(dot + 1)};
}

constexpr std::pair<std::string_view, FolderClass> named_folders[] = {
    {"Screenshots", FolderClass::Screenshots}, {"Screen recordings", FolderClass::Screenshots},
    {"WhatsApp", FolderClass::Messaging},      {"Telegram", FolderClass::Messaging},
    {"Signal", FolderClass::Messaging},        {"Messenger", FolderClass::Messaging},
    {"Download", FolderClass::Downloads},      {"Downloads", FolderClass::Downloads},
    {"Favorites", FolderClass::Favorites},     {"Favourites", FolderClass::Favorites},
};

[[nodiscard]] constexpr bool is_image(const MediaFormat format) noexcept {
    switch (format) {
        case MediaFormat::Jpeg:
        case MediaFormat::Png:
        case MediaFormat::Gif:
        case MediaFormat::WebP:
        case MediaFormat::Heif:
        case MediaFormat::Avif:
        case MediaFormat::Tiff:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool live_still(const MediaFormat format) noexcept {
    return format == MediaFormat::Heif || format == MediaFormat::Jpeg;
}

[[nodiscard]] constexpr bool live_motion(const MediaFormat format) noexcept {
    return format == MediaFormat::QuickTime || format == MediaFormat::Mp4;
}

// A same-name still and motion file are one Live Photo unless their identifiers disagree or both
// capture times are known and more than 3 s apart.
[[nodiscard]] bool same_name_live_pair(const GroupingInput& still, const GroupingInput& motion) noexcept {
    const auto& a = still.content_identifier;
    const auto& b = motion.content_identifier;
    if (!a.empty() && !b.empty() && a != b) {
        return false;
    }
    if (still.captured_unix_ms == 0 || motion.captured_unix_ms == 0) {
        return true;
    }
    // Unsigned difference: no overflow whatever the inputs.
    const auto [lo, hi] = std::minmax(still.captured_unix_ms, motion.captured_unix_ms);
    return static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) <= std::uint64_t{live_pair_window_ms};
}

// IMG_E1234 (edited render) and IMG_O1234 (original adjustments) belong to IMG_1234. Input is lowercase.
[[nodiscard]] std::string base_stem(std::string stem) {
    const auto digits = std::string_view{stem}.substr(std::min<std::size_t>(5, stem.size()));
    if (stem.starts_with("img_") && stem.size() > 5 && (stem[4] == 'e' || stem[4] == 'o') &&
        std::ranges::all_of(digits, [](const char c) noexcept { return c >= '0' && c <= '9'; })) {
        stem.erase(4, 1);
    }
    return stem;
}

}  // namespace

FolderClass folder_class(const std::string_view relative_path) noexcept {
    auto rest = split_path(relative_path).dir;
    auto found = FolderClass::Other;
    std::string_view parent;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto folder = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? ""sv : rest.substr(slash + 1);
        if (iequals(parent, "DCIM") && iequals(folder, "Camera")) {
            found = FolderClass::Camera;
        }
        for (const auto& [name, named_class] : named_folders) {
            if (iequals(folder, name)) {
                found = named_class;
            }
        }
        parent = folder;
    }
    return found;
}

MediaKind classify(const MediaFacts& facts, const std::span<const ScreenSize> screens) noexcept {
    const auto path = split_path(facts.relative_path);
    const bool screenshots_folder = folder_class(facts.relative_path) == FolderClass::Screenshots;
    const auto& sniff = facts.sniff;
    if (sniff.video) {
        const bool recording = screenshots_folder || istarts_with(path.name, "Screen_Recording") ||
                               istarts_with(path.name, "Screenrecorder") || istarts_with(path.name, "ScreenRecording");
        return recording ? MediaKind::ScreenRecording : MediaKind::Video;
    }
    if (sniff.format == MediaFormat::Unknown) {
        return iequals(path.ext, "aae") ? MediaKind::Sidecar : MediaKind::Unknown;
    }
    if (sniff.format == MediaFormat::Tiff) {
        return MediaKind::Raw;  // DNG and other TIFF-based RAW
    }
    if (sniff.animated) {
        return MediaKind::Animated;
    }
    if (facts.motion_photo) {
        return MediaKind::MotionPhoto;
    }
    const bool screen_sized = sniff.format == MediaFormat::Png && !facts.has_camera_exif &&
                              std::ranges::any_of(screens, [&facts](const ScreenSize screen) noexcept {
                                  return (facts.width == screen.width && facts.height == screen.height) ||
                                         (facts.width == screen.height && facts.height == screen.width);
                              });
    if (screenshots_folder || istarts_with(path.name, "Screenshot") || screen_sized) {
        return MediaKind::Screenshot;
    }
    return MediaKind::Photo;
}

bool auto_delete_eligible(const MediaKind kind) noexcept {
    return kind != MediaKind::Raw && kind != MediaKind::Sidecar && kind != MediaKind::Unknown;
}

MediaGroups group_media(const std::span<const GroupingInput> items) {
    const auto n = items.size();
    std::vector<std::string> keys(n);       // lowercase "dir/stem"
    std::vector<std::string> base_keys(n);  // lowercase "dir/IMG_1234" for IMG_E1234 and IMG_O1234, else keys[i]
    std::vector<bool> sidecar(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto path = split_path(items[i].relative_path);
        const auto dir = lowercase(path.dir) + '/';
        auto stem = lowercase(path.stem);
        keys[i] = dir + stem;
        base_keys[i] = dir + base_stem(std::move(stem));
        sidecar[i] = iequals(path.ext, "aae");
    }

    std::vector<std::size_t> parent(n);
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&parent](std::size_t i) {
        while (parent[i] != i) {
            i = parent[i] = parent[parent[i]];
        }
        return i;
    };
    const auto join = [&](const std::size_t keep, const std::size_t other) { parent[root(other)] = root(keep); };

    // Live Photos: each still takes at most one motion file, matched by identifier first, then by name.
    std::vector<bool> paired(n);
    const auto pair_live = [&](const std::size_t still, const std::size_t motion) {
        join(still, motion);
        paired[still] = true;
        paired[motion] = true;
    };
    std::unordered_map<std::string_view, std::size_t> still_by_id;
    for (std::size_t i = 0; i < n; ++i) {
        if (live_still(items[i].format) && !items[i].content_identifier.empty()) {
            still_by_id.try_emplace(items[i].content_identifier, i);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!live_motion(items[i].format) || items[i].content_identifier.empty()) {
            continue;
        }
        if (const auto it = still_by_id.find(items[i].content_identifier); it != still_by_id.end() && !paired[it->second]) {
            pair_live(it->second, i);
        }
    }
    std::unordered_map<std::string_view, std::vector<std::size_t>> stills_by_key;
    for (std::size_t i = 0; i < n; ++i) {
        if (live_still(items[i].format) && !paired[i]) {
            stills_by_key[keys[i]].push_back(i);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = live_motion(items[i].format) && !paired[i] ? stills_by_key.find(keys[i]) : stills_by_key.end();
        if (it == stills_by_key.end()) {
            continue;
        }
        for (const auto still : it->second) {
            if (!paired[still] && same_name_live_pair(items[still], items[i])) {
                pair_live(still, i);
                break;
            }
        }
    }

    // Edits and sidecars follow the original image IMG_1234.* in their directory.
    std::unordered_map<std::string_view, std::size_t> original_by_key;
    for (std::size_t i = 0; i < n; ++i) {
        if (is_image(items[i].format) && base_keys[i] == keys[i]) {
            original_by_key.try_emplace(keys[i], i);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (sidecar[i] || base_keys[i] != keys[i]) {
            if (const auto it = original_by_key.find(base_keys[i]); it != original_by_key.end()) {
                join(it->second, i);
            }
        }
    }

    // Units in order of first item; the card is an original image, then any image, then the rest.
    const auto rank = [&](const std::size_t i) {
        if (sidecar[i]) {
            return 3;
        }
        if (!is_image(items[i].format)) {
            return 2;
        }
        return base_keys[i] == keys[i] ? 0 : 1;
    };
    MediaGroups groups;
    std::vector<std::size_t> unit_at(n, n);  // root -> unit index, n = none yet
    for (std::size_t i = 0; i < n; ++i) {
        const auto r = root(i);
        if (unit_at[r] == n) {
            unit_at[r] = groups.units.size();
            groups.units.emplace_back();
        }
        groups.units[unit_at[r]].push_back(i);
    }
    for (auto& unit : groups.units) {
        std::ranges::stable_sort(unit, {}, rank);
    }

    std::unordered_map<std::string_view, std::size_t> burst_at;
    for (std::size_t i = 0; i < n; ++i) {
        if (items[i].burst_uuid.empty()) {
            continue;
        }
        const auto [it, added] = burst_at.try_emplace(items[i].burst_uuid, groups.bursts.size());
        if (added) {
            groups.bursts.emplace_back();
        }
        groups.bursts[it->second].push_back(i);
    }
    std::erase_if(groups.bursts, [](const std::vector<std::size_t>& burst) { return burst.size() < 2; });
    return groups;
}

}  // namespace pms
