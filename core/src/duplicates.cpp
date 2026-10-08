#include "pms/duplicates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <thread>
#include <tuple>
#include <utility>

namespace pms {
namespace {

constexpr std::size_t neighbor_cap = 256;  // PDQ neighbors kept per item
constexpr unsigned max_threads = 8;
constexpr std::size_t chunk = 64;  // rows a thread takes at a time
constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

// Union-find whose roots are the smallest index in their set, with path halving.
class Forest {
public:
    explicit Forest(const std::size_t n) : parent_(n) { std::iota(parent_.begin(), parent_.end(), std::size_t{0}); }
    std::size_t find(std::size_t x) noexcept {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    void unite(std::size_t a, std::size_t b) noexcept {
        a = find(a);
        b = find(b);
        if (a != b) parent_[std::max(a, b)] = std::min(a, b);
    }

private:
    std::vector<std::size_t> parent_;
};

// Hands rows [0, count) to up to `threads` threads in chunks; rows(begin, end, forest) unites the edges of its rows
// into that thread's own forest over n items, and the forests merge at the end. Components are a property of the
// edge set alone, so they do not depend on the thread count or on which thread ran which row. `rows` must not throw.
template <typename Rows>
Forest connect(const std::size_t n, const std::size_t count, const unsigned threads, const Rows& rows) {
    const auto workers = static_cast<unsigned>(std::clamp<std::size_t>((count + chunk - 1) / chunk, 1, threads));
    std::vector<Forest> forests(workers, Forest(n));
    std::atomic<std::size_t> next{0};
    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (unsigned t = 0; t < workers; ++t)
            pool.emplace_back([&, t] {
                for (std::size_t begin = next.fetch_add(chunk); begin < count; begin = next.fetch_add(chunk))
                    rows(begin, std::min(begin + chunk, count), forests[t]);
            });
    }
    for (unsigned t = 1; t < workers; ++t)
        for (std::size_t x = 0; x < n; ++x) forests[0].unite(x, forests[t].find(x));
    return std::move(forests[0]);
}

struct Neighbor {
    int distance;
    std::size_t index;
    auto operator<=>(const Neighbor&) const = default;
};

}  // namespace

std::vector<DupCluster> find_duplicates(const std::span<const DupItem> items, const EmbeddingMatrix& embeddings,
                                        const DupConfig& config, unsigned threads) {
    const std::size_t n = items.size();
    if (threads == 0) threads = std::thread::hardware_concurrency();
    threads = std::clamp(threads, 1U, max_threads);

    std::vector<DupCluster> clusters;
    std::vector<bool> taken(n);
    const auto similar = [&](const std::size_t a, const std::size_t b, const float threshold) {
        return a < embeddings.rows() && b < embeddings.rows() && embeddings.cosine(a, b) >= threshold;
    };
    const auto score = [&](const std::size_t i) {
        const float s = items[i].keep_score;
        return std::isnan(s) ? -std::numeric_limits<float>::infinity() : s;
    };
    // A group (ascending indices) becomes a cluster around its best member with the members `keep` accepts.
    const auto emit = [&](const std::span<const std::size_t> group, const DupKind kind, const auto& keep) {
        std::size_t best = group.front();
        for (const std::size_t m : group)
            if (score(m) > score(best)) best = m;
        DupCluster cluster{kind, best, {}};
        for (const std::size_t m : group)
            if (m != best && keep(m, best)) cluster.others.push_back(m);
        if (cluster.others.empty()) return;
        taken[best] = true;
        for (const std::size_t m : cluster.others) taken[m] = true;
        clusters.push_back(std::move(cluster));
    };
    // Walks `sorted` in runs of `same` neighbors and emits each run of two or more.
    const auto emit_runs = [&](const std::span<const std::size_t> sorted, const auto& same, const DupKind kind, const auto& keep) {
        for (std::size_t lo = 0, hi = 0; lo < sorted.size(); lo = hi) {
            for (hi = lo + 1; hi < sorted.size() && same(sorted[lo], sorted[hi]);) ++hi;
            if (hi - lo >= 2) emit(sorted.subspan(lo, hi - lo), kind, keep);
        }
    };

    // 1. Exact: equal non-empty sha256 (and the same media type).
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < n; ++i)
        if (!items[i].sha256.empty()) order.push_back(i);
    std::ranges::sort(order, [&](const std::size_t a, const std::size_t b) {
        return std::tie(items[a].is_video, items[a].sha256, a) < std::tie(items[b].is_video, items[b].sha256, b);
    });
    const auto same_hash = [&](const std::size_t a, const std::size_t b) {
        return items[a].is_video == items[b].is_video && items[a].sha256 == items[b].sha256;
    };
    emit_runs(order, same_hash, DupKind::Exact, [](std::size_t, std::size_t) { return true; });

