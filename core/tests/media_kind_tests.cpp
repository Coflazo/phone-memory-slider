#include <algorithm>
#include <cstddef>
#include <vector>
#include "pms/media_kind.hpp"
#include "test_harness.hpp"

void run_media_kind_tests() {
    using namespace pms;
    pms_check(folder_class("DCIM/Camera/IMG_1.jpg") == FolderClass::Camera, "camera folder");
    pms_check(folder_class("Pictures/Screenshots/a.png") == FolderClass::Screenshots, "screenshots folder");
    pms_check(folder_class("Android/media/com.whatsapp/WhatsApp/Media/WhatsApp Images/x.jpg") == FolderClass::Messaging, "whatsapp");
    pms_check(folder_class("Download/x.jpg") == FolderClass::Downloads, "downloads");
    pms_check(folder_class("Pictures/Favourites/x.jpg") == FolderClass::Favorites, "british spelling");

    const std::vector<ScreenSize> screens{{1080, 2400}};
    MediaFacts shot{"DCIM/Camera/IMG_9.png", {MediaFormat::Png, false, false}, 1080, 2400, 0, false, false};
    pms_check(classify(shot, screens) == MediaKind::Screenshot, "PNG at exact screen size without camera EXIF");
    shot.has_camera_exif = true;
    pms_check(classify(shot, screens) == MediaKind::Photo, "camera EXIF wins over size match");
    MediaFacts gif{"Download/x.gif", {MediaFormat::Gif, true, false}, 400, 300, 0, false, false};
    pms_check(classify(gif, screens) == MediaKind::Animated, "animated GIF");
    MediaFacts rec{"Movies/Screen recordings/Screen_Recording_1.mp4", {MediaFormat::Mp4, false, true}, 1080, 2400, 12, false, false};
    pms_check(classify(rec, screens) == MediaKind::ScreenRecording, "screen recording");
    MediaFacts motion{"DCIM/Camera/PXL_1.MP.jpg", {MediaFormat::Jpeg, false, false}, 4000, 3000, 0, true, true};
    pms_check(classify(motion, screens) == MediaKind::MotionPhoto, "motion photo");
    MediaFacts dng{"DCIM/Camera/x.dng", {MediaFormat::Tiff, false, false}, 4000, 3000, 0, true, false};
    pms_check(classify(dng, screens) == MediaKind::Raw && !auto_delete_eligible(MediaKind::Raw), "RAW never auto-queued");

    const std::vector<GroupingInput> items{
        {"DCIM/100APPLE/IMG_1234.HEIC", MediaFormat::Heif, 1000, "CID-1", ""},
        {"DCIM/100APPLE/IMG_1234.MOV", MediaFormat::QuickTime, 1100, "CID-1", ""},
        {"DCIM/100APPLE/IMG_1234.AAE", MediaFormat::Unknown, 0, "", ""},
        {"DCIM/100APPLE/IMG_E1234.HEIC", MediaFormat::Heif, 5000, "", ""},
        {"DCIM/100APPLE/IMG_2000.HEIC", MediaFormat::Heif, 9000, "", "B1"},
        {"DCIM/100APPLE/IMG_2001.HEIC", MediaFormat::Heif, 9100, "", "B1"},
        {"DCIM/100APPLE/IMG_3000.JPG", MediaFormat::Jpeg, 0, "", ""},
        {"DCIM/100APPLE/IMG_3000.MOV", MediaFormat::QuickTime, 0, "", ""},
    };
    const auto g = group_media(items);
    auto unit_of = [&](std::size_t i) {
        for (const auto& u : g.units) for (auto x : u) if (x == i) return u;
        return std::vector<std::size_t>{};
    };
    const auto live = unit_of(0);
    pms_check(live.size() == 4 && live.front() == 0, "HEIC + MOV + AAE + edited render delete together, still shown");
    pms_check(unit_of(6).size() == 2, "stem pairing when identifiers are absent");
    pms_check(g.bursts.size() == 1 && g.bursts[0].size() == 2, "BurstUUID groups a burst");

    // Beyond the brief: the edges of each rule.
    pms_check(folder_class("Pictures/x.jpg") == FolderClass::Other, "plain folder is Other");
    pms_check(folder_class("Screenshots.png") == FolderClass::Other && folder_class("Camera/DCIM/x.jpg") == FolderClass::Other,
              "file name is not a folder, and Camera must sit directly under DCIM");
    pms_check(folder_class("dcim/CAMERA/x.jpg") == FolderClass::Camera, "folder match ignores case");
    pms_check(folder_class("Download/Telegram/x.jpg") == FolderClass::Messaging, "deepest matching folder wins");

    const MediaFacts rotated{"Pictures/x.png", {MediaFormat::Png, false, false}, 2400, 1080, 0, false, false};
    pms_check(classify(rotated, screens) == MediaKind::Screenshot, "landscape screen size matches too");
    const MediaFacts jpeg_at_size{"Pictures/x.jpg", {MediaFormat::Jpeg, false, false}, 1080, 2400, 0, false, false};
    pms_check(classify(jpeg_at_size, screens) == MediaKind::Photo, "size match applies to PNG only");
    const MediaFacts named{"Pictures/screenshot_20240101.jpg", {MediaFormat::Jpeg, false, false}, 640, 480, 0, true, false};
    pms_check(classify(named, screens) == MediaKind::Screenshot, "Screenshot file name prefix, any case");
    const MediaFacts named_rec{"Movies/ScreenRecording_7.mp4", {MediaFormat::Mp4, false, true}, 1, 1, 3, false, false};
    pms_check(classify(named_rec, screens) == MediaKind::ScreenRecording, "screen recording by file name");
    const MediaFacts clip{"DCIM/Camera/VID_1.mp4", {MediaFormat::Mp4, false, true}, 1080, 2400, 5, true, false};
    pms_check(classify(clip, screens) == MediaKind::Video, "camera video");
    const MediaFacts aae{"DCIM/100APPLE/IMG_1.AAE", {}, 0, 0, 0, false, false};
    pms_check(classify(aae, screens) == MediaKind::Sidecar && !auto_delete_eligible(MediaKind::Sidecar), "AAE is a sidecar");
    const MediaFacts junk{"Download/notes.txt", {}, 0, 0, 0, false, false};
    pms_check(classify(junk, screens) == MediaKind::Unknown && !auto_delete_eligible(MediaKind::Unknown), "unsniffable is Unknown");
    pms_check(auto_delete_eligible(MediaKind::Photo) && auto_delete_eligible(MediaKind::Video), "ordinary media is eligible");

    const std::vector<GroupingInput> edges{
        {"A/IMG_4000.HEIC", MediaFormat::Heif, 10000, "", ""},
        {"A/IMG_4000.MOV", MediaFormat::QuickTime, 13001, "", ""},   // 3001 ms apart: not a pair
        {"A/img_5000.heic", MediaFormat::Heif, 20000, "", ""},
        {"A/IMG_5000.MOV", MediaFormat::QuickTime, 23000, "", ""},   // 3000 ms, stem case differs: pair
        {"A/IMG_6000.JPG", MediaFormat::Jpeg, 0, "X", ""},
        {"A/IMG_6000.MOV", MediaFormat::QuickTime, 0, "Y", ""},      // conflicting identifiers: not a pair
        {"A/IMG_7000.JPG", MediaFormat::Jpeg, 0, "", ""},
        {"B/IMG_7000.MOV", MediaFormat::QuickTime, 0, "", ""},       // other directory: not a pair
        {"B/IMG_O7000.AAE", MediaFormat::Unknown, 0, "", ""},        // no IMG_7000 image in B: stays alone
        {"A/IMG_O7000.AAE", MediaFormat::Unknown, 0, "", ""},        // joins A/IMG_7000.JPG
        {"C/IMG_8000.MOV", MediaFormat::QuickTime, 0, "Z", ""},
        {"D/renamed.jpg", MediaFormat::Jpeg, 0, "Z", "B2"},          // same identifier, any name: pair, still first
    };
    const auto e = group_media(edges);
    const auto unit_in = [&](std::size_t i) {
        for (const auto& u : e.units) if (std::ranges::find(u, i) != u.end()) return u;
        return std::vector<std::size_t>{};
    };
    std::vector<std::size_t> seen;
    for (const auto& u : e.units) seen.insert(seen.end(), u.begin(), u.end());
    std::ranges::sort(seen);
    pms_check(seen.size() == edges.size() && std::ranges::adjacent_find(seen) == seen.end(), "every item is in exactly one unit");
    pms_check(unit_in(0).size() == 1 && unit_in(1).size() == 1, "capture times over 3 s apart do not pair");
    pms_check(unit_in(2) == std::vector<std::size_t>{2, 3}, "stem match ignores case at exactly 3 s");
    pms_check(unit_in(4).size() == 1 && unit_in(5).size() == 1, "different identifiers never pair by stem");
    pms_check(unit_in(7).size() == 1 && unit_in(8).size() == 1, "pairing and sidecars stay within a directory");
    pms_check(unit_in(6) == std::vector<std::size_t>{6, 9}, "IMG_O sidecar joins the original image");
    pms_check(unit_in(10) == std::vector<std::size_t>{11, 10}, "identifier pairs across names, still shown first");
    pms_check(e.bursts.empty(), "a single-frame burst is not a burst");
}
