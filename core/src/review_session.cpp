#include "pms/review_session.hpp"

#include <unordered_map>
#include <utility>

namespace pms {

ReviewSession::ReviewSession(std::vector<ReviewItem> items, const RankingPolicy policy) {
    std::vector<AssetFeatures> features;
    features.reserve(items.size());
    std::unordered_map<std::string, ReviewItem> by_id;
    by_id.reserve(items.size());
    for (auto& item : items) {
        features.push_back(item.asset);
        by_id.emplace(item.asset.asset_id, std::move(item));
    }

    const auto ranked = rank_assets(features, policy);
    items_.reserve(ranked.size());
    for (const auto& rank : ranked) {
        auto node = by_id.extract(rank.asset_id);
        if (!node.empty()) {
            if (node.mapped().asset.favorite || node.mapped().asset.explicit_keep) {
                queue_.protect(node.mapped().asset.asset_id);
            }
            items_.push_back(std::move(node.mapped()));
        }
    }
}

std::optional<ReviewItem> ReviewSession::current() const {
    if (position_ >= items_.size()) {
        return std::nullopt;
    }
    return items_[position_];
}

std::size_t ReviewSession::remaining() const noexcept {
    return items_.size() - position_;
}

bool ReviewSession::decide_current(const Decision decision) {
    if (position_ >= items_.size()) {
        return false;
    }
    const auto& item = items_[position_];
    if (!queue_.decide(item.asset.asset_id, decision, item.bytes)) {
        return false;
    }
    ++position_;
    return true;
}

bool ReviewSession::undo() {
    if (position_ == 0 || !queue_.undo().has_value()) {
        return false;
    }
    --position_;
    return true;
}

std::uint64_t ReviewSession::pending_delete_bytes() const noexcept {
    return queue_.pending_delete_bytes();
}

std::size_t ReviewSession::pending_delete_count() const noexcept {
    return queue_.pending_delete_count();
}

TrashBatch ReviewSession::prepare_trash_batch() const {
    return queue_.prepare_trash_batch();
}

}  // namespace pms
