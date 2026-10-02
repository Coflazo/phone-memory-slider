#include "pms/review_queue.hpp"

#include <algorithm>
#include <utility>

namespace pms {

void ReviewQueue::protect(std::string asset_id) {
    protected_assets_.insert(asset_id);
    if (const auto existing = decisions_.find(asset_id); existing != decisions_.end()) {
        remove_delete_aggregate(existing->second);
        decisions_.erase(existing);
    }
}

bool ReviewQueue::decide(std::string asset_id, const Decision decision, const std::uint64_t bytes) {
    if (decision == Decision::Delete && protected_assets_.contains(asset_id)) {
        return false;
    }

    const auto existing = decisions_.find(asset_id);
    if (existing != decisions_.end() && existing->second.decision == decision && existing->second.bytes == bytes) {
        return false;
    }

    Mutation mutation{asset_id, std::nullopt};
    if (existing != decisions_.end()) {
        mutation.previous = existing->second;
        remove_delete_aggregate(existing->second);
    }

    auto next = ReviewDecision{std::move(asset_id), decision, bytes};
    add_delete_aggregate(next);
    decisions_.insert_or_assign(next.asset_id, next);
    history_.push_back(std::move(mutation));
    return true;
}

std::optional<ReviewDecision> ReviewQueue::undo() {
    if (history_.empty()) {
        return std::nullopt;
    }

    auto mutation = std::move(history_.back());
    history_.pop_back();
    auto current = decisions_.find(mutation.asset_id);
    if (current == decisions_.end()) {
        return std::nullopt;
    }

    auto undone = current->second;
    remove_delete_aggregate(current->second);
    if (mutation.previous.has_value()) {
        decisions_.insert_or_assign(mutation.asset_id, *mutation.previous);
        add_delete_aggregate(*mutation.previous);
    } else {
        decisions_.erase(current);
    }
    return undone;
}

std::size_t ReviewQueue::pending_delete_count() const noexcept {
    return pending_delete_count_;
}

std::uint64_t ReviewQueue::pending_delete_bytes() const noexcept {
    return pending_delete_bytes_;
}

TrashBatch ReviewQueue::prepare_trash_batch() const {
    TrashBatch batch;
    batch.asset_ids.reserve(pending_delete_count_);
    for (const auto& [asset_id, decision] : decisions_) {
        if (decision.decision == Decision::Delete && !protected_assets_.contains(asset_id)) {
            batch.asset_ids.push_back(asset_id);
            batch.total_bytes += decision.bytes;
        }
    }
    std::ranges::sort(batch.asset_ids);
    return batch;
}

void ReviewQueue::remove_delete_aggregate(const ReviewDecision& decision) noexcept {
    if (decision.decision == Decision::Delete) {
        --pending_delete_count_;
        pending_delete_bytes_ -= decision.bytes;
    }
}

void ReviewQueue::add_delete_aggregate(const ReviewDecision& decision) noexcept {
    if (decision.decision == Decision::Delete) {
        ++pending_delete_count_;
        pending_delete_bytes_ += decision.bytes;
    }
}

}  // namespace pms