    // 2. Burst edges among the rest: the same burst id, or neighbors in capture order (photos, then videos).
    std::vector<std::size_t> timeline;
    for (std::size_t i = 0; i < n; ++i)
        if (!taken[i] && items[i].captured_unix_ms != 0) timeline.push_back(i);
    std::ranges::sort(timeline, [&](const std::size_t a, const std::size_t b) {
        return std::tie(items[a].is_video, items[a].captured_unix_ms, a) < std::tie(items[b].is_video, items[b].captured_unix_ms, b);
    });
    std::vector<std::size_t> position(n, none);
    for (std::size_t p = 0; p < timeline.size(); ++p) position[timeline[p]] = p;
    const auto close_in_time = [&](const std::int64_t earlier, const std::int64_t later) {  // later >= earlier
        // Unsigned subtraction is exact for any later >= earlier, where the signed one could overflow.
        return config.window_ms >= 0 &&
               static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier) <= static_cast<std::uint64_t>(config.window_ms);
    };
    const auto burst_edge = [&](std::size_t a, std::size_t b) {
        if (items[a].is_video != items[b].is_video) return false;
        if (!items[a].burst_uuid.empty() && items[a].burst_uuid == items[b].burst_uuid) return true;
        if (position[a] == none || position[b] == none) return false;
        if (position[a] > position[b]) std::swap(a, b);
        return position[b] - position[a] <= config.window_items &&
               close_in_time(items[a].captured_unix_ms, items[b].captured_unix_ms) && similar(a, b, config.burst_min_cosine);
    };
    Forest bursts = connect(n, timeline.size(), threads, [&](const std::size_t begin, const std::size_t end, Forest& forest) {
        for (std::size_t p = begin; p < end; ++p)
            for (std::size_t q = p + 1; q < timeline.size() && q - p <= config.window_items; ++q) {
                const DupItem& a = items[timeline[p]];
                const DupItem& b = items[timeline[q]];
                if (a.is_video != b.is_video || !close_in_time(a.captured_unix_ms, b.captured_unix_ms)) break;
                if (similar(timeline[p], timeline[q], config.burst_min_cosine)) forest.unite(timeline[p], timeline[q]);
            }
    });
    order.clear();
    for (std::size_t i = 0; i < n; ++i)
        if (!taken[i] && !items[i].burst_uuid.empty()) order.push_back(i);
    std::ranges::sort(order, [&](const std::size_t a, const std::size_t b) {
        return std::tie(items[a].is_video, items[a].burst_uuid, a) < std::tie(items[b].is_video, items[b].burst_uuid, b);
    });
    for (std::size_t k = 1; k < order.size(); ++k) {
        const DupItem& a = items[order[k - 1]];
        const DupItem& b = items[order[k]];
        if (a.is_video == b.is_video && a.burst_uuid == b.burst_uuid) bursts.unite(order[k - 1], order[k]);
    }

    // A component becomes a star around its best shot: a member stays only if it looks like best or has a burst edge
    // to it, so a chain A~B~C cannot pull in a C that looks nothing like A.
    const auto star_member = [&](const std::size_t m, const std::size_t best) {
        return similar(m, best, config.star_min_cosine) || burst_edge(m, best);
    };
    const auto emit_components = [&](Forest& forest, const std::span<const std::size_t> members, const DupKind kind) {
        std::vector<std::pair<std::size_t, std::size_t>> rooted;  // (root, item), sorted: components in ascending order
        rooted.reserve(members.size());
        for (const std::size_t m : members) rooted.emplace_back(forest.find(m), m);
        std::ranges::sort(rooted);
        std::vector<std::size_t> sorted(rooted.size()), roots(n, none);
        for (std::size_t k = 0; k < rooted.size(); ++k) {
            sorted[k] = rooted[k].second;
            roots[sorted[k]] = rooted[k].first;
        }
        emit_runs(sorted, [&](const std::size_t a, const std::size_t b) { return roots[a] == roots[b]; }, kind, star_member);
    };
    std::vector<std::size_t> rest;
    for (std::size_t i = 0; i < n; ++i)
        if (!taken[i]) rest.push_back(i);
    emit_components(bursts, rest, DupKind::Burst);

    // 3. PDQ near-duplicates among the rest: both hashes reliable, brute force over all pairs of the same media type.
    // Each item keeps its neighbor_cap closest neighbors (ties by lower index); scanning in index order, a candidate
    // enters a full heap only when strictly closer than its worst member.
    // ponytail: every pair is scanned twice (once per row) and each row streams all hashes from L3: 250,000 photos
    // take about 25 s on 8 threads. Tile rows x columns, or scan half rows, if that ever matters next to embedding.
    std::vector<std::size_t> eligible;  // photos, then videos
    std::size_t photos = 0;
    for (const bool video : {false, true}) {
        for (std::size_t i = 0; i < n; ++i)
            if (!taken[i] && items[i].is_video == video && items[i].pdq.quality >= config.pdq_min_quality) eligible.push_back(i);
        if (!video) photos = eligible.size();
    }
    std::vector<std::array<std::uint64_t, 4>> hashes(eligible.size());
    for (std::size_t k = 0; k < eligible.size(); ++k) hashes[k] = items[eligible[k]].pdq.bits;
    Forest near = connect(n, eligible.size(), threads, [&](const std::size_t begin, const std::size_t end, Forest& forest) {
        const int max_distance = config.pdq_max_distance;
        const std::array<std::uint64_t, 4>* const h = hashes.data();
        std::array<Neighbor, neighbor_cap> heap;
        for (std::size_t r = begin; r < end; ++r) {
            const std::size_t lo = r < photos ? 0 : photos, hi = r < photos ? photos : hashes.size();
            const std::array<std::uint64_t, 4> a = h[r];
            std::size_t size = 0;
            for (std::size_t c = lo; c < hi; ++c) {
                const int d = std::popcount(a[0] ^ h[c][0]) + std::popcount(a[1] ^ h[c][1]) + std::popcount(a[2] ^ h[c][2]) +
                              std::popcount(a[3] ^ h[c][3]);
                if (d > max_distance || c == r) continue;
                if (size < neighbor_cap) {
                    heap[size++] = {d, c};
                    std::push_heap(heap.begin(), heap.begin() + static_cast<std::ptrdiff_t>(size));
                } else if (d < heap.front().distance) {
                    std::pop_heap(heap.begin(), heap.end());
                    heap.back() = {d, c};
                    std::push_heap(heap.begin(), heap.end());
                } else {
                    continue;
                }
                if (size == neighbor_cap && heap.front().distance == 0) break;  // no later index can be closer
            }
            for (std::size_t k = 0; k < size; ++k) forest.unite(eligible[r], eligible[heap[k].index]);
        }
    });
    emit_components(near, eligible, DupKind::Near);

    std::ranges::sort(clusters, {}, [](const DupCluster& c) { return std::pair{c.kind, c.best}; });
    return clusters;
}

}  // namespace pms
