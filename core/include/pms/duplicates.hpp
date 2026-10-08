#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pms/embeddings.hpp"
#include "pms/pdq.hpp"

namespace pms {

struct DupItem {
    std::string sha256;            // empty when unknown (e.g. Bluetooth before hashing)
    PdqHash pdq;                   // quality 0 when not computed
    std::int64_t captured_unix_ms{};  // 0 when unknown
    std::string burst_uuid;
    float keep_score{};            // quality + taste, higher = better shot
    bool is_video{};
};

struct DupConfig {
    int pdq_max_distance{31};
    int pdq_min_quality{50};
    float burst_min_cosine{0.93F};
    std::int64_t window_ms{60'000};
    std::size_t window_items{50};
    float star_min_cosine{0.90F};
};

enum class DupKind : std::uint8_t { Exact, Burst, Near };

struct DupCluster {
    DupKind kind{};
    std::size_t best{};
    std::vector<std::size_t> others;  // ascending
    bool operator==(const DupCluster&) const = default;
};

// Groups duplicates for "keep the best one" cards. Each item ends in at most one cluster, and a video never pairs
// with a photo. Kinds are found in turn (Ruling R2), each among the items no earlier kind took:
//   Exact  equal non-empty sha256.
//   Burst  connected components of burst edges: the same non-empty burst_uuid, or two items with known capture
//          times at most window_items apart in capture order (videos and photos ordered separately), at most
//          window_ms apart, with cosine >= burst_min_cosine.
//   Near   connected components of PDQ edges: hamming <= pdq_max_distance between two hashes that both have quality
//          >= pdq_min_quality (flat frames hash to quality 0 and never match). Brute force over all pairs; each item
//          keeps its 256 closest neighbors (ties by lower index), which bounds the work for hot identical hashes.
// A Burst or Near component then becomes a star around its best shot (max keep_score, NaN lowest, ties by lower
// index): a member stays only if cosine(member, best) >= star_min_cosine or it has a burst edge to best, so a chain
// A~B~C never pulls C in unless C itself looks like A. Members a star drops may join a later kind; a component
// whose star keeps nobody yields no cluster. Embedding row i belongs to items[i]; an item without a row has no
// cosine, so it joins only through burst_uuid or the exact hash.
//
// Clusters come sorted by kind, then best. threads = 0 uses std::thread::hardware_concurrency(); any count is
// capped at 8. The result does not depend on the thread count. Memory beyond the result is linear: one parent index
// per item per thread, a packed copy of the PDQ hashes and a few index arrays (no edge lists are stored).
[[nodiscard]] std::vector<DupCluster> find_duplicates(std::span<const DupItem> items, const EmbeddingMatrix& embeddings,
                                                      const DupConfig& config = {}, unsigned threads = 0);

}  // namespace pms
