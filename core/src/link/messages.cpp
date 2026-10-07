#include "pms/link/messages.hpp"

#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace pms::link {

bool valid_utf8(const std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint32_t lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t smallest = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            extra = 1;
            code = lead & 0x1FU;
            smallest = 0x80U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra = 2;
            code = lead & 0x0FU;
            smallest = 0x800U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra = 3;
            code = lead & 0x07U;
            smallest = 0x10000U;
        } else {
            return false;
        }
        if (text.size() - i <= extra) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const std::uint32_t next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < smallest || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

namespace {

constexpr std::uint8_t media_image = 1;  // MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE
constexpr std::uint8_t media_video = 3;  // MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO

// Big-endian writer. Caps throw std::length_error; encode() checks everything else by decoding.
class Writer {
public:
    template <typename T>
    void num(const T value) {
        static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
        const auto bits = static_cast<std::uint64_t>(value);
        for (std::size_t shift = sizeof(T) * 8; shift > 0;) {
            shift -= 8;
            out_.push_back(static_cast<std::byte>((bits >> shift) & 0xFFU));
        }
    }
    template <typename E>
    void enumeration(const E value) {
        num(static_cast<std::underlying_type_t<E>>(value));
    }
    void flag(const bool value) {
        num(static_cast<std::uint8_t>(value ? 1U : 0U));
    }
    void count(const std::size_t n, const std::size_t cap) {
        if (n > cap) {
            throw std::length_error("list above its protocol cap");
        }
        num(static_cast<std::uint16_t>(n));
    }
    void str(const std::string_view text) {
        if (text.size() > max_string) {
            throw std::length_error("string above 1024 bytes");
        }
        num(static_cast<std::uint16_t>(text.size()));
        const auto raw = std::as_bytes(std::span{text});
        out_.insert(out_.end(), raw.begin(), raw.end());
    }
    void bytes(const std::span<const std::byte> data) {
        if (data.size() > max_jpeg) {
            throw std::length_error("JPEG above 60 KB");
        }
        num(static_cast<std::uint16_t>(data.size()));
        out_.insert(out_.end(), data.begin(), data.end());
    }
    void sha256(const std::array<std::uint8_t, 32>& digest) {
        for (const auto value : digest) {
            num(value);
        }
    }
    [[nodiscard]] std::vector<std::byte> finish() && {
        if (out_.size() > max_plaintext) {
            throw std::length_error("message above 65519 bytes");
        }
        return std::move(out_);
    }

private:
    std::vector<std::byte> out_;
};

// Big-endian reader. Every read checks remaining() first and returns false on overrun or a broken rule.
class Reader {
public:
    explicit Reader(const std::span<const std::byte> in) noexcept : in_{in} {}

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size(); }

