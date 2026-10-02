#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pms {

enum class Decision : std::uint8_t { Keep, Delete, Skip };

struct ReviewDecision {
    std::string asset_id;
    Decision decision{Decision::Skip};
    std::uint64_t bytes{};
};

struct TrashBatch {
    std::vector<std::string> asset_ids;
    std::uint64_t total_bytes{};
};

class ReviewQueue final {
public:
    void protect(std::string asset_id);

    [[nodiscard]] bool decide(std::string asset_id, Decision decision, std::uint64_t bytes);
    [[nodiscard]] std::optional<ReviewDecision> undo();
    [[nodiscard]] std::size_t pending_delete_count() const noexcept;
    [[nodiscard]] std::uint64_t pending_delete_bytes() const noexcept;
    [[nodiscard]] TrashBatch prepare_trash_batch() const;

private:
    struct Mutation {
        std::string asset_id;
        std::optional<ReviewDecision> previous;
    };

    void remove_delete_aggregate(const ReviewDecision& decision) noexcept;
    void add_delete_aggregate(const ReviewDecision& decision) noexcept;

    std::unordered_set<std::string> protected_assets_;
    std::unordered_map<std::string, ReviewDecision> decisions_;
    std::vector<Mutation> history_;
    std::size_t pending_delete_count_{};
    std::uint64_t pending_delete_bytes_{};
};

}  // namespace pms

