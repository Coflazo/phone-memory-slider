#include "pms/media_meta.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <system_error>

namespace pms {
namespace {

using Bytes = std::span<const std::byte>;
using namespace std::string_view_literals;

// Whole-string integer parse: no sign for unsigned types, no whitespace, no trailing bytes, no overflow.
template <class Int>
[[nodiscard]] std::optional<Int> parse_int(const std::string_view text) noexcept {
    Int value{};
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Value of the first `name="..."` attribute. ponytail: attribute form with double quotes only (what
// Google and Samsung cameras write); add element form if a real file carries it.
[[nodiscard]] std::optional<std::string_view> attribute(const std::string_view xml, const std::string_view name) noexcept {
    for (auto at = xml.find(name); at != std::string_view::npos; at = xml.find(name, at + 1)) {
        auto rest = xml.substr(at + name.size());
        if (!rest.starts_with("=\"")) {
            continue;
        }
        rest.remove_prefix(2);
        const auto close = rest.find('"');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return rest.substr(0, close);
    }
    return std::nullopt;
}

[[nodiscard]] bool matches(const Bytes data, const std::size_t offset, const std::string_view magic) noexcept {
    return offset <= data.size() && data.size() - offset >= magic.size() &&
           std::ranges::equal(data.subspan(offset, magic.size()), magic,
                              [](const std::byte b, const char c) noexcept { return b == static_cast<std::byte>(c); });
}

// Payload after `signature` of the first APP1 segment carrying it, clamped to the bytes we hold.
[[nodiscard]] std::optional<Bytes> find_app1(const Bytes jpeg, const std::string_view signature) noexcept {
    if (!matches(jpeg, 0, "\xFF\xD8")) {
        return std::nullopt;
    }
    std::size_t offset = 2;
    while (jpeg.size() - offset >= 4) {  // invariant: offset <= jpeg.size()
        if (jpeg[offset] != std::byte{0xFF}) {
            return std::nullopt;
        }
        const auto marker = std::to_integer<unsigned>(jpeg[offset + 1]);
        if (marker == 0xFF) {  // fill byte
            ++offset;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {  // scan data or end of image: no metadata follows
            return std::nullopt;
        }
        // Segment length is big-endian and counts its own two bytes.
        const auto length = (std::to_integer<std::size_t>(jpeg[offset + 2]) << 8) | std::to_integer<std::size_t>(jpeg[offset + 3]);
        if (length < 2) {
            return std::nullopt;
        }
        const auto body = jpeg.subspan(offset + 4, std::min(length - 2, jpeg.size() - offset - 4));
        if (marker == 0xE1 && matches(body, 0, signature)) {
            return body.subspan(signature.size());
        }
        if (length > jpeg.size() - offset - 2) {
            break;
        }
        offset += 2 + length;
    }
    return std::nullopt;
}

constexpr std::size_t max_ifd_entries = 512;
constexpr std::size_t max_ascii_bytes = 256;
constexpr std::uint16_t type_ascii = 2;
constexpr std::uint16_t type_long = 4;
constexpr std::uint16_t type_undefined = 7;
constexpr std::uint16_t type_ifd = 13;

struct Reader {
    Bytes data;
    bool big{};

    [[nodiscard]] std::optional<Bytes> slice(const std::size_t offset, const std::size_t length) const noexcept {
        if (offset > data.size() || length > data.size() - offset) {
            return std::nullopt;
        }
        return data.subspan(offset, length);
    }
    [[nodiscard]] std::optional<std::uint32_t> uint(const std::size_t offset, const std::size_t width) const noexcept {
        const auto field = slice(offset, width);
        if (!field) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            value = (value << 8) | std::to_integer<std::uint32_t>((*field)[big ? i : width - 1 - i]);
        }
        return value;
    }
    [[nodiscard]] std::optional<std::uint16_t> u16(const std::size_t offset) const noexcept {
        const auto value = uint(offset, 2);
        return value ? std::optional{static_cast<std::uint16_t>(*value)} : std::nullopt;
    }
    [[nodiscard]] std::optional<std::uint32_t> u32(const std::size_t offset) const noexcept { return uint(offset, 4); }
};

// Byte order from a 2-byte "II"/"MM" marker.
[[nodiscard]] std::optional<Reader> reader_for(const Bytes data, const std::size_t marker_at) noexcept {
    if (matches(data, marker_at, "II")) {
        return Reader{data, false};
    }
    if (matches(data, marker_at, "MM")) {
        return Reader{data, true};
    }
    return std::nullopt;
}

struct Entry {
    std::uint16_t tag{};
    std::uint16_t type{};
    std::uint32_t count{};
    std::size_t value_at{};  // offset of the 4-byte value/offset field
};

// Calls visit(Entry) for each entry of the IFD at `ifd`; stops at the first entry past the end.
template <class Visit>
void walk_ifd(const Reader& reader, const std::size_t ifd, Visit&& visit) noexcept {
    const std::size_t count = reader.u16(ifd).value_or(0);
    if (count > max_ifd_entries) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = ifd + 2 + 12 * i;
        const auto tag = reader.u16(at);
        const auto type = reader.u16(at + 2);
        const auto n = reader.u32(at + 4);
        if (!tag || !type || !n || !reader.slice(at + 8, 4)) {
            return;
        }
        visit(Entry{*tag, *type, *n, at + 8});
    }
}

// Bytes of a 1-byte-per-count value (ASCII, UNDEFINED). Values over 4 bytes sit at an offset from
// the reader's base. A value running past the end is rejected, or with `clamp` cut to what is there.
[[nodiscard]] std::optional<Bytes> value_bytes(const Reader& reader, const Entry& entry, const bool clamp) noexcept {
    const std::size_t length = entry.count;
    if (length <= 4) {
        return reader.slice(entry.value_at, length);
    }
    const auto offset = reader.u32(entry.value_at);
    if (!offset) {
        return std::nullopt;
    }
    if (clamp && *offset <= reader.data.size()) {
        return reader.data.subspan(*offset, std::min(length, reader.data.size() - *offset));
    }
    return reader.slice(*offset, length);
}

// ASCII value up to its first NUL, capped at max_ascii_bytes; empty when absent or out of bounds.
[[nodiscard]] std::string ascii(const Reader& reader, const Entry& entry) {
    const auto value = entry.type == type_ascii ? value_bytes(reader, entry, false) : std::nullopt;
    if (!value) {
        return {};
    }
    const auto capped = value->first(std::min(value->size(), max_ascii_bytes));
    const auto text = capped.first(static_cast<std::size_t>(std::ranges::find(capped, std::byte{0}) - capped.begin()));
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// "YYYY:MM:DD HH:MM:SS" plus optional OffsetTimeOriginal "+HH:MM"; local time is taken as UTC
// without an offset. Returns 0 for anything malformed.
[[nodiscard]] std::int64_t exif_time_ms(const std::string_view date, const std::string_view offset) noexcept {
    if (date.size() < 19 || date[4] != ':' || date[7] != ':' || date[10] != ' ' || date[13] != ':' || date[16] != ':') {
        return 0;
    }
    const auto y = parse_int<unsigned>(date.substr(0, 4));
    const auto mo = parse_int<unsigned>(date.substr(5, 2));
    const auto d = parse_int<unsigned>(date.substr(8, 2));
    const auto h = parse_int<unsigned>(date.substr(11, 2));
    const auto mi = parse_int<unsigned>(date.substr(14, 2));
    const auto s = parse_int<unsigned>(date.substr(17, 2));
    if (!y || !mo || !d || !h || !mi || !s || *h > 23 || *mi > 59 || *s > 59) {
        return 0;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{static_cast<int>(*y)}, std::chrono::month{*mo}, std::chrono::day{*d}};
    if (!ymd.ok()) {
        return 0;
    }
    std::chrono::seconds time = std::chrono::sys_days{ymd}.time_since_epoch() + std::chrono::seconds(*h * 3600 + *mi * 60 + *s);
    if (offset.size() >= 6 && (offset[0] == '+' || offset[0] == '-') && offset[3] == ':') {
        const auto oh = parse_int<unsigned>(offset.substr(1, 2));
        const auto om = parse_int<unsigned>(offset.substr(4, 2));
        if (oh && om && *oh <= 14 && *om <= 59) {
            const std::chrono::seconds shift(*oh * 3600 + *om * 60);
            time += offset[0] == '+' ? -shift : shift;
        }
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(time).count();
}

// Apple MakerNote: "Apple iOS\0", version 00 01, byte order at +12, IFD at +14; offsets are relative
// to the MakerNote start.
void read_apple_maker_note(const Bytes note, ExifFacts& facts) noexcept {
    if (!matches(note, 0, "Apple iOS\0"sv)) {
        return;
    }
    const auto reader = reader_for(note, 12);
    if (!reader) {
        return;
    }
    walk_ifd(*reader, 14, [&](const Entry& entry) noexcept {
        if (entry.tag == 0x000B) {
            facts.burst_uuid = ascii(*reader, entry);
        } else if (entry.tag == 0x0011) {
            facts.content_identifier = ascii(*reader, entry);
        }
    });
}

}  // namespace

std::optional<MotionPhotoInfo> parse_motion_photo_xmp(const std::string_view xmp) noexcept {
    if (!xmp.contains(R"(Camera:MotionPhoto="1")") && !xmp.contains(R"(GCamera:MicroVideo="1")")) {
        return std::nullopt;
    }
    // v1: Item:Length of the Container:Item element whose Item:Semantic is MotionPhoto.
    std::optional<std::string_view> length;
    if (const auto at = xmp.find(R"(Item:Semantic="MotionPhoto")"); at != std::string_view::npos) {
        const auto open = xmp.rfind('<', at);
        const auto begin = open == std::string_view::npos ? 0 : open;
        const auto close = xmp.find('>', at);
        length = attribute(xmp.substr(begin, close == std::string_view::npos ? close : close - begin), "Item:Length");
    }
    if (!length) {
        length = attribute(xmp, "GCamera:MicroVideoOffset");  // legacy: video is the last N bytes
    }
    const auto bytes = length ? parse_int<std::uint64_t>(*length) : std::nullopt;
    if (!bytes || *bytes == 0) {
        return std::nullopt;
    }
    MotionPhotoInfo info{*bytes};
    if (const auto timestamp = attribute(xmp, "Camera:MotionPhotoPresentationTimestampUs")) {
        info.presentation_us = parse_int<std::int64_t>(*timestamp).value_or(-1);
    }
    return info;
}

std::optional<std::string_view> find_jpeg_xmp(const std::span<const std::byte> jpeg) noexcept {
    const auto packet = find_app1(jpeg, "http://ns.adobe.com/xap/1.0/\0"sv);
    if (!packet) {
        return std::nullopt;
    }
    return std::string_view{reinterpret_cast<const char*>(packet->data()), packet->size()};
}

std::optional<std::span<const std::byte>> find_jpeg_exif(const std::span<const std::byte> jpeg) noexcept {
    return find_app1(jpeg, "Exif\0\0"sv);
}

ExifFacts parse_exif(const std::span<const std::byte> tiff) noexcept {
    ExifFacts facts;
    const auto reader = reader_for(tiff, 0);
    const auto ifd0 = reader ? reader->u32(4) : std::nullopt;
    if (!ifd0 || reader->u16(2) != 42) {
        return facts;
    }
    std::optional<std::uint32_t> exif_ifd;
    walk_ifd(*reader, *ifd0, [&](const Entry& entry) noexcept {
        switch (entry.tag) {
            case 0x010F: facts.make = ascii(*reader, entry); break;
            case 0x0110: facts.model = ascii(*reader, entry); break;
            case 0x8769:
                if (entry.type == type_long || entry.type == type_ifd) {
                    exif_ifd = reader->u32(entry.value_at);
                }
                break;
            default: break;
        }
    });
    facts.has_camera = !facts.make.empty() || !facts.model.empty();
    if (!exif_ifd) {
        return facts;
    }
    std::string taken;
    std::string offset;
    std::optional<Bytes> maker_note;
    walk_ifd(*reader, *exif_ifd, [&](const Entry& entry) noexcept {
        switch (entry.tag) {
            case 0x9003: taken = ascii(*reader, entry); break;
            case 0x9011: offset = ascii(*reader, entry); break;
            case 0x927C:
                // MakerNote counts are often wrong: clamp to the blob; each value inside is re-checked.
                maker_note = entry.type == type_undefined ? value_bytes(*reader, entry, true) : std::nullopt;
                break;
            default: break;
        }
    });
    facts.captured_unix_ms = exif_time_ms(taken, offset);
    if (maker_note) {
        read_apple_maker_note(*maker_note, facts);
    }
    return facts;
}

}  // namespace pms
