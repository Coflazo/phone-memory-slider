#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pms/ranking.hpp"
#include "pms/review_queue.hpp"

namespace pms {

enum class MediaKind : std::uint8_t { Photo, Video };

enum class ReasonCode : std::uint8_t {
    ExactDuplicate,
    NearDuplicate,
    Blurry,
    ScreenCapture,
    LargeVideo,
    LowPersonalMatch,
};

struct ReviewItem {
    AssetFeatures asset;
    MediaKind media_kind{MediaKind::Photo};
    std::uint64_t bytes{};
    std::string display_name{};
    std::vector<ReasonCode> reasons{};
    std::string preview_uri{};
};

class ReviewSession final {
public:
    explicit ReviewSession(std::vector<ReviewItem> items, RankingPolicy policy = {});

    [[nodiscard]] std::optional<ReviewItem> current() const;
    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] bool decide_current(Decision decision);
    [[nodiscard]] bool undo();
    [[nodiscard]] std::uint64_t pending_delete_bytes() const noexcept;
    [[nodiscard]] std::size_t pending_delete_count() const noexcept;
    [[nodiscard]] TrashBatch prepare_trash_batch() const;

private:
    std::vector<ReviewItem> items_;
    std::size_t position_{};
    ReviewQueue queue_;
};

}  // namespace pms
