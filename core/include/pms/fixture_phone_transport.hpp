#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pms/phone_transport.hpp"

namespace pms {

class FixturePhoneTransport final : public PhoneTransport {
public:
    explicit FixturePhoneTransport(std::vector<CatalogAsset> catalog);

    void connect() override;
    void disconnect() noexcept override;
    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] std::expected<CatalogPage, TransportError> catalog_page(
        std::string_view cursor,
        std::size_t limit) const override;
    [[nodiscard]] std::expected<TrashPreparation, TransportError> prepare_trash(
        std::span<const std::string> asset_ids) override;
    [[nodiscard]] std::expected<TrashResult, TransportError> commit_trash(
        std::string_view token) override;

private:
    std::vector<CatalogAsset> catalog_;
    bool connected_{};
    std::uint64_t revision_{1};
    std::uint64_t token_counter_{};
    std::optional<TrashPreparation> pending_;
    std::unordered_map<std::string, TrashResult> committed_;
};

}  // namespace pms
