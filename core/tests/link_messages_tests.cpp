#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pms/link/messages.hpp"
#include "test_harness.hpp"

#ifndef PMS_TEST_FIXTURES
#error "PMS_TEST_FIXTURES must point at core/tests/fixtures"
#endif

namespace {

using pms::link::Message;
using pms::link::MsgType;
using Bytes = std::vector<std::byte>;

// Appends value as `width` big-endian bytes.
void put(Bytes& b, const std::uint64_t value, const std::size_t width) {
    for (std::size_t shift = width * 8; shift > 0;) {
        shift -= 8;
        b.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

// Hand-built messages, independent of encode(). Each is valid except for the parameter under test.
Bytes header_bytes(const std::uint8_t type) {
    Bytes b;
    put(b, type, 1);
    put(b, 1, 4);  // request_id
    return b;
}

Bytes id_list(const std::size_t n) {
    auto b = header_bytes(0x12);
    put(b, n, 2);
    for (std::size_t i = 0; i < n; ++i) {
        put(b, i + 1, 8);
    }
    return b;
}

Bytes trash_request(const std::size_t n) {
    auto b = header_bytes(0x20);
    put(b, n, 2);
    for (std::size_t i = 0; i < n; ++i) {
        put(b, i + 1, 8);        // id
        put(b, 100, 8);          // size
        put(b, 1700000000, 8);   // date_modified_s
        b.resize(b.size() + 32); // sha256
    }
    return b;
}

Bytes keyframes(const std::size_t n) {
    auto b = header_bytes(0x15);
    put(b, 1, 8);  // id
    put(b, 0, 1);  // status Ok
    put(b, n, 2);
    for (std::size_t i = 0; i < n; ++i) {
        put(b, 0, 2);  // empty JPEG
    }
    return b;
}

Bytes keyframes_request(const std::size_t count) {
    auto b = header_bytes(0x14);
    put(b, 1, 8);
    put(b, count, 1);
    return b;
}

Bytes catalog_since(const std::size_t max_items) {
    auto b = header_bytes(0x10);
    put(b, 0, 2);  // empty store_version
    put(b, 0, 8);  // generation
    put(b, max_items, 4);
    return b;
}

Bytes catalog_page(const std::size_t n, const std::uint8_t media_type) {
    auto b = header_bytes(0x11);
    put(b, 0, 2);  // empty store_version
    put(b, 0, 8);  // next_generation
    put(b, 0, 1);  // full_resync
    put(b, 0, 1);  // more
    put(b, n, 2);
    for (std::size_t i = 0; i < n; ++i) {
        put(b, i + 1, 8);
        put(b, media_type, 1);
        put(b, 0, 2);  // empty relative_path
        put(b, 0, 2);  // empty mime
        b.resize(b.size() + 45);  // size .. favorite, all zero
    }
    put(b, 0, 2);  // no removed ids
    return b;
}

Bytes preview(const std::size_t jpeg_size) {
    auto b = header_bytes(0x13);
    put(b, 1, 8);    // id
    put(b, 0, 1);    // status Ok
    put(b, 256, 4);  // width
    put(b, 192, 4);  // height
    put(b, jpeg_size, 2);
    b.resize(b.size() + jpeg_size);
    return b;
}

// A Keyframes message of exactly `total` bytes: 32 bytes of fields and lengths, the rest over 8 JPEGs.
Bytes keyframes_of_size(const std::size_t total) {
    auto b = header_bytes(0x15);
    put(b, 1, 8);
    put(b, 0, 1);
    put(b, 8, 2);
    const auto body = total - 32;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto size = body / 8 + (i < body % 8 ? std::size_t{1} : std::size_t{0});
        put(b, size, 2);
        b.resize(b.size() + size);
    }
    return b;
}

std::array<std::uint8_t, 32> counting_sha256() {
    std::array<std::uint8_t, 32> sha{};
    for (std::size_t i = 0; i < sha.size(); ++i) {
        sha[i] = static_cast<std::uint8_t>(i);
    }
    return sha;
}

// One message of every type with fixed values. The Android port rebuilds the same list and
// must produce golden_messages.bin byte for byte; change it only with a protocol change.
std::vector<Message> golden_set() {
    using namespace pms::link;
    const std::vector<std::byte> jpeg{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xD9}};
    CatalogPage page{"1700000000:abc", 42, true, false, {}, {7, 8}};
    page.items.push_back({1, 1, "DCIM/Camera/a.jpg", "image/jpeg", 123456, 4000, 3000, 0, 1700000000000, 1700000000, 41, true});
    page.items.push_back({2, 3, "DCIM/Camera/b.mp4", "video/mp4", 9876543210, 1920, 1080, 15000, -1, 1700000100, 42, false});
    return {
        {MsgType::Hello, 0, Hello{1, "Pixel 9", "Android 15", "2.0.0", 0}},
        {MsgType::HelloAck, 0, HelloAck{1, "Desktop \xC3\xBC"}},
        {MsgType::CatalogSince, 1, CatalogSince{"", 0, 2000}},
        {MsgType::CatalogPage, 1, page},
        {MsgType::PreviewRequest, 0x01020304, IdList{{1, 2}}},
        {MsgType::Preview, 0x01020304, Preview{1, ItemStatus::Ok, 256, 192, jpeg}},
        {MsgType::KeyframesRequest, 3, KeyframesRequest{2, 8}},
        {MsgType::Keyframes, 3, Keyframes{2, ItemStatus::Ok, {jpeg, jpeg}}},
        {MsgType::HashRequest, 4, IdList{{1}}},
        {MsgType::Hash, 4, Hash{1, ItemStatus::Ok, counting_sha256()}},
        {MsgType::TrashRequest, 5, TrashRequest{{{1, 123456, 1700000000, counting_sha256()}}}},
        {MsgType::TrashResult, 5, TrashResult{{{1, TrashOutcome::Trashed}, {2, TrashOutcome::Favorite}}}},
        {MsgType::Error, 6, ErrorMsg{6, "Trash dialog already open"}},
        {MsgType::Bye, 0, std::monostate{}},
    };
}

// Each message preceded by its u16 BE length. Not a frame stream: Bye is shorter than a frame may be.
Bytes golden_bytes() {
    Bytes out;
    for (const auto& message : golden_set()) {
        const auto bytes = pms::link::encode(message);
        put(out, bytes.size(), 2);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

void check_golden_messages() {
    for (const auto& message : golden_set()) {
        const auto bytes = pms::link::encode(message);
        const auto back = pms::link::decode(bytes);
        pms_check(back && back->type == message.type && pms::link::encode(*back) == bytes, "every message type round-trips");
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            pms_check(!pms::link::decode(std::span(bytes).first(size)), "every truncation of every message type is refused");
        }
    }
    pms_check(golden_set().size() == 14, "golden set covers all 14 message types");

    const std::filesystem::path path = std::filesystem::path(PMS_TEST_FIXTURES) / "golden_messages.bin";
    pms_check(std::filesystem::exists(path), "golden_messages.bin exists (pms_core_tests --write-golden core/tests/fixtures)");
    std::vector<std::byte> file(static_cast<std::size_t>(std::filesystem::file_size(path)));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    pms_check(in.good() && file == golden_bytes(), "encoder output matches golden_messages.bin byte for byte");
}

}  // namespace

void write_link_golden_messages(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    const auto bytes = golden_bytes();
    std::ofstream out(dir / "golden_messages.bin", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    pms_check(out.good(), "golden_messages.bin written");
}

void run_link_messages_tests() {
    using namespace pms::link;
    CatalogPage page{"v1", 42, false, true, {}, {7, 8}};
    page.items.push_back({1, 1, "DCIM/Camera/a.jpg", "image/jpeg", 123456, 4000, 3000, 0, 1700000000000, 1700000000, 41, true});
    const Message m{MsgType::CatalogPage, 9, page};
    const auto bytes = encode(m);
    const auto back = decode(bytes);
    pms_check(back && back->request_id == 9, "round trip keeps request id");
    const auto& p = std::get<CatalogPage>(back->body);
    pms_check(p.items.size() == 1 && p.items[0].favorite && p.items[0].relative_path == "DCIM/Camera/a.jpg" && p.removed.size() == 2, "fields survive");
    auto trailing = bytes; trailing.push_back(std::byte{0});
    pms_check(!decode(trailing), "trailing bytes rejected");
    auto cut = bytes; cut.resize(cut.size() - 3);
    pms_check(!decode(cut), "truncated message rejected");
    std::vector<std::byte> unknown{std::byte{0x77}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    pms_check(!decode(unknown), "unknown type rejected");
    IdList many; many.ids.resize(17);
    bool threw = false;
    try { (void)encode({MsgType::PreviewRequest, 1, many}); } catch (const std::length_error&) { threw = true; }
    pms_check(threw, "id lists above 16 are refused");
    pms_check(decode(encode({MsgType::Bye, 0, std::monostate{}})).has_value(), "Bye has no payload");

    // Error = type | request_id | u16 code | u16 length | text. A full 1025-byte string is still refused.
    std::vector<std::byte> long_text{std::byte{0x30}, {}, {}, {}, {}, {}, std::byte{1}, std::byte{0x04}, std::byte{0x01}};
    long_text.resize(long_text.size() + 1025, std::byte{'a'});
    pms_check(!decode(long_text), "strings above 1024 bytes rejected");
    const std::vector<std::byte> bad_utf8{std::byte{0x30}, {}, {}, {}, {}, {}, std::byte{1}, {}, std::byte{1}, std::byte{0xFF}};
    pms_check(!decode(bad_utf8), "strings must be valid UTF-8");
    auto bad_status = encode({MsgType::Hash, 4, Hash{1, ItemStatus::Ok, {}}});
    bad_status[13] = std::byte{4};
    pms_check(!decode(bad_status), "unknown item status rejected");
    auto bad_flag = bytes;
    bad_flag[5 + 2 + 2 + 8] = std::byte{2};  // full_resync after store_version "v1" and next_generation
    pms_check(!decode(bad_flag), "booleans other than 0 and 1 rejected");
    threw = false;
    try { (void)encode({MsgType::Hash, 1, IdList{}}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "a body that does not match its type is refused");
    threw = false;
    try { (void)encode({MsgType::HashRequest, 1, IdList{}}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "an empty id list is refused at encode");
    threw = false;
    try { (void)encode({MsgType::KeyframesRequest, 1, KeyframesRequest{1, 0}}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "a request for 0 keyframes is refused at encode");

    // Decode-side caps on hand-built bytes: the cap itself is accepted, one past it is refused.
    pms_check(decode(id_list(16)) && !decode(id_list(17)) && !decode(id_list(0)), "id lists hold 1 to 16 ids");
    pms_check(decode(trash_request(500)) && !decode(trash_request(501)) && !decode(trash_request(0)), "trash requests hold 1 to 500 items");
    threw = false;
    try { (void)encode({MsgType::TrashRequest, 1, TrashRequest{}}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "an empty trash request is refused at encode");
    pms_check(decode(keyframes(8)) && !decode(keyframes(9)), "at most 8 keyframes");
    pms_check(decode(keyframes_request(8)) && !decode(keyframes_request(9)) && !decode(keyframes_request(0)), "keyframe requests ask for 1 to 8 frames");
    pms_check(decode(catalog_since(2000)) && !decode(catalog_since(2001)), "max_items is at most 2000");
    pms_check(decode(catalog_page(1000, 1)) && !decode(catalog_page(2001, 1)), "at most 2000 catalog items (2001 also overflows the plaintext limit)");
    pms_check(decode(catalog_page(1, 3)) && !decode(catalog_page(1, 2)), "media_type is 1 or 3");
    pms_check(decode(preview(max_jpeg)) && !decode(preview(max_jpeg + 1)), "JPEGs are at most 61,440 bytes");
    pms_check(keyframes_of_size(max_plaintext + 1).size() == 65'520 && decode(keyframes_of_size(max_plaintext)) &&
                  !decode(keyframes_of_size(max_plaintext + 1)),
              "plaintext is at most 65,519 bytes");

    check_golden_messages();
}
