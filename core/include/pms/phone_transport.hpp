#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pms {

enum class PermissionCoverage : std::uint8_t { Denied, Partial, Full };
enum class TransportError : std::uint8_t {
    Disconnected,
    InvalidRequest,
    StaleCursor,
    AssetNotFound,
    ProtectedAsset,
    InvalidToken,
};

struct CatalogAsset {
    std::string asset_id;
    std::uint64_t bytes{};
    bool favorite{};
    bool trashed{};
};

struct CatalogPage {
    std::uint64_t revision{};
    std::vector<CatalogAsset> assets;
    std::string next_cursor;
    bool complete{};
};

struct TrashPreparation {
    std::string token;
    std::vector<std::string> asset_ids;
    std::uint64_t total_bytes{};
};

struct TrashResult {
    std::string token;
    std::vector<std::string> trashed_ids;
};

class PhoneTransport {
public:
    virtual ~PhoneTransport() = default;
    virtual void connect() = 0;
    virtual void disconnect() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    [[nodiscard]] virtual std::expected<CatalogPage, TransportError> catalog_page(
        std::string_view cursor,
        std::size_t limit) const = 0;
    [[nodiscard]] virtual std::expected<TrashPreparation, TransportError> prepare_trash(
        std::span<const std::string> asset_ids) = 0;
    [[nodiscard]] virtual std::expected<TrashResult, TransportError> commit_trash(
        std::string_view token) = 0;
};

}  // namespace pms

