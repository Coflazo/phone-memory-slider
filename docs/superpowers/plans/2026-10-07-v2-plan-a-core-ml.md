# Plan A: v2 core ML, media understanding and evaluation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace v1's 16-number hand-made "ML" with a measured pipeline: real media typing, SigLIP2 embeddings on the user's GPU, a hand-written positive-unlabeled taste model, an evidence-based delete model, exact near-duplicate search, multi-label categories, an active-learning deck policy, an honest ETA, and an evaluation harness that proves it on a self-built phone-shaped gallery.

**Architecture:** `core/` stays pure C++23 with no third-party runtime dependency (plus vendored PDQ). A new `engine/` library holds the parts that touch real files and the GPU: decoders (Qt image I/O, libheif, Qt's FFmpeg) and the ONNX Runtime image encoder. `tools/` holds the dev-only gallery builder, tag-table builder and the `pms-eval` CLI that runs the shipped C++ code end to end.

**Tech Stack:** C++23 (MSVC 14.43 / clang / gcc), CMake 3.24+, Qt 6.8.3 (Core/Gui only in `engine/`), libheif 1.23.6 (LGPL, DLL), Qt's FFmpeg 7.1 (LGPL, DLL), ONNX Runtime 1.30 + WebGPU plugin EP 0.4.0 (MIT), SigLIP2 B/32-256 fp32 (Apache-2.0), PDQ (BSD, vendored), Python 3.12 dev venv for tools only.

**Spec:** `C:\Users\Cagan\.claude\plans\okey-i-want-you-dapper-thacker.md` (approved master plan) and `docs/spikes.md` (measured decisions).

## Global Constraints

- No code path in `core/`, `engine/` or the app may open a socket or any URL. FFmpeg inputs always set `protocol_whitelist=file`.
- The shipped encoder is `siglip2-base-patch32-256` `vision_model.onnx` fp32, verified by SHA-256 at load. No fp16 or int8 encoder (spike 4: cosine 0.55-0.76).
- WebGPU EP gets exactly one device: the one with the smallest `DxgiHighPerformanceIndex`; CPU EP fallback. Never register the Azure EP.
- Favorites, seeds and explicit keeps are a hard filter, never a model feature, and never appear in a delete queue.
- Every per-item failure is recorded and surfaced; one bad file never aborts a run.
- Embeddings are stored as IEEE fp16, computed in fp32, L2-normalized.
- All randomness takes an explicit `std::uint64_t seed`; same inputs and seed give the same output.
- C++: `/W4 /WX` (MSVC) or `-Wall -Wextra -Wpedantic -Werror`; `[[nodiscard]]`, `noexcept` where true, no raw `new`, spans for views.
- Tests use the existing `pms_check(condition, message)` harness (`core/tests/test_harness.hpp`) and one `run_*_tests()` function per file registered in `core/tests/test_main.cpp`.
- Commits authored as Coflazo, conventional prefixes (`feat:`, `test:`, `fix:`), no AI trailers.

## Review Focus

1. A file whose extension lies (a PNG named `.jpg`, an MP4 named `.heic`) is typed by its bytes, and a truncated or zero-length file is `Unknown`, not a crash. Pinned in Task 1.
2. A gallery with zero favorites and zero seeds still produces a ranked deck from evidence alone (taste weight is neutral), not a crash or an all-equal score. Pinned in Task 8.
3. A gallery of 250,000 items with many identical flat images (sky, black screenshots) finishes duplicate search in bounded time and never chains unrelated photos into one cluster. Pinned in Task 9.
4. A stalled transfer (rate drops to zero) shows "Estimating..." again rather than a frozen or negative ETA. Pinned in Task 12.
5. A corrupted, truncated or decompression-bomb image (huge declared dimensions) is rejected before allocation and reported as unreadable. Pinned in Task 13.

---

## File structure

```
core/include/pms/media_format.hpp   core/src/media_format.cpp    byte sniffing
core/include/pms/media_meta.hpp     core/src/media_meta.cpp      XMP Motion Photo, Apple MakerNote, EXIF camera presence
core/include/pms/media_kind.hpp     core/src/media_kind.cpp      kind, folder class, deletion units, bursts
core/include/pms/quality.hpp        core/src/quality.cpp         blur, exposure, noise
core/include/pms/pdq.hpp            core/src/pdq.cpp             wrapper over third_party/pdq
core/include/pms/embeddings.hpp     core/src/embeddings.cpp      fp16 matrix, dot products
core/include/pms/taste.hpp          core/src/taste.cpp           L2-logistic P-vs-U head, L-BFGS
core/include/pms/delete_model.hpp   core/src/delete_model.cpp    8-feature MAP logistic
core/include/pms/duplicates.hpp     core/src/duplicates.cpp      exact, burst, PDQ near-dup, star clusters
core/include/pms/tags.hpp           core/src/tags.cpp            tag table, multi-label tags, category ranking
core/include/pms/deck_policy.hpp    core/src/deck_policy.cpp     likely-delete / help-me-learn / exploration cards
core/include/pms/eta.hpp            core/src/eta.cpp             bottleneck ETA with hysteresis + text
core/tests/<name>_tests.cpp                                       one file per header
third_party/pdq/                                                  vendored PDQ (BSD) at a pinned commit
engine/include/pms/engine/decode.hpp engine/src/decode.cpp       256 px RGB proxies from files
engine/include/pms/engine/encoder.hpp engine/src/encoder.cpp     ONNX Runtime SigLIP2 encoder
engine/tests/                                                     decode + encoder tests (Qt Test-free, pms_check)
cmake/Dependencies.cmake                                          pinned ORT, WebGPU EP, libheif, Qt-FFmpeg SDK, model
models/manifest.json                                              model + tag table: url, sha256, license, bytes
tools/make_test_gallery.py  tools/build_tag_table.py  tools/pms-eval.cpp  tools/eval_report.py
```

---

### Task 1: Media format sniffing

**Files:**
- Create: `core/include/pms/media_format.hpp`, `core/src/media_format.cpp`
- Test: `core/tests/media_format_tests.cpp`
- Modify: `core/CMakeLists.txt` (add source), `core/tests/CMakeLists.txt` + `core/tests/test_main.cpp` (register `run_media_format_tests`)

**Interfaces:**
- Produces:
```cpp
namespace pms {
enum class MediaFormat : std::uint8_t { Unknown, Jpeg, Png, Gif, WebP, Heif, Avif, Tiff, Mp4, QuickTime, ThreeGp, Matroska, WebM, Avi };
struct SniffResult { MediaFormat format{MediaFormat::Unknown}; bool animated{}; bool video{}; };
inline constexpr std::size_t sniff_bytes = 65536;   // callers pass up to this many leading bytes
[[nodiscard]] SniffResult sniff_media(std::span<const std::byte> head) noexcept;
[[nodiscard]] std::string_view to_string(MediaFormat) noexcept;
}
```

Rules (all bounds-checked, never read past `head.size()`):
- JPEG `FF D8 FF`. PNG `89 50 4E 47 0D 0A 1A 0A`; animated if an `acTL` chunk appears before the first `IDAT` (walk chunks: 4-byte BE length, 4-byte type, skip length + 4 CRC).
- GIF `GIF87a`/`GIF89a`; animated if `NETSCAPE2.0` appears in head or a second image descriptor (`0x2C`) is found while walking blocks.
- WebP `RIFF....WEBP`; animated if chunk `VP8X` flags byte has bit `0x02`.
- ISO-BMFF: bytes 4..7 == `ftyp`; major brand and compatible brands decide: `heic heix hevc heim heis mif1 msf1 hevx` -> Heif (`msf1`/`hevs`/`hevm` sequence brands -> animated); `avif avis` -> Avif (`avis` animated); `qt  ` -> QuickTime (video); `3gp4 3gp5 3gp6 3g2a` -> ThreeGp (video); `isom iso2 mp41 mp42 avc1 dash M4V ` -> Mp4 (video).
- Matroska/WebM EBML `1A 45 DF A3`; DocType `webm` -> WebM else Matroska (video). AVI `RIFF....AVI ` (video). TIFF `II*\0` or `MM\0*` (DNG and most RAW are TIFF-based).

- [ ] **Step 1: Write the failing test**

```cpp
#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include "pms/media_format.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}
std::vector<std::byte> ascii(std::string_view s) {
    std::vector<std::byte> out;
    for (char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}
void append(std::vector<std::byte>& a, const std::vector<std::byte>& b) { a.insert(a.end(), b.begin(), b.end()); }
std::vector<std::byte> be32(std::uint32_t v) { return bytes({int(v >> 24), int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)}); }
std::vector<std::byte> ftyp(std::string_view major, std::initializer_list<std::string_view> compat) {
    std::vector<std::byte> box = ascii("ftyp");
    append(box, ascii(major));
    append(box, bytes({0, 0, 0, 0}));
    for (auto c : compat) append(box, ascii(c));
    std::vector<std::byte> out = be32(static_cast<std::uint32_t>(box.size() + 4));
    append(out, box);
    return out;
}
}  // namespace

void run_media_format_tests() {
    using pms::MediaFormat;
    pms_check(pms::sniff_media({}).format == MediaFormat::Unknown, "empty input is Unknown");
    pms_check(pms::sniff_media(bytes({0xFF, 0xD8})).format == MediaFormat::Unknown, "truncated JPEG magic is Unknown");
    pms_check(pms::sniff_media(bytes({0xFF, 0xD8, 0xFF, 0xE1})).format == MediaFormat::Jpeg, "JPEG magic");

    auto png = bytes({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    auto still = png;
    append(still, be32(0)); append(still, ascii("IDAT")); append(still, be32(0));
    pms_check(!pms::sniff_media(still).animated, "PNG without acTL is still");
    auto apng = png;
    append(apng, be32(8)); append(apng, ascii("acTL")); append(apng, bytes({0,0,0,2,0,0,0,0})); append(apng, be32(0));
    append(apng, be32(0)); append(apng, ascii("IDAT")); append(apng, be32(0));
    const auto a = pms::sniff_media(apng);
    pms_check(a.format == MediaFormat::Png && a.animated, "APNG is an animated PNG");

    auto gif = ascii("GIF89a");
    append(gif, bytes({1, 0, 1, 0, 0, 0, 0}));
    append(gif, ascii("!\xFF\x0BNETSCAPE2.0"));
    pms_check(pms::sniff_media(gif).animated, "GIF with NETSCAPE loop block is animated");

    auto webp = ascii("RIFF"); append(webp, bytes({30, 0, 0, 0})); append(webp, ascii("WEBPVP8X"));
    append(webp, bytes({10, 0, 0, 0, 0x02, 0, 0, 0}));
    pms_check(pms::sniff_media(webp).animated, "WebP VP8X animation flag");

    pms_check(pms::sniff_media(ftyp("heic", {"mif1", "heic"})).format == MediaFormat::Heif, "HEIC brand");
    pms_check(pms::sniff_media(ftyp("msf1", {"msf1", "hevc"})).animated, "HEIF image sequence is animated");
    pms_check(pms::sniff_media(ftyp("avif", {"avif", "mif1"})).format == MediaFormat::Avif, "AVIF brand");
    const auto mov = pms::sniff_media(ftyp("qt  ", {"qt  "}));
    pms_check(mov.format == MediaFormat::QuickTime && mov.video, "QuickTime is video");
    const auto mp4 = pms::sniff_media(ftyp("isom", {"isom", "avc1"}));
    pms_check(mp4.format == MediaFormat::Mp4 && mp4.video, "MP4 is video");

    // A huge box length must not be trusted.
    auto liar = ftyp("heic", {"mif1"});
    liar[0] = std::byte{0x7F};
    pms_check(pms::sniff_media(liar).format == MediaFormat::Heif, "ftyp with oversized length still sniffs safely");

    pms_check(pms::sniff_media(bytes({0x1A, 0x45, 0xDF, 0xA3})).video, "EBML is video");
    pms_check(pms::sniff_media(ascii("II*\0")).format == MediaFormat::Tiff, "little-endian TIFF/DNG");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-v2 --config Release --target pms_core_tests && build-v2/core/tests/Release/pms_core_tests.exe`
Expected: compile error, `pms/media_format.hpp` not found.

- [ ] **Step 3: Implement** `core/src/media_format.cpp` per the rules above. Helpers: `bool starts_with(span, initializer_list<uint8_t>)`, `uint32_t be32_at(span, size_t)` returning 0 when out of range, `bool contains(span, string_view)` bounded to `head`. ISO-BMFF: read the ftyp box size, clamp it to `head.size()`, collect brands at offsets 8 (major) and 16.. step 4 (compatible).

- [ ] **Step 4: Run test to verify it passes** (same command). Expected: `All core tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/pms/media_format.hpp core/src/media_format.cpp core/tests/media_format_tests.cpp core/CMakeLists.txt core/tests/CMakeLists.txt core/tests/test_main.cpp
git commit -m "feat(core): sniff media formats from bytes, not extensions"
```

---

### Task 2: Media metadata parsers (Motion Photo XMP, Apple MakerNote, camera EXIF)

**Files:**
- Create: `core/include/pms/media_meta.hpp`, `core/src/media_meta.cpp`
- Test: `core/tests/media_meta_tests.cpp` (+ register)

**Interfaces:**
- Produces:
```cpp
namespace pms {
struct MotionPhotoInfo { std::uint64_t video_length{}; std::int64_t presentation_us{-1}; };
// Google Motion Photo v1 (Camera:MotionPhoto + Container:Directory) and legacy MicroVideo/MicroVideoOffset.
[[nodiscard]] std::optional<MotionPhotoInfo> parse_motion_photo_xmp(std::string_view xmp) noexcept;
// Finds the APP1 "http://ns.adobe.com/xap/1.0/\0" XMP packet in a JPEG prefix.
[[nodiscard]] std::optional<std::string_view> find_jpeg_xmp(std::span<const std::byte> jpeg) noexcept;
// Finds the APP1 "Exif\0\0" TIFF block in a JPEG prefix.
[[nodiscard]] std::optional<std::span<const std::byte>> find_jpeg_exif(std::span<const std::byte> jpeg) noexcept;
struct ExifFacts { bool has_camera{}; std::string make; std::string model; std::string content_identifier; std::string burst_uuid; std::int64_t captured_unix_ms{}; };
// tiff = EXIF TIFF block (starts with II*\0 or MM\0*). Reads IFD0 Make/Model, ExifIFD DateTimeOriginal,
// and Apple MakerNote ("Apple iOS\0", tags 0x0011 ContentIdentifier, 0x000B BurstUUID). Never reads out of bounds.
[[nodiscard]] ExifFacts parse_exif(std::span<const std::byte> tiff) noexcept;
}
```
Bounds rules: every offset is validated against `tiff.size()`; IFD entry counts above 512 are rejected; ASCII values are capped at 256 bytes; MakerNote offsets are relative to the MakerNote start (Apple layout: 14-byte header `Apple iOS\0` + version `00 01` + `MM`, IFD at +14).

- [ ] **Step 1: Write the failing test**

```cpp
#include <string>
#include <vector>
#include "pms/media_meta.hpp"
#include "test_harness.hpp"

namespace {
struct Tiff {  // tiny big-endian TIFF writer for tests
    std::vector<std::byte> b;
    void u8(int v) { b.push_back(static_cast<std::byte>(v)); }
    void u16(int v) { u8(v >> 8); u8(v & 255); }
    void u32(std::uint32_t v) { u16(int(v >> 16)); u16(int(v & 0xFFFF)); }
    void str(std::string_view s) { for (char c : s) u8(static_cast<unsigned char>(c)); }
    void put32(std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::byte>((v >> (24 - 8 * i)) & 255); }
};

std::vector<std::byte> apple_exif(std::string_view content_id, std::string_view burst) {
    Tiff t;
    t.str("MM"); t.u16(42); t.u32(8);
    // IFD0: Make, ExifIFD pointer
    t.u16(2);
    t.u16(0x010F); t.u16(2); t.u32(6); const auto make_off = t.b.size(); t.u32(0);
    t.u16(0x8769); t.u16(4); t.u32(1); const auto exif_ptr = t.b.size(); t.u32(0);
    t.u32(0);
    t.put32(make_off, static_cast<std::uint32_t>(t.b.size())); t.str("Apple"); t.u8(0);
    t.put32(exif_ptr, static_cast<std::uint32_t>(t.b.size()));
    // ExifIFD: MakerNote (UNDEFINED)
    t.u16(1);
    const std::uint32_t mn_len = 14 + 2 + 2 * 12 + 4 + 64 + 64;
    t.u16(0x927C); t.u16(7); t.u32(mn_len); const auto mn_ptr = t.b.size(); t.u32(0);
    t.u32(0);
    const auto mn = t.b.size();
    t.put32(mn_ptr, static_cast<std::uint32_t>(mn));
    t.str("Apple iOS"); t.u8(0); t.u16(1); t.str("MM");
    t.u16(2);
    t.u16(0x000B); t.u16(2); t.u32(static_cast<std::uint32_t>(burst.size() + 1)); const auto burst_off = t.b.size(); t.u32(0);
    t.u16(0x0011); t.u16(2); t.u32(static_cast<std::uint32_t>(content_id.size() + 1)); const auto cid_off = t.b.size(); t.u32(0);
    t.u32(0);
    t.put32(burst_off, static_cast<std::uint32_t>(t.b.size() - mn)); t.str(burst); t.u8(0);
    t.put32(cid_off, static_cast<std::uint32_t>(t.b.size() - mn)); t.str(content_id); t.u8(0);
    return t.b;
}
}  // namespace

void run_media_meta_tests() {
    const std::string xmp =
        R"(<x:xmpmeta><rdf:RDF><rdf:Description Camera:MotionPhoto="1" Camera:MotionPhotoVersion="1" )"
        R"(Camera:MotionPhotoPresentationTimestampUs="1234"><Container:Directory><rdf:Seq>)"
        R"(<rdf:li><Container:Item Item:Mime="image/jpeg" Item:Semantic="Primary" Item:Length="0"/></rdf:li>)"
        R"(<rdf:li><Container:Item Item:Mime="video/mp4" Item:Semantic="MotionPhoto" Item:Length="345678"/></rdf:li>)"
        R"(</rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>)";
    const auto mp = pms::parse_motion_photo_xmp(xmp);
    pms_check(mp && mp->video_length == 345678 && mp->presentation_us == 1234, "Motion Photo v1 directory");
    const auto legacy = pms::parse_motion_photo_xmp(R"(<x GCamera:MicroVideo="1" GCamera:MicroVideoOffset="99000"/>)");
    pms_check(legacy && legacy->video_length == 99000, "legacy MicroVideo offset");
    pms_check(!pms::parse_motion_photo_xmp(R"(<x Camera:MotionPhoto="0"/>)"), "MotionPhoto=0 is not a motion photo");
    pms_check(!pms::parse_motion_photo_xmp(R"(<x Item:Length="18446744073709551616" Camera:MotionPhoto="1"/>)"),
              "overflowing length is rejected");

    const auto exif = apple_exif("6F2A1C0E-AAAA-BBBB-CCCC-123456789ABC", "BURST-1");
    const auto facts = pms::parse_exif(exif);
    pms_check(facts.has_camera && facts.make == "Apple", "Make tag read from IFD0");
    pms_check(facts.content_identifier == "6F2A1C0E-AAAA-BBBB-CCCC-123456789ABC", "Apple ContentIdentifier");
    pms_check(facts.burst_uuid == "BURST-1", "Apple BurstUUID");

    auto broken = exif;
    broken.resize(broken.size() / 2);
    const auto partial = pms::parse_exif(broken);
    pms_check(partial.content_identifier.empty(), "truncated MakerNote yields no identifier, no crash");
    pms_check(!pms::parse_exif({}).has_camera, "empty EXIF");
}
```

- [ ] **Step 2: Run test to verify it fails** (`pms_core_tests`): compile error, header missing.
- [ ] **Step 3: Implement.** XMP: attribute scan with `std::string_view::find` for `Camera:MotionPhoto="1"` or `GCamera:MicroVideo="1"`; video length = the `Item:Length` of the item whose `Item:Semantic="MotionPhoto"`, else `GCamera:MicroVideoOffset`; parse integers with `std::from_chars` and reject on error/overflow. EXIF: a `Reader` struct with `endian`, `u16(off)`, `u32(off)` returning `std::optional`; walk IFD0 tags 0x010F/0x0110/0x8769; in ExifIFD read 0x9003 DateTimeOriginal (`YYYY:MM:DD HH:MM:SS`, interpret as UTC when no offset tag) and 0x927C MakerNote; Apple MakerNote IFD at MakerNote+14 with its own `MM` endianness.
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `feat(core): parse Motion Photo XMP and Apple MakerNote ids safely`.

---

### Task 3: Media kinds, folder classes, deletion units and bursts

**Files:**
- Create: `core/include/pms/media_kind.hpp`, `core/src/media_kind.cpp`
- Test: `core/tests/media_kind_tests.cpp` (+ register)

**Interfaces:**
- Consumes: `SniffResult`, `MediaFormat` (Task 1), `ExifFacts` (Task 2).
- Produces:
```cpp
namespace pms {
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
[[nodiscard]] FolderClass folder_class(std::string_view relative_path) noexcept;
[[nodiscard]] MediaKind classify(const MediaFacts& facts, std::span<const ScreenSize> screens) noexcept;
[[nodiscard]] bool auto_delete_eligible(MediaKind) noexcept;  // false for Raw, Sidecar, Unknown
struct GroupingInput {
    std::string relative_path; MediaFormat format{}; std::int64_t captured_unix_ms{};
    std::string content_identifier; std::string burst_uuid;
};
struct MediaGroups {
    std::vector<std::vector<std::size_t>> units;   // items deleted together; units[i][0] is the card shown
    std::vector<std::vector<std::size_t>> bursts;  // same BurstUUID, size >= 2
};
[[nodiscard]] MediaGroups group_media(std::span<const GroupingInput> items);
}
```
Rules: folder class matches path segments case-insensitively (`DCIM/Camera` -> Camera; any segment `Screenshots`/`Screen recordings` -> Screenshots; `WhatsApp`, `Telegram`, `Signal`, `Messenger` -> Messaging; `Download`/`Downloads` -> Downloads; `Favorites`/`Favourites` -> Favorites). Screenshot = still image AND (folder Screenshots OR filename starts with `Screenshot`) OR (no camera EXIF AND PNG AND exact match to a known screen size, either orientation). Screen recording = video AND (Screenshots folder OR filename starts with `Screen_Recording`/`Screenrecorder`/`ScreenRecording`). Live pair: QuickTime/Mp4 item and Heif/Jpeg item with equal non-empty `content_identifier`; otherwise same directory + same case-insensitive stem + both timestamps known and within 3,000 ms (or one unknown). AAE: `IMG_1234.AAE` and `IMG_O1234.AAE` join the unit of `IMG_1234.<still>`; `IMG_E1234.<ext>` (edited render) joins the same unit.

- [ ] **Step 1: Write the failing test**

```cpp
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
}
```

- [ ] **Step 2-4:** fail (missing header) -> implement with `std::unordered_map` keyed by lowercase `dir/stem` and by content identifier -> pass.
- [ ] **Step 5: Commit** `feat(core): classify media kinds and keep Live Photos and sidecars together`.

---

### Task 4: Image quality metrics

**Files:** Create `core/include/pms/quality.hpp`, `core/src/quality.cpp`; Test `core/tests/quality_tests.cpp` (+ register)

**Interfaces:**
- Produces:
```cpp
namespace pms {
struct GrayView { const std::uint8_t* pixels{}; std::uint32_t width{}, height{}, stride{}; };
struct Quality {
    float sharpness{};        // max over a 4x4 tile grid of variance of the 3x3 Laplacian (bokeh-safe)
    float clipped_fraction{}; // share of pixels <= 2 or >= 253
    float luma_p1{}, luma_p99{};
    float noise_sigma{};      // Immerkaer (1996) estimator
};
[[nodiscard]] Quality measure_quality(GrayView image) noexcept;  // returns zeros for images smaller than 8x8
}
```
Immerkaer: `sigma = sqrt(pi/2) / (6 (W-2)(H-2)) * sum |I * N|`, `N = [[1,-2,1],[-2,4,-2],[1,-2,1]]`.

- [ ] **Step 1: Write the failing test**

```cpp
#include <cmath>
#include <random>
#include <vector>
#include "pms/quality.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::uint8_t> checker(std::uint32_t w, std::uint32_t h, int cell) {
    std::vector<std::uint8_t> p(std::size_t{w} * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) p[y * w + x] = ((x / cell + y / cell) % 2) ? 220 : 30;
    return p;
}
std::vector<std::uint8_t> box_blur(const std::vector<std::uint8_t>& s, std::uint32_t w, std::uint32_t h, int r) {
    std::vector<std::uint8_t> d(s.size());
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            int sum = 0, n = 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int xx = int(x) + dx, yy = int(y) + dy;
                    if (xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h)) { sum += s[yy * w + xx]; ++n; }
                }
            d[y * w + x] = static_cast<std::uint8_t>(sum / n);
        }
    return d;
}
}  // namespace

void run_quality_tests() {
    const std::uint32_t w = 256, h = 192;
    const auto sharp = checker(w, h, 8);
    const auto soft = box_blur(sharp, w, h, 4);
    const auto qs = pms::measure_quality({sharp.data(), w, h, w});
    const auto qb = pms::measure_quality({soft.data(), w, h, w});
    pms_check(qs.sharpness > 4.0F * qb.sharpness, "blur lowers sharpness a lot");

    std::vector<std::uint8_t> flat(std::size_t{w} * h, 128);
    pms_check(pms::measure_quality({flat.data(), w, h, w}).noise_sigma < 0.01F, "flat image has no noise");
    std::mt19937 rng(1);
    std::normal_distribution<float> n(0.0F, 5.0F);
    for (auto& v : flat) v = static_cast<std::uint8_t>(std::clamp(128.0F + n(rng), 0.0F, 255.0F));
    const float sigma = pms::measure_quality({flat.data(), w, h, w}).noise_sigma;
    pms_check(sigma > 4.0F && sigma < 6.0F, "Immerkaer estimate tracks sigma=5");

    std::vector<std::uint8_t> white(std::size_t{w} * h, 255);
    pms_check(pms::measure_quality({white.data(), w, h, w}).clipped_fraction > 0.99F, "blown-out image is clipped");

    // Bokeh: sharp subject in one tile, blur elsewhere, still reads sharp.
    auto bokeh = soft;
    for (std::uint32_t y = 0; y < h / 4; ++y)
        for (std::uint32_t x = 0; x < w / 4; ++x) bokeh[y * w + x] = sharp[y * w + x];
    pms_check(pms::measure_quality({bokeh.data(), w, h, w}).sharpness > 0.5F * qs.sharpness, "max-tile sharpness ignores blurred background");
    std::uint8_t tiny[4] = {};
    pms_check(pms::measure_quality({tiny, 2, 2, 2}).sharpness == 0.0F, "tiny images return zeros");
}
```

- [ ] **Step 2-4:** fail -> implement (single pass Laplacian per tile with running mean/variance in double; histogram for percentiles) -> pass.
- [ ] **Step 5: Commit** `feat(core): measure blur, exposure and noise per image`.

---

### Task 5: PDQ perceptual hash (vendored)

**Files:**
- Create: `third_party/pdq/` with `LICENSE` (ThreatExchange BSD), `README.md` naming the source commit, and only: `common/pdqbasetypes.h`, `common/pdqhashtypes.{h,cpp}`, `common/pdqutils.{h,cpp}`, `downscaling/downscaling.{h,cpp}`, `hashing/pdqhashing.{h,cpp}`, `hashing/torben.{h,cpp}` (not the 900 KB `pdqhamming.cpp`; we use `std::popcount`). Fix include paths to be relative to `third_party/`.
- Create: `core/include/pms/pdq.hpp`, `core/src/pdq.cpp`; Test `core/tests/pdq_tests.cpp`
- Modify: `core/CMakeLists.txt` (add `third_party/pdq` sources, compile them without `/WX` via a separate OBJECT library `pms_pdq`), `THIRD_PARTY_NOTICES.md`

**Interfaces:**
```cpp
namespace pms {
struct PdqHash { std::array<std::uint64_t, 4> bits{}; int quality{}; };
struct RgbView { const std::uint8_t* pixels{}; std::uint32_t width{}, height{}, stride{}; };  // interleaved RGB8
[[nodiscard]] PdqHash pdq_hash(RgbView image);   // caller passes <= 512 px long side
[[nodiscard]] int hamming(const PdqHash& a, const PdqHash& b) noexcept;
}
```
Source pin: `gh api repos/facebook/ThreatExchange/commits/main --jq .sha` at vendoring time, written into `third_party/pdq/README.md` with the file list and their SHA-256.

- [ ] **Step 1: Failing test**

```cpp
#include <vector>
#include "pms/pdq.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::uint8_t> scene(std::uint32_t w, std::uint32_t h, int shift, int gain) {
    std::vector<std::uint8_t> p(std::size_t{w} * h * 3);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const int v = ((x + shift) * 7 + y * 3) % 200 + ((x / 32 + y / 32) % 2) * 40 + gain;
            for (int c = 0; c < 3; ++c) p[(y * w + x) * 3 + c] = static_cast<std::uint8_t>(std::clamp(v + c * 10, 0, 255));
        }
    return p;
}
}  // namespace

void run_pdq_tests() {
    const auto a = scene(320, 240, 0, 0), b = scene(320, 240, 0, 12), c = scene(320, 240, 97, 0);
    const auto ha = pms::pdq_hash({a.data(), 320, 240, 320 * 3});
    const auto hb = pms::pdq_hash({b.data(), 320, 240, 320 * 3});
    const auto hc = pms::pdq_hash({c.data(), 320, 240, 320 * 3});
    pms_check(pms::hamming(ha, ha) == 0, "identical images hash identically");
    pms_check(pms::hamming(ha, hb) <= 31, "brightness change stays a near duplicate");
    pms_check(pms::hamming(ha, hc) > 31, "a different scene is not a near duplicate");
    std::vector<std::uint8_t> flat(320 * 240 * 3, 90);
    pms_check(pms::pdq_hash({flat.data(), 320, 240, 960}).quality < 50, "flat images report low PDQ quality");
}
```
- [ ] **Step 2-4:** fail -> implement (RGB -> float luma `0.299R+0.587G+0.114B`, call `pdqHash256FromFloatLuma`, copy the 16 16-bit words into 4 `uint64_t`) -> pass.
- [ ] **Step 5: Commit** `feat(core): vendor PDQ and hash images for near-duplicate search`.

---

### Task 6: fp16 embedding matrix

**Files:** Create `core/include/pms/embeddings.hpp`, `core/src/embeddings.cpp`; Test `core/tests/embeddings_tests.cpp`

**Interfaces:**
```cpp
namespace pms {
[[nodiscard]] std::uint16_t half_from_float(float) noexcept;  // IEEE 754 binary16, round to nearest even
[[nodiscard]] float float_from_half(std::uint16_t) noexcept;
class EmbeddingMatrix {
public:
    explicit EmbeddingMatrix(std::size_t dim = 768);
    void append(std::span<const float> vector);         // L2-normalizes, then stores fp16; throws on dim mismatch or non-finite
    [[nodiscard]] std::size_t rows() const noexcept;
    [[nodiscard]] std::size_t dim() const noexcept;
    [[nodiscard]] float dot(std::size_t row, std::span<const float> query) const noexcept;
    [[nodiscard]] float cosine(std::size_t a, std::size_t b) const noexcept;
    void copy_row(std::size_t row, std::span<float> out) const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> raw() const noexcept;
    static EmbeddingMatrix from_raw(std::size_t dim, std::vector<std::uint16_t> halves);
};
}
```
- [ ] **Step 1: Failing test**

```cpp
#include <cmath>
#include <vector>
#include "pms/embeddings.hpp"
#include "test_harness.hpp"

void run_embeddings_tests() {
    for (float v : {0.0F, 1.0F, -2.5F, 65504.0F, 6.1035156e-05F, 0.333333F})
        pms_check(std::abs(pms::float_from_half(pms::half_from_float(v)) - v) <= std::abs(v) * 1e-3F + 1e-7F, "fp16 round trip");
    pms_check(std::isinf(pms::float_from_half(pms::half_from_float(1e9F))), "overflow becomes inf");
    pms::EmbeddingMatrix m(4);
    m.append(std::vector<float>{3, 0, 4, 0});
    m.append(std::vector<float>{0, 1, 0, 0});
    pms_check(m.rows() == 2, "two rows");
    pms_check(std::abs(m.dot(0, std::vector<float>{0.6F, 0, 0.8F, 0}) - 1.0F) < 1e-3F, "rows are unit length");
    pms_check(std::abs(m.cosine(0, 1)) < 1e-3F, "orthogonal rows");
    bool threw = false;
    try { m.append(std::vector<float>{1, 2}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "dimension mismatch throws");
    const auto copy = pms::EmbeddingMatrix::from_raw(4, {m.raw().begin(), m.raw().end()});
    pms_check(copy.rows() == 2 && std::abs(copy.cosine(0, 0) - 1.0F) < 1e-3F, "raw round trip");
}
```
- [ ] **Step 2-4:** fail -> implement (bit-exact conversion with subnormals; `dot` accumulates in float over halves) -> pass.
- [ ] **Step 5: Commit** `feat(core): store embeddings as normalized fp16`.

---

### Task 7: Taste head (L2-logistic, positive vs unlabeled, L-BFGS)

**Files:** Create `core/include/pms/taste.hpp`, `core/src/taste.cpp`; Test `core/tests/taste_tests.cpp`; Create `core/tests/fixtures/taste_reference.bin` and `tools/reference/taste_reference.py` (writes the fixture with scikit-learn).

**Interfaces:**
- Consumes: `EmbeddingMatrix` (Task 6).
- Produces:
```cpp
namespace pms {
struct TasteConfig { float l2{1.0F}; int max_iterations{200}; float tolerance{1e-6F}; std::size_t unlabeled_cap{8000}; std::uint64_t seed{0}; };
struct TasteModel {
    std::vector<float> weights; float bias{};
    [[nodiscard]] float logit(std::span<const float> x) const noexcept;
    [[nodiscard]] float logit(const EmbeddingMatrix& m, std::size_t row) const noexcept;
};
struct TasteReport { std::vector<float> loss; std::size_t positives{}, negatives{}, unlabeled_used{}; };
// Objective: sum_i logloss(y_i, w.x_i + b) + (l2/2)||w||^2  (equals scikit-learn LogisticRegression(C = 1/l2)).
// y = 1 for positives (favorites, seeds, swipe keeps); y = 0 for negatives (swipe deletes) and for a seeded
// subsample of at most unlabeled_cap unlabeled rows. Returns a zero model when there are no positives.
[[nodiscard]] TasteModel train_taste(const EmbeddingMatrix& x, std::span<const std::size_t> positives,
                                     std::span<const std::size_t> negatives, std::span<const std::size_t> unlabeled,
                                     const TasteConfig& config = {}, TasteReport* report = nullptr);
// For tests: objective value and gradient at (w, b) on dense float rows.
[[nodiscard]] double taste_objective(std::span<const float> rows, std::size_t dim, std::span<const float> y,
                                     std::span<const float> w, float b, float l2, std::span<float> grad_w, float& grad_b);
}
```
Optimizer: L-BFGS (history 10) with backtracking Armijo line search (`c1 = 1e-4`, halve step), stop when `||grad||_inf < tolerance * max(1, |f|)` or `max_iterations`. Rows are converted from fp16 once into a contiguous float buffer for the training call.

- [ ] **Step 1: Fixture script** `tools/reference/taste_reference.py`:

```python
"""Writes core/tests/fixtures/taste_reference.bin: X (200x16 float32), y (200 float32), sklearn w (16), b (1)."""
import pathlib, numpy as np
from sklearn.linear_model import LogisticRegression
rng = np.random.default_rng(0)
X = rng.normal(size=(200, 16)).astype(np.float32)
X /= np.linalg.norm(X, axis=1, keepdims=True)
true = rng.normal(size=16)
y = (X @ true + 0.3 * rng.normal(size=200) > 0.4).astype(np.float32)
lr = LogisticRegression(C=1.0, tol=1e-10, max_iter=10000).fit(X, y)
out = pathlib.Path(__file__).resolve().parents[2] / "core/tests/fixtures/taste_reference.bin"
out.parent.mkdir(parents=True, exist_ok=True)
np.concatenate([X.ravel(), y, lr.coef_.ravel().astype(np.float32), lr.intercept_.astype(np.float32)]).astype("<f4").tofile(out)
print(out, out.stat().st_size)
```
Run: `.tools/venv/Scripts/python.exe tools/reference/taste_reference.py` -> 13,668 bytes.

- [ ] **Step 2: Failing test**

```cpp
#include <cmath>
#include <fstream>
#include <random>
#include <vector>
#include "pms/taste.hpp"
#include "test_harness.hpp"

#ifndef PMS_TEST_FIXTURES
#error "PMS_TEST_FIXTURES must point at core/tests/fixtures"
#endif

void run_taste_tests() {
    // 1. Gradient check against central finite differences.
    std::mt19937 rng(3);
    std::normal_distribution<float> n;
    const std::size_t rows = 30, dim = 6;
    std::vector<float> x(rows * dim), y(rows), w(dim), g(dim);
    for (auto& v : x) v = n(rng);
    for (auto& v : y) v = n(rng) > 0 ? 1.0F : 0.0F;
    for (auto& v : w) v = 0.3F * n(rng);
    float b = 0.1F, gb = 0;
    (void)pms::taste_objective(x, dim, y, w, b, 1.0F, g, gb);
    for (std::size_t j = 0; j < dim; ++j) {
        auto wp = w, wm = w; wp[j] += 1e-3F; wm[j] -= 1e-3F;
        std::vector<float> tmp(dim); float tb = 0;
        const double num = (pms::taste_objective(x, dim, y, wp, b, 1.0F, tmp, tb) - pms::taste_objective(x, dim, y, wm, b, 1.0F, tmp, tb)) / 2e-3;
        pms_check(std::abs(num - g[j]) < 1e-2 * std::max(1.0, std::abs(num)), "analytic gradient matches finite differences");
    }

    // 2. Parity with scikit-learn on a fixed problem.
    std::ifstream f(std::string(PMS_TEST_FIXTURES) + "/taste_reference.bin", std::ios::binary);
    std::vector<float> blob(200 * 16 + 200 + 16 + 1);
    f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size() * sizeof(float)));
    pms_check(static_cast<bool>(f), "fixture readable");
    pms::EmbeddingMatrix m(16);
    std::vector<std::size_t> pos, neg;
    for (std::size_t i = 0; i < 200; ++i) {
        m.append(std::span<const float>(blob.data() + i * 16, 16));
        (blob[3200 + i] > 0.5F ? pos : neg).push_back(i);
    }
    pms::TasteReport report;
    const auto model = pms::train_taste(m, pos, neg, {}, {}, &report);
    for (std::size_t j = 0; j < 16; ++j)
        pms_check(std::abs(model.weights[j] - blob[3400 + j]) < 2e-2F, "weights match scikit-learn");
    pms_check(std::abs(model.bias - blob[3416]) < 2e-2F, "bias matches scikit-learn");
    pms_check(report.loss.size() >= 2 && report.loss.back() <= report.loss.front(), "loss decreases");

    // 3. No positives -> neutral model; unlabeled cap respected and seeded.
    const auto empty = pms::train_taste(m, {}, neg, {}, {});
    pms_check(std::abs(empty.logit(m, 0)) < 1e-6F, "no positives gives a neutral model");
    std::vector<std::size_t> unl(neg.begin(), neg.end());
    pms::TasteReport capped;
    (void)pms::train_taste(m, pos, {}, unl, pms::TasteConfig{.unlabeled_cap = 10}, &capped);
    pms_check(capped.unlabeled_used == 10, "unlabeled subsample respects the cap");
}
```
`core/tests/CMakeLists.txt`: `target_compile_definitions(pms_core_tests PRIVATE PMS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")`.

- [ ] **Step 3: Run, expect compile failure.**
- [ ] **Step 4: Implement.** Core loop (keep this shape, it is what MODEL.md will quote):

```cpp
// f(w,b) = sum_i [ log(1 + e^{z_i}) - y_i z_i ] + l2/2 |w|^2,  z_i = w.x_i + b
// grad_w = X^T (sigmoid(z) - y) + l2 w,  grad_b = sum_i (sigmoid(z_i) - y_i)
double taste_objective(std::span<const float> rows, std::size_t dim, std::span<const float> y,
                       std::span<const float> w, float b, float l2, std::span<float> grad_w, float& grad_b) {
    const std::size_t n = y.size();
    double f = 0.0, gb = 0.0;
    std::fill(grad_w.begin(), grad_w.end(), 0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        const float* xi = rows.data() + i * dim;
        double z = b;
        for (std::size_t j = 0; j < dim; ++j) z += double(w[j]) * xi[j];
        f += (z > 0 ? z + std::log1p(std::exp(-z)) : std::log1p(std::exp(z))) - y[i] * z;
        const double r = 1.0 / (1.0 + std::exp(-z)) - y[i];
        gb += r;
        for (std::size_t j = 0; j < dim; ++j) grad_w[j] += static_cast<float>(r * xi[j]);
    }
    for (std::size_t j = 0; j < dim; ++j) { f += 0.5 * l2 * double(w[j]) * w[j]; grad_w[j] += l2 * w[j]; }
    grad_b = static_cast<float>(gb);
    return f;
}
```
L-BFGS over the packed vector `[w..., b]` (two-loop recursion, `s`/`y` ring buffers), loss appended each iteration.
- [ ] **Step 5: Run tests, expect pass.** Also add a timing check in `tools/pms-benchmark.cpp`: 8,000 x 768 rows train in under 1.5 s Release on the dev laptop (print the number; not a unit test).
- [ ] **Step 6: Commit** `feat(core): train the taste head with L-BFGS, verified against scikit-learn`.

---

### Task 8: Delete model (8 features, MAP logistic around hand-set weights)

**Files:** Create `core/include/pms/delete_model.hpp`, `core/src/delete_model.cpp`; Test `core/tests/delete_model_tests.cpp`

**Interfaces:**
```cpp
namespace pms {
inline constexpr std::size_t delete_feature_count = 8;
enum DeleteFeature : std::size_t { NegTaste, BlurZ, ExposureZ, DupNonBest, ScreenshotAge, LogBytesZ, IsVideo, DocumentLike };
using DeleteFeatures = std::array<float, delete_feature_count>;
struct DeleteModel {
    std::array<float, delete_feature_count> beta{}; float bias{};
    [[nodiscard]] float logit(const DeleteFeatures& x) const noexcept;
    [[nodiscard]] float probability(const DeleteFeatures& x) const noexcept;
};
// Cold start: beta = {1.2, 0.8, 0.6, 2.0, 0.9, 0.5, 0.2, -1.0}, bias = -1.5 (documents lean keep; non-best duplicates lean delete).
[[nodiscard]] DeleteModel default_delete_model() noexcept;
// MAP fit: minimize sum logloss + 1/(2 sigma^2) |beta - prior.beta|^2 + 1/(2 sigma^2)(bias - prior.bias)^2 by Newton (9x9 solve).
[[nodiscard]] DeleteModel fit_delete_model(std::span<const DeleteFeatures> x, std::span<const std::uint8_t> deleted,
                                           const DeleteModel& prior, float prior_sigma = 1.0F);
// Standardization helpers: z-score with mean/std of the unlabeled set; std floor 1e-3.
struct ZScore { float mean{}, stddev{1.0F}; [[nodiscard]] float operator()(float v) const noexcept; };
[[nodiscard]] ZScore fit_zscore(std::span<const float> values) noexcept;
}
```
`NegTaste` is `-taste_logit` clipped to `[-8, 8]`; with no taste model it is 0 (neutral), so ranking falls back to evidence (Review Focus 2).

- [ ] **Step 1: Failing test**

```cpp
#include <cmath>
#include <random>
#include <vector>
#include "pms/delete_model.hpp"
#include "test_harness.hpp"

void run_delete_model_tests() {
    using namespace pms;
    const auto prior = default_delete_model();
    DeleteFeatures dup{}; dup[DupNonBest] = 1.0F;
    DeleteFeatures doc{}; doc[DocumentLike] = 1.0F;
    pms_check(prior.probability(dup) > prior.probability(DeleteFeatures{}), "non-best duplicates lean delete at cold start");
    pms_check(prior.probability(doc) < prior.probability(DeleteFeatures{}), "documents lean keep at cold start");

    // Zero labels: fit returns the prior exactly.
    const auto same = fit_delete_model({}, {}, prior);
    for (std::size_t j = 0; j < delete_feature_count; ++j) pms_check(std::abs(same.beta[j] - prior.beta[j]) < 1e-6F, "no data keeps the prior");

    // A user who keeps every video: labels pull IsVideo negative.
    std::vector<DeleteFeatures> x; std::vector<std::uint8_t> y;
    std::mt19937 rng(5);
    for (int i = 0; i < 200; ++i) {
        DeleteFeatures f{}; f[IsVideo] = (i % 2) ? 1.0F : 0.0F; f[BlurZ] = std::normal_distribution<float>()(rng);
        x.push_back(f); y.push_back(f[IsVideo] > 0 ? 0 : static_cast<std::uint8_t>(i % 4 == 0));
    }
    const auto fit = fit_delete_model(x, y, prior);
    pms_check(fit.beta[IsVideo] < prior.beta[IsVideo] - 0.5F, "labels move a weight away from the prior");
    pms_check(std::isfinite(fit.bias), "fit stays finite");

    const auto z = fit_zscore(std::vector<float>{2, 2, 2});
    pms_check(std::abs(z(2.0F)) < 1e-6F && std::isfinite(z(3.0F)), "constant feature does not divide by zero");
}
```
- [ ] **Step 2-4:** fail -> implement Newton: gradient `X^T(p - y) + (theta - theta0)/sigma^2`, Hessian `X^T diag(p(1-p)) X + I/sigma^2`, solve 9x9 by Cholesky, 25 iterations max, stop on step norm < 1e-8 -> pass.
- [ ] **Step 5: Commit** `feat(core): score deletions with evidence and learn from swipes`.

---

### Task 9: Duplicates: exact, bursts, PDQ near-duplicates, star clusters, best shot

**Files:** Create `core/include/pms/duplicates.hpp`, `core/src/duplicates.cpp`; Test `core/tests/duplicates_tests.cpp`

**Interfaces:**
- Consumes: `PdqHash`, `hamming` (Task 5), `EmbeddingMatrix` (Task 6).
- Produces:
```cpp
namespace pms {
struct DupItem {
    std::string sha256;            // empty when unknown (e.g. Bluetooth before hashing)
    PdqHash pdq;                   // quality 0 when not computed
    std::int64_t captured_unix_ms{};
    std::string burst_uuid;
    float keep_score{};            // quality + taste, higher = better shot
    bool is_video{};
};
struct DupConfig { int pdq_max_distance{31}; int pdq_min_quality{50}; float burst_min_cosine{0.93F};
                   std::int64_t window_ms{60'000}; std::size_t window_items{50}; float star_min_cosine{0.90F}; };
enum class DupKind : std::uint8_t { Exact, Burst, Near };
struct DupCluster { DupKind kind{}; std::size_t best{}; std::vector<std::size_t> others; };
// Embeddings row i belongs to items[i]. threads = 0 uses std::thread::hardware_concurrency().
[[nodiscard]] std::vector<DupCluster> find_duplicates(std::span<const DupItem> items, const EmbeddingMatrix& embeddings,
                                                      const DupConfig& config = {}, unsigned threads = 0);
}
```
Algorithm (each item ends in at most one cluster; priority Exact > Burst > Near):
1. Exact: group equal non-empty `sha256`.
2. Burst edges: same non-empty `burst_uuid`; plus pairs within the capture-time window (sort by time, compare each to the next `window_items` with `|dt| <= window_ms`) with `cosine >= burst_min_cosine`. Videos never pair with photos.
3. Near edges: brute-force all pairs over items with `pdq.quality >= pdq_min_quality`, XOR + `std::popcount` over 4 words, split the outer loop across `threads`, collect pairs with distance `<= pdq_max_distance`.
4. Connected components over edges, then a star per component: `best` = max `keep_score` (ties by lower index); keep a member only if `cosine(member, best) >= star_min_cosine` (embedding exists) or it has an exact/burst edge to `best`. Dropped members return to singletons. This prevents chaining (Review Focus 3).

- [ ] **Step 1: Failing test**

```cpp
#include <chrono>
#include <vector>
#include "pms/duplicates.hpp"
#include "test_harness.hpp"

namespace {
pms::PdqHash h(std::uint64_t a, int q = 100) { return {{a, 0, 0, 0}, q}; }
std::vector<float> unit(std::size_t dim, std::size_t hot, float lean = 0.0F) {
    std::vector<float> v(dim, 0.0F); v[hot] = 1.0F; v[(hot + 1) % dim] = lean; return v;
}
}  // namespace

void run_duplicates_tests() {
    using namespace pms;
    pms::EmbeddingMatrix e(8);
    std::vector<DupItem> items;
    auto add = [&](DupItem it, std::vector<float> v) { items.push_back(std::move(it)); e.append(v); };
    add({"aa", h(0b1111), 1000, "", 0.9F, false}, unit(8, 0));          // 0 exact pair, best
    add({"aa", h(0b1111), 2000, "", 0.1F, false}, unit(8, 0));          // 1
    add({"", h(0xF0F0), 50'000, "", 0.5F, false}, unit(8, 2));          // 2 burst by time + cosine
    add({"", h(0xF0F1), 50'500, "", 0.8F, false}, unit(8, 2, 0.05F));   // 3 burst best
    add({"", h(0xAAAA'0000), 900'000'000, "", 0.4F, false}, unit(8, 4)); // 4 near (WhatsApp copy, far in time)
    add({"", h(0xAAAA'0003), 5, "", 0.7F, false}, unit(8, 4, 0.1F));     // 5 near best
    add({"", h(0xFFFF'FFFF'FFFF'FFFF), 7, "", 0.2F, false}, unit(8, 6)); // 6 unrelated
    const auto clusters = find_duplicates(items, e, {}, 2);
    auto find = [&](std::size_t i) -> const DupCluster* {
        for (const auto& c : clusters) { if (c.best == i) return &c; for (auto o : c.others) if (o == i) return &c; }
        return nullptr;
    };
    pms_check(find(0) && find(0)->kind == DupKind::Exact && find(0)->best == 0, "exact pair keeps the better copy");
    pms_check(find(2) && find(2)->kind == DupKind::Burst && find(2)->best == 3, "burst picks the best shot");
    pms_check(find(4) && find(4)->kind == DupKind::Near && find(4)->best == 5, "PDQ finds cross-time copies");
    pms_check(find(6) == nullptr, "unrelated photo stays alone");

    // Chain guard: A~B and B~C by PDQ, but A and C differ by embedding: C must not join A's star.
    pms::EmbeddingMatrix e2(8);
    std::vector<DupItem> chain{{"", h(0), 0, "", 0.9F, false}, {"", h(0xFFFF), 0, "", 0.5F, false}, {"", h(0xFFFF'FFFF), 0, "", 0.1F, false}};
    e2.append(unit(8, 0)); e2.append(unit(8, 0, 0.3F)); e2.append(unit(8, 5));
    const auto chained = find_duplicates(chain, e2, {.pdq_max_distance = 16}, 1);
    for (const auto& c : chained) for (auto o : c.others) pms_check(o != 2, "star clustering drops chained outliers");

    // Scale: 20,000 identical flat-ish hashes finish fast and form bounded work.
    std::vector<DupItem> many(20'000, DupItem{"", h(0x1234), 0, "", 0.5F, false});
    pms::EmbeddingMatrix e3(8);
    for (std::size_t i = 0; i < many.size(); ++i) e3.append(unit(8, i % 8));
    const auto t0 = std::chrono::steady_clock::now();
    (void)find_duplicates(many, e3, {}, 0);
    pms_check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20), "20k identical hashes finish quickly");
}
```
- [ ] **Step 2-4:** fail -> implement -> pass. Hot-path note: edge lists for identical hashes are capped per item at 256 neighbors (keep closest by distance, then by index) to bound memory; the star pass still uses the full component.
- [ ] **Step 5: Commit** `feat(core): find exact, burst and near duplicates without chaining`.

---

### Task 10: Tags and categories

**Files:** Create `core/include/pms/tags.hpp`, `core/src/tags.cpp`; Test `core/tests/tags_tests.cpp`

**Interfaces:**
```cpp
namespace pms {
struct TagTable {
    std::vector<std::string> names; EmbeddingMatrix prompts{768}; std::vector<float> thresholds;
    float logit_scale{}; float logit_bias{}; std::string model_sha256;
};
// Binary format "PMSTAG1\0", u32 count, u32 dim, f32 logit_scale, f32 logit_bias, 64-byte ascii model sha256,
// then per tag: u16 name length + utf8 name, f32 threshold, dim x f32 prompt embedding. Throws std::runtime_error on any bound violation.
[[nodiscard]] TagTable load_tag_table(std::span<const std::byte> blob);
[[nodiscard]] std::vector<std::byte> save_tag_table(const TagTable& table);
struct TagHit { std::size_t tag{}; float probability{}; };
// p = sigmoid(exp(logit_scale) * cos + logit_bias); returns hits with p >= threshold, highest first, at most 3.
[[nodiscard]] std::vector<TagHit> tags_for(const TagTable& table, const EmbeddingMatrix& items, std::size_t row);
struct CategoryRow { std::string name; std::size_t count{}; std::uint64_t bytes{}; double expected_reclaim_bytes{}; };
// Categories = tags plus media kinds; sorted by expected_reclaim_bytes = sum(p_delete * bytes) descending.
[[nodiscard]] std::vector<CategoryRow> rank_categories(std::span<const std::vector<std::string>> item_categories,
                                                       std::span<const float> p_delete, std::span<const std::uint64_t> bytes);
}
```
- [ ] **Step 1: Failing test** (round-trip, threshold filter, ordering, malformed blob):

```cpp
#include <cmath>
#include <vector>
#include "pms/tags.hpp"
#include "test_harness.hpp"

void run_tags_tests() {
    pms::TagTable t; t.prompts = pms::EmbeddingMatrix(4); t.logit_scale = std::log(10.0F); t.logit_bias = -5.0F;
    t.model_sha256 = std::string(64, 'a');
    t.names = {"food", "pet"}; t.thresholds = {0.5F, 0.5F};
    t.prompts.append(std::vector<float>{1, 0, 0, 0}); t.prompts.append(std::vector<float>{0, 1, 0, 0});
    const auto blob = pms::save_tag_table(t);
    const auto back = pms::load_tag_table(blob);
    pms_check(back.names == t.names && back.model_sha256 == t.model_sha256, "tag table round trip");
    pms::EmbeddingMatrix items(4);
    items.append(std::vector<float>{0.95F, 0.3F, 0, 0});
    const auto hits = pms::tags_for(back, items, 0);
    pms_check(hits.size() == 1 && back.names[hits[0].tag] == "food", "only tags above threshold");
    auto bad = blob; bad.resize(20);
    bool threw = false;
    try { (void)pms::load_tag_table(bad); } catch (const std::runtime_error&) { threw = true; }
    pms_check(threw, "truncated tag table is rejected");
    const std::vector<std::vector<std::string>> cats{{"Screenshots"}, {"Screenshots"}, {"food"}};
    const auto rows = pms::rank_categories(cats, std::vector<float>{0.9F, 0.8F, 0.1F}, std::vector<std::uint64_t>{1000, 1000, 50'000});
    pms_check(rows.front().name == "food" && std::abs(rows.front().expected_reclaim_bytes - 5000.0) < 1e-6, "rank by expected reclaimable bytes");
}
```
- [ ] **Step 2-4:** fail -> implement -> pass.
- [ ] **Step 5: Commit** `feat(core): multi-label tags and categories ranked by reclaimable space`.

---

### Task 11: Deck policy (likely delete, help me learn, exploration)

**Files:** Create `core/include/pms/deck_policy.hpp`, `core/src/deck_policy.cpp`; Test `core/tests/deck_policy_tests.cpp`

**Interfaces:**
```cpp
namespace pms {
enum class CardReason : std::uint8_t { LikelyDelete, HelpMeLearn, Exploration };
struct DeckCard { std::size_t item{}; CardReason reason{}; };
struct DeckPolicy { std::size_t uncertainty_every{8}; std::size_t exploration_every{10}; float diversity_max_cosine{0.90F}; };
// eligible[i] == 0 removes an item (protected, already decided, not auto-delete eligible).
// Card k (0-based, counting from served_so_far) is Exploration when (k+1) % exploration_every == 0, else
// HelpMeLearn when (k+1) % uncertainty_every == 0, else the highest remaining p_delete.
// HelpMeLearn = smallest |p - 0.5| among eligible items whose max cosine to every labeled item is <= diversity_max_cosine.
// Exploration = uniform random eligible item (splitmix64 from seed + served_so_far).
[[nodiscard]] std::vector<DeckCard> next_cards(std::span<const float> p_delete, std::span<const std::uint8_t> eligible,
                                               const EmbeddingMatrix& embeddings, std::span<const std::size_t> labeled,
                                               std::size_t served_so_far, std::size_t count, const DeckPolicy& policy,
                                               std::uint64_t seed);
}
```
- [ ] **Step 1: Failing test**

```cpp
#include <set>
#include <vector>
#include "pms/deck_policy.hpp"
#include "test_harness.hpp"

void run_deck_policy_tests() {
    using namespace pms;
    const std::size_t n = 40;
    EmbeddingMatrix e(4);
    std::vector<float> p(n); std::vector<std::uint8_t> ok(n, 1);
    for (std::size_t i = 0; i < n; ++i) { p[i] = float(i) / n; std::vector<float> v{1, float(i), 0, 0}; e.append(v); }
    ok[n - 1] = 0;  // protected
    const auto cards = next_cards(p, ok, e, {}, 0, 20, DeckPolicy{}, 42);
    pms_check(cards.size() == 20, "requested count");
    pms_check(cards[0].reason == CardReason::LikelyDelete && cards[0].item == n - 2, "top card is the highest eligible p_delete");
    pms_check(cards[7].reason == CardReason::HelpMeLearn, "every 8th card helps the model learn");
    pms_check(cards[9].reason == CardReason::Exploration, "every 10th card explores");
    std::set<std::size_t> seen;
    for (const auto& c : cards) { pms_check(ok[c.item] == 1, "never serves an ineligible item"); pms_check(seen.insert(c.item).second, "no repeats"); }
    const auto again = next_cards(p, ok, e, {}, 0, 20, DeckPolicy{}, 42);
    pms_check(again[9].item == cards[9].item, "seeded exploration is reproducible");
    std::vector<std::uint8_t> none(n, 0);
    pms_check(next_cards(p, none, e, {}, 0, 5, DeckPolicy{}, 1).empty(), "nothing eligible gives an empty deck");
}
```
- [ ] **Step 2-4, Step 5:** implement, pass, commit `feat(core): mix likely deletes with learning and exploration cards`.

---

### Task 12: ETA estimator and wording

**Files:** Create `core/include/pms/eta.hpp`, `core/src/eta.cpp`; Test `core/tests/eta_tests.cpp`

**Interfaces:**
```cpp
namespace pms {
using EtaClock = std::chrono::steady_clock;
struct StageProgress { double done{}; double total{}; bool total_known{true}; };
enum class EtaPhase : std::uint8_t { Estimating, Counting, Remaining, Done };
struct EtaDisplay { EtaPhase phase{EtaPhase::Estimating}; double seconds{}; double counted{}; };
class EtaEstimator {
public:
    explicit EtaEstimator(std::size_t stages, std::chrono::seconds window = std::chrono::seconds(30));
    void update(std::size_t stage, StageProgress progress, EtaClock::time_point now);
    // Bottleneck model: concurrent stages finish when the slowest one does, so remaining = max_i remaining_i.
    // Rate_i = (done_now - done_at_window_start) / elapsed over the window. Estimating until 3 s and 20 samples,
    // and again whenever any unfinished stage has rate 0 for the whole window (stall, Review Focus 4).
    // The shown seconds change only when the new value differs by >10% or 15 s passed since the last change.
    [[nodiscard]] EtaDisplay display(EtaClock::time_point now);
};
// "less than a minute", "about 1 minute", "about 7 minutes", "about 1 hour 10 minutes", "about 3 hours".
[[nodiscard]] std::string eta_text(double seconds);
}
```
- [ ] **Step 1: Failing test**

```cpp
#include <chrono>
#include "pms/eta.hpp"
#include "test_harness.hpp"

void run_eta_tests() {
    using namespace std::chrono;
    using pms::EtaPhase;
    pms::EtaEstimator eta(2);
    auto t = pms::EtaClock::time_point{};
    eta.update(0, {0, 1000}, t);
    pms_check(eta.display(t).phase == EtaPhase::Estimating, "starts estimating");
    for (int s = 1; s <= 40; ++s) {  // stage 0: 10 items/s of 1000; stage 1: 50/s of 1000 (not the bottleneck)
        t += seconds(1);
        eta.update(0, {10.0 * s, 1000}, t);
        eta.update(1, {50.0 * s, 1000}, t);
    }
    auto d = eta.display(t);
    pms_check(d.phase == EtaPhase::Remaining && d.seconds > 54 && d.seconds < 66, "bottleneck stage sets the ETA (600 s left of 1000/10)");
    // Hysteresis: a small speed wobble does not change the shown value.
    const double shown = d.seconds;
    t += seconds(1); eta.update(0, {410.5, 1000}, t);
    pms_check(eta.display(t).seconds == shown, "small changes do not jitter the display");
    // Stall: no progress for the whole window.
    for (int s = 0; s < 31; ++s) { t += seconds(1); eta.update(0, {410.5, 1000}, t); }
    pms_check(eta.display(t).phase == EtaPhase::Estimating, "a stall falls back to Estimating");
    // Unknown total counts instead of guessing.
    pms::EtaEstimator scan(1);
    scan.update(0, {12431, 0, false}, t);
    const auto c = scan.display(t);
    pms_check(c.phase == EtaPhase::Counting && c.counted == 12431, "unknown totals show a live count");
    pms_check(pms::eta_text(30) == "less than a minute" && pms::eta_text(65) == "about 1 minute", "short wording");
    pms_check(pms::eta_text(4200) == "about 1 hour 10 minutes", "hour wording");
}
```
- [ ] **Step 2-4, Step 5:** implement (per-stage deque of (time, done) samples trimmed to the window), pass, commit `feat(core): honest bottleneck ETA with hysteresis`.

---

### Task 13: Engine decoders (256 px RGB proxies)

**Files:**
- Create: `cmake/Dependencies.cmake`; `engine/CMakeLists.txt`; `engine/include/pms/engine/decode.hpp`; `engine/src/decode.cpp`; `engine/tests/decode_tests.cpp`; `engine/tests/make_fixtures.py` (writes tiny JPEG/PNG/GIF/WebP/HEIC/MP4 fixtures into `engine/tests/fixtures/`, each < 60 KB, generated from synthetic pixels so they are license-free)
- Modify: root `CMakeLists.txt` (`add_subdirectory(engine)` when Qt is found), `THIRD_PARTY_NOTICES.md` (libheif LGPL-3.0, Qt FFmpeg LGPL-2.1+ with source offer)

**Interfaces:**
```cpp
namespace pms::engine {
struct RgbImage { std::uint32_t width{}, height{}; std::vector<std::uint8_t> pixels; };  // interleaved RGB8
struct DecodeLimits { std::uint64_t max_pixels{200'000'000}; std::uint64_t max_bytes{4ULL << 30}; };
enum class DecodeError : std::uint8_t { None, Unreadable, Unsupported, TooLarge, Corrupt };
struct Proxy { RgbImage image; DecodeError error{DecodeError::None}; std::uint32_t source_width{}, source_height{}; double duration_s{}; };
// Still images: long side scaled to `size` (JPEG via QImageReader::setScaledSize, HEIC via its thumbnail item when
// present, else primary). EXIF orientation / HEIF irot+imir applied. Declared dimensions are checked against
// limits BEFORE decoding (Review Focus 5).
[[nodiscard]] Proxy decode_still(const std::filesystem::path& file, MediaFormat format, std::uint32_t size, const DecodeLimits& = {});
// Videos and animations: `frames` evenly spaced keyframes (videos via libav with protocol_whitelist=file;
// GIF/WebP/HEIF sequences via QImageReader/libheif). Each frame long side = size.
[[nodiscard]] std::vector<Proxy> decode_frames(const std::filesystem::path& file, MediaFormat format, std::uint32_t size, int frames);
}
```
`cmake/Dependencies.cmake` stages pinned inputs into `${CMAKE_BINARY_DIR}/_deps` with `file(DOWNLOAD ... EXPECTED_HASH SHA256=...)`: ffmpeg-7.1 source tarball (`40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6`), libheif-1.23.6 (`4484346dc5995319dbc11e3a1c35d0a2ec46511ce370900869337fd2c7033125`), then runs `tools/qt_ffmpeg_sdk.py` and builds libheif with the exact options from `docs/spikes.md` section 6 via `ExternalProject_Add`. Downloads happen at configure time only; the built product never downloads.

- [ ] **Step 1: Fixtures + failing test**

```cpp
#include <filesystem>
#include "pms/engine/decode.hpp"
#include "pms/media_format.hpp"
#include "test_harness.hpp"

#ifndef PMS_ENGINE_FIXTURES
#error "PMS_ENGINE_FIXTURES required"
#endif

void run_decode_tests() {
    namespace fs = std::filesystem;
    using pms::MediaFormat;
    const fs::path dir{PMS_ENGINE_FIXTURES};
    for (auto [name, fmt] : {std::pair{"photo.jpg", MediaFormat::Jpeg}, {"photo.png", MediaFormat::Png},
                             {"photo.webp", MediaFormat::WebP}, {"photo.heic", MediaFormat::Heif}}) {
        const auto p = pms::engine::decode_still(dir / name, fmt, 256);
        pms_check(p.error == pms::engine::DecodeError::None, name);
        pms_check(std::max(p.image.width, p.image.height) == 256 && p.image.pixels.size() == std::size_t{p.image.width} * p.image.height * 3, "proxy is 256 px RGB");
    }
    const auto rotated = pms::engine::decode_still(dir / "rotated_exif6.jpg", MediaFormat::Jpeg, 256);
    pms_check(rotated.image.height > rotated.image.width, "EXIF orientation 6 is applied");
    const auto frames = pms::engine::decode_frames(dir / "clip.mp4", MediaFormat::Mp4, 256, 8);
    pms_check(frames.size() == 8 && frames[0].duration_s > 1.0, "eight keyframes from a video");
    const auto anim = pms::engine::decode_frames(dir / "anim.gif", MediaFormat::Gif, 256, 4);
    pms_check(anim.size() == 4, "frames from an animated GIF");
    const auto bomb = pms::engine::decode_still(dir / "bomb.png", MediaFormat::Png, 256);  // header declares 60000x60000
    pms_check(bomb.error == pms::engine::DecodeError::TooLarge, "decompression bomb rejected before allocation");
    const auto trunc = pms::engine::decode_still(dir / "truncated.jpg", MediaFormat::Jpeg, 256);
    pms_check(trunc.error != pms::engine::DecodeError::None, "truncated JPEG reported, no crash");
    const auto liar = pms::engine::decode_frames(dir / "playlist_named.mp4", MediaFormat::Mp4, 256, 2);  // an HLS playlist pointing at http://
    pms_check(liar.empty() || liar[0].error != pms::engine::DecodeError::None, "network references are refused");
}
```
`engine/tests/make_fixtures.py` builds: `photo.jpg/png/webp` (synthetic gradient 1200x900 via Pillow), `photo.heic` (pillow-heif, with a 320 px thumbnail), `rotated_exif6.jpg` (1200x900 + EXIF Orientation=6), `clip.mp4` (ffmpeg `testsrc2` 640x360 4 s), `anim.gif` (6 frames), `bomb.png` (valid 1x1 PNG with IHDR rewritten to 60000x60000 and CRC fixed), `truncated.jpg` (first 300 bytes of photo.jpg), `playlist_named.mp4` (text `#EXTM3U\n#EXTINF:1,\nhttp://127.0.0.1:9/x.ts\n`).
- [ ] **Step 2: Run, expect failure** (engine target missing).
- [ ] **Step 3: Implement** using the spike code in `tools/spikes/decode/decode_bench.cpp` as the starting point (it already has the libheif thumbnail path and the libav keyframe loop); add: `QImageReader::size()` check against `max_pixels` before `read()`, `QImageReader::setAutoTransform(true)`, libheif `heif_image_handle_get_width/height` check before decode, `heif_decoding_options.ignore_transformations = false`, libav `format_whitelist`-free open but `protocol_whitelist=file` and `AVFMT_FLAG_NOBUFFER` not needed; reject `fmt->iformat` named `hls`/`dash`/`rtsp`/`sdp`/`concat` explicitly.
- [ ] **Step 4: Run, expect pass.** Copy `heif.dll` and Qt's FFmpeg DLLs next to the test binary with a POST_BUILD step.
- [ ] **Step 5: Commit** `feat(engine): decode phone media into 256 px proxies safely`.

---

### Task 14: Engine encoder (ONNX Runtime + WebGPU, SigLIP2 B/32 fp32)

**Files:**
- Create: `engine/include/pms/engine/encoder.hpp`, `engine/src/encoder.cpp`, `engine/tests/encoder_tests.cpp`, `models/manifest.json`, `tools/models.py` (download-and-verify for dev and CI; never called by the app)
- Modify: `cmake/Dependencies.cmake` (ORT 1.30.0 SDK per OS + `onnxruntime-ep-webgpu` 0.4.0 binaries, SHA-256 pinned), `engine/CMakeLists.txt`

**Interfaces:**
```cpp
namespace pms::engine {
enum class Device : std::uint8_t { Gpu, Cpu };
struct EncoderInfo { Device device{}; std::string adapter; std::size_t input_size{256}; std::size_t dim{768}; };
class ImageEncoder {
public:
    // Verifies the model file's SHA-256 against `expected_sha256` before loading. Prefers WebGPU on the adapter with the
    // smallest DxgiHighPerformanceIndex (CoreML EP on macOS); falls back to CPU EP. Never appends any other EP.
    static std::unique_ptr<ImageEncoder> open(const std::filesystem::path& model, std::string_view expected_sha256,
                                              Device preferred, std::string* error);
    [[nodiscard]] const EncoderInfo& info() const noexcept;
    // One L2-normalized 768-d vector per image, row-major. Batches are split to `max_batch`.
    [[nodiscard]] std::vector<float> embed(std::span<const RgbImage> images, std::size_t max_batch = 16);
};
// PIL-compatible antialiased bilinear resize to size x size, then (x/255 - 0.5)/0.5, CHW float.
[[nodiscard]] std::vector<float> preprocess(const RgbImage& image, std::size_t size);
}
```
`models/manifest.json`:
```json
{"encoder": {"name": "siglip2-base-patch32-256 vision fp32", "file": "siglip2-b32-256-vision-fp32.onnx",
  "url": "https://huggingface.co/onnx-community/siglip2-base-patch32-256-ONNX/resolve/main/onnx/vision_model.onnx",
  "sha256": "<filled by tools/models.py --pin>", "bytes": 378500000, "license": "Apache-2.0", "source": "google/siglip2-base-patch32-256"},
 "tags": {"file": "tags-b32.bin", "built_by": "tools/build_tag_table.py", "sha256": "<filled when built>"}}
```
- [ ] **Step 1: Failing tests**: `preprocess` parity against a fixture written by `engine/tests/make_fixtures.py` with PIL (`resize((256,256), BILINEAR)` of `photo.jpg`, normalized, saved as `preprocess_ref.f32`), max abs diff < 2e-2; `open()` with a wrong SHA-256 fails with an error string and returns null; when the model file exists (skip with a printed note otherwise), embedding the same image twice gives cosine > 0.9999, embedding `photo.jpg` vs a 5% brightness change > 0.95, and the GPU and CPU paths agree (cosine > 0.999) when a GPU is present.
- [ ] **Step 2: Run, expect failure.**
- [ ] **Step 3: Implement** with the ORT C++ API (`Ort::Env`, `Ort::SessionOptions`, `RegisterExecutionProviderLibrary`, `GetEpDevices`, `AppendExecutionProvider_V2` with exactly one device); SHA-256 via `QCryptographicHash` streaming; inputs `pixel_values` [N,3,256,256], output `pooler_output` [N,768].
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit** `feat(engine): embed images with SigLIP2 on the local GPU, CPU fallback`.

---

### Task 15: Test gallery, tag table and evaluation harness (S1)

**Files:**
- Create: `tools/make_test_gallery.py`, `tools/build_tag_table.py`, `tools/pms-eval.cpp`, `tools/eval_report.py`, `evaluation/README.md`, `evaluation/gates.json`, `evaluation/ATTRIBUTION.md`
- Modify: `tools/CMakeLists.txt` (add `pms_eval` linking `pms_core`, `pms_engine`), `.gitignore` (`evaluation/gallery*/`, `evaluation/runs/`)

**Interfaces:**
- Consumes: everything above.
- Produces: `evaluation/report.json` with the gate table below, `evaluation/charts/*.svg`.

`tools/make_test_gallery.py --out evaluation/gallery --seed 7` builds a phone-shaped tree from the COCO subset (`tools/spikes/coco_subset.py` output, human labels) plus generated items, and writes `evaluation/gallery/manifest.json`:
```json
{"items": [{"path": "DCIM/Camera/IMG_0001.jpg", "kind": "Photo", "coco_super": "food", "license": 4,
            "truth": {"exact_of": null, "near_of": null, "burst": null, "useful_keep": false}, "sha256": "..."}],
 "personas": [{"name": "food lover", "loves": ["food", "kitchen"], "favorites": ["DCIM/Camera/IMG_0012.jpg", "..."]}]}
```
Contents (seeded, ~450 items): 250 COCO photos across supercategories; 20 exact copies (into `WhatsApp/Media`); 40 near copies (resize 50-80%, JPEG q 40-70, crop 5-15%, +-15% brightness) into `Download`/`WhatsApp`; 8 bursts of 4 (crops of one photo, BurstUUID in an injected EXIF MakerNote, 300 ms apart); 25 degraded (Gaussian blur r 4-8, gamma 0.3 dark, +80 blown); 30 synthetic screenshots (PNG at 1080x2400, UI-like blocks and text) and 10 document/receipt images (white paper, black text) marked `useful_keep`; 20 video clips from Blender open movies (CC BY 3.0, 5-15 s, some with `vidstab`-style shake added); 2 screen recordings; 10 GIFs; 5 animated WebP; 15 HEIC; 5 Live Photo pairs; 3 Motion Photos. Three personas (pets & nature, food & cooking, people & sports): favorites = 30 items from loved supercategories, sampled with skew (70% from the most recent half by capture time, 30% weighted by sharpness).

`pms-eval` runs the shipped code on that tree (decode -> encode -> quality/PDQ -> taste per persona with 5-fold out-of-fold scores -> delete model cold start and after 100 simulated swipes with 10% label noise -> duplicates -> kinds) and writes raw scores. `tools/eval_report.py` computes metrics with 1,000x bootstrap CIs, checks `evaluation/gates.json`, writes `report.json` + SVG charts, exits non-zero on any failed gate.

`evaluation/gates.json` (from the master plan):
```json
{"favorite_violations": {"max": 0},
 "taste_auc_np20": {"min": 0.65}, "taste_auc_np50": {"min": 0.70}, "taste_auc_after_100_swipes": {"min": 0.78},
 "delete_precision_at_50": {"min": 0.80}, "useful_keep_in_top100": {"max": 5},
 "neardup_recall_synthetic": {"min": 0.95}, "neardup_precision_synthetic": {"min": 0.98},
 "best_shot_agreement": {"min": 0.65}, "kind_accuracy": {"min": 1.0}, "screenshot_precision": {"min": 0.99},
 "eta_median_abs_error_after_25pct": {"max": 0.20}}
```
- [ ] **Step 1:** Write `make_test_gallery.py` and run it; check `manifest.json` counts per kind.
- [ ] **Step 2:** Write `build_tag_table.py` (SigLIP2 B/32 text tower fp16 on CPU, prompt ensembles for ~24 tags: people, selfie, group photo, pet, dog, cat, food, drink, landscape, beach, mountains, city, night, document, receipt, screenshot, meme, whiteboard, car, flowers, sports, concert, baby, art; thresholds tuned on COCO human labels for precision >= 0.9 where labels exist, else 0.5) -> `models/tags-b32.bin`.
- [ ] **Step 3:** Write `pms-eval.cpp` and `eval_report.py`; first run fails gates that depend on missing pieces, which is expected until Tasks 1-14 are in.
- [ ] **Step 4:** With Tasks 1-14 merged, run `pms-eval` + `eval_report.py`; every gate passes, or the failing gate gets a written diagnosis and a fix task before moving on.
- [ ] **Step 5: Commit** `feat(eval): phone-shaped test gallery and gated evaluation report`.

---

## Self-review notes

- Spec coverage: media kinds incl. Live/Motion/AAE/bursts (Tasks 1-3), quality (4), near-dups and best shot (5, 9), embeddings + encoder on GPU (6, 14), PU taste head chosen in spike 5 (7), two-stage delete scoring with document_like and hard protection (8, 11), categories by reclaimable bytes (10), active learning + exploration (11), ETA (12), decoders incl. HEIC/video and bombs (13), test gallery + gates (15). Desktop pipeline, UI, Bluetooth and vault live in Plans B and C.
- Removed from the master plan by spikes: no int8/fp16 encoder, no MLP (logistic won), no ANN index.
- Old v1 `media_analysis` / `preference_model` / 16-d `FixedEmbedding` stay until Plan C switches the desktop over, then are deleted there.
