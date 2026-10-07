#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "pms/link/frames.hpp"

namespace pms::link {

// Plaintext message: u8 type | u32 BE request_id | payload. Byte layouts are in docs/PROTOCOL.md.
inline constexpr std::size_t max_plaintext = max_frame - 16;  // one frame holds the plaintext plus a 16-byte AEAD tag
inline constexpr std::size_t max_string = 1024;
inline constexpr std::size_t max_catalog_items = 2000;
inline constexpr std::size_t max_removed_ids = 20000;
inline constexpr std::size_t max_request_ids = 16;
inline constexpr std::size_t max_keyframes = 8;
inline constexpr std::size_t max_jpeg = 60 * 1024;
inline constexpr std::size_t max_trash_items = 500;

enum class MsgType : std::uint8_t { Hello = 0x01, HelloAck = 0x02, CatalogSince = 0x10, CatalogPage = 0x11,
    PreviewRequest = 0x12, Preview = 0x13, KeyframesRequest = 0x14, Keyframes = 0x15, HashRequest = 0x16, Hash = 0x17,
    TrashRequest = 0x20, TrashResult = 0x21, Error = 0x30, Bye = 0x31 };
enum class ItemStatus : std::uint8_t { Ok = 0, Missing = 1, Unreadable = 2, Busy = 3 };
enum class TrashOutcome : std::uint8_t { Trashed = 0, UserDeclined = 1, Changed = 2, Missing = 3, Favorite = 4, Failed = 5 };

struct Hello { std::uint16_t protocol{1}; std::string model; std::string os; std::string app_version; std::uint32_t capabilities{}; };
struct HelloAck { std::uint16_t protocol{1}; std::string desktop_name; };
struct CatalogSince { std::string store_version; std::uint64_t generation{}; std::uint32_t max_items{2000}; };
// media_type uses MediaStore's values: 1 = image, 3 = video.
struct CatalogItem { std::uint64_t id{}; std::uint8_t media_type{}; std::string relative_path; std::string mime;
    std::uint64_t size{}; std::uint32_t width{}, height{}, duration_ms{}; std::int64_t date_taken_ms{}, date_modified_s{};
    std::uint64_t generation_modified{}; bool favorite{}; };
struct CatalogPage { std::string store_version; std::uint64_t next_generation{}; bool full_resync{}; bool more{};
    std::vector<CatalogItem> items; std::vector<std::uint64_t> removed; };
struct IdList { std::vector<std::uint64_t> ids; };                                  // PreviewRequest, HashRequest (1 to 16 ids)
struct Preview { std::uint64_t id{}; ItemStatus status{}; std::uint32_t width{}, height{}; std::vector<std::byte> jpeg; };
struct KeyframesRequest { std::uint64_t id{}; std::uint8_t count{8}; };              // count 1 to 8
struct Keyframes { std::uint64_t id{}; ItemStatus status{}; std::vector<std::vector<std::byte>> jpegs; };
struct Hash { std::uint64_t id{}; ItemStatus status{}; std::array<std::uint8_t, 32> sha256{}; };
struct TrashItem { std::uint64_t id{}; std::uint64_t size{}; std::int64_t date_modified_s{}; std::array<std::uint8_t, 32> sha256{}; };
struct TrashRequest { std::vector<TrashItem> items; };                              // 1 to 500
struct TrashResult { std::vector<std::pair<std::uint64_t, TrashOutcome>> items; };
struct ErrorMsg { std::uint16_t code{}; std::string message; };
using Body = std::variant<Hello, HelloAck, CatalogSince, CatalogPage, IdList, Preview, KeyframesRequest, Keyframes, Hash,
                          TrashRequest, TrashResult, ErrorMsg, std::monostate /*Bye*/>;
struct Message { MsgType type{}; std::uint32_t request_id{}; Body body; };

// Throws std::length_error over a cap or over max_plaintext, and std::invalid_argument if the body does
// not match the type or holds a value decode() would refuse. Anything encode() returns decodes.
[[nodiscard]] std::vector<std::byte> encode(const Message& m);
// nullopt on any violation: unknown type, overrun, cap, enum or boolean out of range, invalid UTF-8, trailing bytes.
[[nodiscard]] std::optional<Message> decode(std::span<const std::byte> plaintext) noexcept;

}  // namespace pms::link
