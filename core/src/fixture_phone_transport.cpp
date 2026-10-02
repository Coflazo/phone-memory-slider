#include "pms/fixture_phone_transport.hpp"

#include <algorithm>
#include <charconv>
#include <unordered_set>
#include <utility>

namespace pms {
namespace {

[[nodiscard]] std::expected<std::size_t, TransportError> parse_cursor(
    const std::string_view cursor,
    const std::uint64_t revision) {
    if (cursor.empty()) {
        return 0;
    }
    const auto separator = cursor.find(':');
    if (separator == std::string_view::npos) {
        return std::unexpected{TransportError::InvalidRequest};
    }

    std::uint64_t cursor_revision{};
    std::size_t offset{};
    const auto revision_result = std::from_chars(cursor.data(), cursor.data() + separator, cursor_revision);
    const auto offset_result = std::from_chars(cursor.data() + separator + 1, cursor.data() + cursor.size(), offset);
    if (revision_result.ec != std::errc{} || revision_result.ptr != cursor.data() + separator ||
        offset_result.ec != std::errc{} || offset_result.ptr != cursor.data() + cursor.size()) {
        return std::unexpected{TransportError::InvalidRequest};
    }
    if (cursor_revision != revision) {
        return std::unexpected{TransportError::StaleCursor};
    }
    return offset;
}

}  // namespace

FixturePhoneTransport::FixturePhoneTransport(std::vector<CatalogAsset> catalog)
    : catalog_(std::move(catalog)) {}

void FixturePhoneTransport::connect() {
    connected_ = true;
}

void FixturePhoneTransport::disconnect() noexcept {
    connected_ = false;
    pending_.reset();
}

bool FixturePhoneTransport::connected() const noexcept {
    return connected_;
}

std::expected<CatalogPage, TransportError> FixturePhoneTransport::catalog_page(
    const std::string_view cursor,
    const std::size_t limit) const {
    if (!connected_) {
        return std::unexpected{TransportError::Disconnected};
    }
    if (limit == 0 || limit > 1'000) {
        return std::unexpected{TransportError::InvalidRequest};
    }
    const auto offset = parse_cursor(cursor, revision_);
    if (!offset.has_value() || *offset > catalog_.size()) {
        return std::unexpected{offset.has_value() ? TransportError::InvalidRequest : offset.error()};
    }

    const auto end = std::min(catalog_.size(), *offset + limit);
    CatalogPage page;
    page.revision = revision_;
    page.assets.assign(catalog_.begin() + static_cast<std::ptrdiff_t>(*offset),
                       catalog_.begin() + static_cast<std::ptrdiff_t>(end));
    page.complete = end == catalog_.size();
    if (!page.complete) {
        page.next_cursor = std::to_string(revision_) + ":" + std::to_string(end);
    }
    return page;
}

std::expected<TrashPreparation, TransportError> FixturePhoneTransport::prepare_trash(
    const std::span<const std::string> asset_ids) {
    if (!connected_) {
        return std::unexpected{TransportError::Disconnected};
    }
    if (asset_ids.empty()) {
        return std::unexpected{TransportError::InvalidRequest};
    }

    TrashPreparation preparation;
    std::unordered_set<std::string> seen;
    seen.reserve(asset_ids.size());
    for (const auto& asset_id : asset_ids) {
        if (!seen.insert(asset_id).second) {
            continue;
        }
        const auto asset = std::ranges::find(catalog_, asset_id, &CatalogAsset::asset_id);
        if (asset == catalog_.end() || asset->trashed) {
            return std::unexpected{TransportError::AssetNotFound};
        }
        if (asset->favorite) {
            return std::unexpected{TransportError::ProtectedAsset};
        }
        preparation.asset_ids.push_back(asset_id);
        preparation.total_bytes += asset->bytes;
    }
    preparation.token = "trash-" + std::to_string(++token_counter_);
    pending_ = preparation;
    return preparation;
}

std::expected<TrashResult, TransportError> FixturePhoneTransport::commit_trash(const std::string_view token) {
    if (const auto previous = committed_.find(std::string{token}); previous != committed_.end()) {
        return previous->second;
    }
    if (!connected_) {
        return std::unexpected{TransportError::Disconnected};
    }
    if (!pending_.has_value() || pending_->token != token) {
        return std::unexpected{TransportError::InvalidToken};
    }

    TrashResult result{pending_->token, pending_->asset_ids};
    for (const auto& asset_id : result.trashed_ids) {
        if (const auto asset = std::ranges::find(catalog_, asset_id, &CatalogAsset::asset_id);
            asset != catalog_.end()) {
            asset->trashed = true;
        }
    }
    ++revision_;
    committed_.emplace(result.token, result);
    pending_.reset();
    return result;
}

}  // namespace pms