    template <typename T>
    [[nodiscard]] bool num(T& out) noexcept {
        static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
        if (in_.size() < sizeof(T)) {
            return false;
        }
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bits = (bits << 8U) | std::to_integer<std::uint64_t>(in_[i]);
        }
        in_ = in_.subspan(sizeof(T));
        out = static_cast<T>(bits);
        return true;
    }
    template <typename E>
    [[nodiscard]] bool enumeration(E& out, const E last) noexcept {
        std::underlying_type_t<E> value{};
        if (!num(value) || value > static_cast<std::underlying_type_t<E>>(last)) {
            return false;
        }
        out = static_cast<E>(value);
        return true;
    }
    [[nodiscard]] bool flag(bool& out) noexcept {
        std::uint8_t value = 0;
        if (!num(value) || value > 1) {
            return false;
        }
        out = value == 1;
        return true;
    }
    [[nodiscard]] bool count(std::size_t& out, const std::size_t cap) noexcept {
        return length(out) && out <= cap;
    }
    [[nodiscard]] bool str(std::string& out) {
        std::size_t size = 0;
        if (!length(size) || size > max_string || size > in_.size()) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(in_.data()), size);
        in_ = in_.subspan(size);
        return valid_utf8(out);
    }
    [[nodiscard]] bool bytes(std::vector<std::byte>& out) {
        std::size_t size = 0;
        if (!length(size) || size > max_jpeg || size > in_.size()) {
            return false;
        }
        const auto data = in_.first(size);
        out.assign(data.begin(), data.end());
        in_ = in_.subspan(size);
        return true;
    }
    [[nodiscard]] bool sha256(std::array<std::uint8_t, 32>& out) noexcept {
        for (auto& value : out) {
            if (!num(value)) {
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] bool length(std::size_t& out) noexcept {
        std::uint16_t value = 0;
        if (!num(value)) {
            return false;
        }
        out = value;
        return true;
    }

    std::span<const std::byte> in_;
};

using TrashEntry = std::pair<std::uint64_t, TrashOutcome>;

// List elements. Declared before write_list/read_list so the templates find them.
void write_value(Writer& w, const std::uint64_t id) { w.num(id); }
[[nodiscard]] bool read_value(Reader& r, std::uint64_t& id) noexcept { return r.num(id); }

void write_value(Writer& w, const std::vector<std::byte>& jpeg) { w.bytes(jpeg); }
[[nodiscard]] bool read_value(Reader& r, std::vector<std::byte>& jpeg) { return r.bytes(jpeg); }

void write_value(Writer& w, const CatalogItem& v) {
    w.num(v.id);
    w.num(v.media_type);
    w.str(v.relative_path);
    w.str(v.mime);
    w.num(v.size);
    w.num(v.width);
    w.num(v.height);
    w.num(v.duration_ms);
    w.num(v.date_taken_ms);
    w.num(v.date_modified_s);
    w.num(v.generation_modified);
    w.flag(v.favorite);
}
[[nodiscard]] bool read_value(Reader& r, CatalogItem& v) {
    return r.num(v.id) && r.num(v.media_type) && (v.media_type == media_image || v.media_type == media_video) &&
           r.str(v.relative_path) && r.str(v.mime) && r.num(v.size) && r.num(v.width) && r.num(v.height) &&
           r.num(v.duration_ms) && r.num(v.date_taken_ms) && r.num(v.date_modified_s) &&
           r.num(v.generation_modified) && r.flag(v.favorite);
}

void write_value(Writer& w, const TrashItem& v) {
    w.num(v.id);
    w.num(v.size);
    w.num(v.date_modified_s);
    w.sha256(v.sha256);
}
[[nodiscard]] bool read_value(Reader& r, TrashItem& v) noexcept {
    return r.num(v.id) && r.num(v.size) && r.num(v.date_modified_s) && r.sha256(v.sha256);
}

void write_value(Writer& w, const TrashEntry& v) {
    w.num(v.first);
    w.enumeration(v.second);
}
[[nodiscard]] bool read_value(Reader& r, TrashEntry& v) noexcept {
    return r.num(v.first) && r.enumeration(v.second, TrashOutcome::Failed);
}

// Lists: u16 BE count, then the elements.
template <typename T>
void write_list(Writer& w, const std::vector<T>& items, const std::size_t cap) {
    w.count(items.size(), cap);
    for (const auto& item : items) {
        write_value(w, item);
    }
}
template <typename T>
[[nodiscard]] bool read_list(Reader& r, std::vector<T>& items, const std::size_t cap) {
    std::size_t n = 0;
    if (!r.count(n, cap)) {
        return false;
    }
    // Grow one parsed element at a time: memory follows the bytes present, not the claimed count.
    for (std::size_t i = 0; i < n; ++i) {
        T item{};
        if (!read_value(r, item)) {
            return false;
        }
        items.push_back(std::move(item));
    }
    return true;
}

// Message bodies.
void write_value(Writer& w, const Hello& v) {
    w.num(v.protocol);
    w.str(v.model);
    w.str(v.os);
    w.str(v.app_version);
    w.num(v.capabilities);
}
[[nodiscard]] bool read_value(Reader& r, Hello& v) {
    return r.num(v.protocol) && r.str(v.model) && r.str(v.os) && r.str(v.app_version) && r.num(v.capabilities);
}

void write_value(Writer& w, const HelloAck& v) {
    w.num(v.protocol);
    w.str(v.desktop_name);
}
[[nodiscard]] bool read_value(Reader& r, HelloAck& v) {
    return r.num(v.protocol) && r.str(v.desktop_name);
}

void write_value(Writer& w, const CatalogSince& v) {
    w.str(v.store_version);
    w.num(v.generation);
    w.num(v.max_items);
}
[[nodiscard]] bool read_value(Reader& r, CatalogSince& v) {
    return r.str(v.store_version) && r.num(v.generation) && r.num(v.max_items) && v.max_items <= max_catalog_items;
}

void write_value(Writer& w, const CatalogPage& v) {
    w.str(v.store_version);
    w.num(v.next_generation);
    w.flag(v.full_resync);
    w.flag(v.more);
    write_list(w, v.items, max_catalog_items);
    write_list(w, v.removed, max_removed_ids);
}
[[nodiscard]] bool read_value(Reader& r, CatalogPage& v) {
    return r.str(v.store_version) && r.num(v.next_generation) && r.flag(v.full_resync) && r.flag(v.more) &&
           read_list(r, v.items, max_catalog_items) && read_list(r, v.removed, max_removed_ids);
}

void write_value(Writer& w, const IdList& v) { write_list(w, v.ids, max_request_ids); }
[[nodiscard]] bool read_value(Reader& r, IdList& v) { return read_list(r, v.ids, max_request_ids) && !v.ids.empty(); }

void write_value(Writer& w, const Preview& v) {
    w.num(v.id);
    w.enumeration(v.status);
    w.num(v.width);
    w.num(v.height);
    w.bytes(v.jpeg);
}
[[nodiscard]] bool read_value(Reader& r, Preview& v) {
    return r.num(v.id) && r.enumeration(v.status, ItemStatus::Busy) && r.num(v.width) && r.num(v.height) &&
           r.bytes(v.jpeg);
}

void write_value(Writer& w, const KeyframesRequest& v) {
    w.num(v.id);
    w.num(v.count);
}
[[nodiscard]] bool read_value(Reader& r, KeyframesRequest& v) noexcept {
    return r.num(v.id) && r.num(v.count) && v.count != 0 && static_cast<std::size_t>(v.count) <= max_keyframes;
}

void write_value(Writer& w, const Keyframes& v) {
    w.num(v.id);
    w.enumeration(v.status);
    write_list(w, v.jpegs, max_keyframes);
}
[[nodiscard]] bool read_value(Reader& r, Keyframes& v) {
    return r.num(v.id) && r.enumeration(v.status, ItemStatus::Busy) && read_list(r, v.jpegs, max_keyframes);
}

void write_value(Writer& w, const Hash& v) {
    w.num(v.id);
    w.enumeration(v.status);
    w.sha256(v.sha256);
}
[[nodiscard]] bool read_value(Reader& r, Hash& v) noexcept {
    return r.num(v.id) && r.enumeration(v.status, ItemStatus::Busy) && r.sha256(v.sha256);
}

void write_value(Writer& w, const TrashRequest& v) { write_list(w, v.items, max_trash_items); }
[[nodiscard]] bool read_value(Reader& r, TrashRequest& v) {
    return read_list(r, v.items, max_trash_items) && !v.items.empty();
}

void write_value(Writer& w, const TrashResult& v) { write_list(w, v.items, max_trash_items); }
[[nodiscard]] bool read_value(Reader& r, TrashResult& v) { return read_list(r, v.items, max_trash_items); }

void write_value(Writer& w, const ErrorMsg& v) {
    w.num(v.code);
    w.str(v.message);
}
[[nodiscard]] bool read_value(Reader& r, ErrorMsg& v) {
    return r.num(v.code) && r.str(v.message);
}

void write_value(Writer& /*w*/, const std::monostate& /*bye*/) {}
[[nodiscard]] bool read_value(Reader& /*r*/, std::monostate& /*bye*/) noexcept { return true; }

// The single type -> body mapping. Calls f(std::type_identity<Body alternative>); false for an unknown type.
template <typename F>
bool with_body_type(const MsgType type, F&& f) {
    switch (type) {
    case MsgType::Hello: f(std::type_identity<Hello>{}); return true;
    case MsgType::HelloAck: f(std::type_identity<HelloAck>{}); return true;
    case MsgType::CatalogSince: f(std::type_identity<CatalogSince>{}); return true;
    case MsgType::CatalogPage: f(std::type_identity<CatalogPage>{}); return true;
    case MsgType::PreviewRequest: f(std::type_identity<IdList>{}); return true;
    case MsgType::Preview: f(std::type_identity<Preview>{}); return true;
    case MsgType::KeyframesRequest: f(std::type_identity<KeyframesRequest>{}); return true;
    case MsgType::Keyframes: f(std::type_identity<Keyframes>{}); return true;
    case MsgType::HashRequest: f(std::type_identity<IdList>{}); return true;
    case MsgType::Hash: f(std::type_identity<Hash>{}); return true;
    case MsgType::TrashRequest: f(std::type_identity<TrashRequest>{}); return true;
    case MsgType::TrashResult: f(std::type_identity<TrashResult>{}); return true;
    case MsgType::Error: f(std::type_identity<ErrorMsg>{}); return true;
    case MsgType::Bye: f(std::type_identity<std::monostate>{}); return true;
    }
    return false;
}

}  // namespace

std::vector<std::byte> encode(const Message& m) {
    bool matches = false;
    (void)with_body_type(m.type, [&]<typename T>(std::type_identity<T>) { matches = std::holds_alternative<T>(m.body); });
    if (!matches) {
        throw std::invalid_argument("message body does not match its type");
    }
    Writer w;
    w.enumeration(m.type);
    w.num(m.request_id);
    std::visit([&w](const auto& body) { write_value(w, body); }, m.body);
    auto bytes = std::move(w).finish();
    // decode() is the one definition of valid (enums, booleans, UTF-8, media types): never send what the peer must refuse.
    if (!decode(bytes)) {
        throw std::invalid_argument("message holds a value the protocol refuses");
    }
    return bytes;
}

std::optional<Message> decode(const std::span<const std::byte> plaintext) noexcept {
    try {
        if (plaintext.size() > max_plaintext) {
            return std::nullopt;
        }
        Reader r{plaintext};
        std::uint8_t type = 0;
        Message m;
        if (!r.num(type) || !r.num(m.request_id)) {
            return std::nullopt;
        }
        m.type = static_cast<MsgType>(type);
        bool ok = false;
        const bool known = with_body_type(m.type, [&]<typename T>(std::type_identity<T>) {
            T body{};
            ok = read_value(r, body);
            m.body = std::move(body);
        });
        if (!known || !ok || r.remaining() != 0) {
            return std::nullopt;
        }
        return m;
    } catch (...) {
        return std::nullopt;  // only allocation can throw here; treat it like a violation and close the session
    }
}

}  // namespace pms::link
