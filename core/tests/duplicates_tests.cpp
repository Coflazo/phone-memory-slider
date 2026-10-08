#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include "pms/duplicates.hpp"
#include "test_harness.hpp"

namespace {
pms::PdqHash h(std::uint64_t a, int q = 100) { return {{a, 0, 0, 0}, q}; }
std::vector<float> unit(std::size_t dim, std::size_t hot, float lean = 0.0F) {
    std::vector<float> v(dim, 0.0F); v[hot] = 1.0F; v[(hot + 1) % dim] = lean; return v;
}
// One word all ones: these four and h(0) are at least 64 bits apart, so they never form a PDQ edge.
pms::PdqHash w(std::size_t word) { pms::PdqHash p{{}, 100}; p.bits[word] = ~std::uint64_t{0}; return p; }

struct Gallery {
    std::vector<pms::DupItem> items;
    pms::EmbeddingMatrix embeddings;
};

// Scenes of 1-5 shots: PDQ hashes a few bits apart (sometimes too far, sometimes quality 0), embeddings with varying
// noise, capture times seconds apart (or unknown), some exact copies, burst ids and videos; plus 600 identical hashes.
Gallery structured_gallery(const std::size_t scenes, const std::uint64_t seed) {
    constexpr std::size_t dim = 16;
    Gallery g{{}, pms::EmbeddingMatrix(dim)};
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> normal;
    std::uniform_real_distribution<float> uniform;
    for (std::size_t s = 0; s < scenes; ++s) {
        pms::PdqHash base{{rng(), rng(), rng(), rng()}, 0};
        std::vector<float> scene(dim);
        for (float& x : scene) x = normal(rng);
        const std::int64_t start = rng() % 10 == 0 ? 0 : 1'600'000'000'000 + static_cast<std::int64_t>(rng() % 31'536'000'000);
        const std::size_t shots = 1 + rng() % 5;
        const float noise = 0.02F + 0.1F * uniform(rng);
        for (std::size_t k = 0; k < shots; ++k) {
            pms::DupItem item;
            item.pdq = base;
            for (std::uint64_t flips = rng() % 40; flips > 0; --flips) {
                const auto bit = rng() % 256;
                item.pdq.bits[bit / 64] ^= std::uint64_t{1} << (bit % 64);
            }
            item.pdq.quality = rng() % 10 == 0 ? 0 : 50 + static_cast<int>(rng() % 50);
            item.captured_unix_ms = start == 0 ? 0 : start + static_cast<std::int64_t>(k * (rng() % 40'000));
            const bool copy = k > 0 && rng() % 8 == 0;
            item.sha256 = copy ? g.items.back().sha256 : "s" + std::to_string(s) + "k" + std::to_string(k);
            item.burst_uuid = s % 7 == 0 ? "B" + std::to_string(s) : "";
            item.keep_score = static_cast<float>(rng() % 10) / 10.0F;  // ties on purpose
            item.is_video = s % 11 == 0;
            std::vector<float> v = scene;
            for (float& x : v) x += noise * normal(rng) * 4.0F;
            g.items.push_back(std::move(item));
            g.embeddings.append(v);
        }
    }
    for (std::size_t i = 0; i < 600; ++i) {  // hot identical hash, more members than the 256-neighbor cap
        g.items.push_back({"", h(0x5EED'5EED), 0, "", 0.5F, false});
        g.embeddings.append(unit(dim, i % 3));
    }
    return g;
}

void run_extra_duplicates_tests() {
    using namespace pms;
    const auto run = [](const std::vector<DupItem>& items, const std::vector<std::vector<float>>& vectors, const DupConfig& config = {}) {
        EmbeddingMatrix e(vectors.front().size());
        for (const auto& v : vectors) e.append(v);
        return find_duplicates(items, e, config, 2);
    };
    pms_check(find_duplicates({}, EmbeddingMatrix(8)).empty(), "no items, no clusters");

    // Videos never pair with photos, by hash, burst id, time or PDQ; two videos still pair.
    const auto mixed = run({{"same", h(7), 1000, "B", 0.5F, false}, {"same", h(7), 1000, "B", 0.6F, true}}, {unit(8, 0), unit(8, 0)});
    pms_check(mixed.empty(), "a video never pairs with a photo");
    const auto videos = run({{"v", h(7), 0, "", 0.5F, true}, {"v", h(7), 0, "", 0.6F, true}}, {unit(8, 0), unit(8, 1)});
    pms_check(videos.size() == 1 && videos[0].kind == DupKind::Exact && videos[0].best == 1, "two copies of a video pair");

    // Flat frames hash to quality 0: identical hashes and embeddings never cluster by PDQ, even with a good partner.
    const auto flat = run({{"", h(0, 0), 0, "", 0.5F, false}, {"", h(0, 0), 0, "", 0.6F, false}, {"", h(0, 0), 0, "", 0.7F, false}},
                          {unit(8, 0), unit(8, 0), unit(8, 0)});
    pms_check(flat.empty(), "quality-0 frames never cluster by PDQ");
    pms_check(run({{"", h(0, 49), 0, "", 0.5F, false}, {"", h(0, 100), 0, "", 0.6F, false}}, {unit(8, 0), unit(8, 0)}).empty(),
              "both hashes must reach pdq_min_quality");
    const auto at_min = run({{"", h(0, 50), 0, "", 0.5F, false}, {"", h(0, 50), 0, "", 0.6F, false}}, {unit(8, 0), unit(8, 0)});
    pms_check(at_min.size() == 1 && at_min[0].kind == DupKind::Near, "quality exactly at the minimum counts");
    const auto far = run({{"", h(0), 0, "", 0.5F, false}, {"", h(0xFFFF'FFFF), 0, "", 0.6F, false}}, {unit(8, 0), unit(8, 0)});
    pms_check(far.empty(), "PDQ distance above the maximum is no edge, even with equal embeddings");

    // A shared burst id keeps shots that do not look alike (a moving subject); the best shot leads.
    const auto burst = run({{"", h(1), 0, "B1", 0.2F, false}, {"", h(0xFF00), 0, "B1", 0.9F, false}, {"", h(0xFF'0000), 0, "B1", 0.4F, false}},
                           {unit(8, 0), unit(8, 3), unit(8, 6)});
    pms_check(burst.size() == 1 && burst[0].kind == DupKind::Burst && burst[0].best == 1 &&
                  burst[0].others == std::vector<std::size_t>{0, 2}, "a burst id groups shots that do not look alike");

    // Capture-time window: unknown times never pair; window_ms and window_items bound the comparison.
    const std::vector<std::vector<float>> twins{unit(8, 0), unit(8, 0)};
    pms_check(run({{"", h(1), 0, "", 0.5F, false}, {"", h(0xFFFF'FFFF'0000), 0, "", 0.6F, false}}, twins).empty(), "unknown capture times never pair");
    const auto timed = run({{"", h(1), 1000, "", 0.5F, false}, {"", h(0xFFFF'FFFF'0000), 61'000, "", 0.6F, false}}, twins);
    pms_check(timed.size() == 1 && timed[0].kind == DupKind::Burst && timed[0].best == 1, "shots within window_ms pair");
    pms_check(run({{"", h(1), 1000, "", 0.5F, false}, {"", h(0xFFFF'FFFF'0000), 61'001, "", 0.6F, false}}, twins).empty(), "beyond window_ms no pair");
    const std::vector<DupItem> dense{{"", w(0), 1000, "", 0.5F, false}, {"", w(1), 1001, "", 0.5F, false}, {"", w(2), 1002, "", 0.6F, false}};
    const std::vector<std::vector<float>> dense_vectors{unit(8, 0), unit(8, 4), unit(8, 0)};
    pms_check(run(dense, dense_vectors, {.window_items = 1}).empty(), "window_items bounds the comparison");
    const auto two = run(dense, dense_vectors, {.window_items = 2});
    pms_check(two.size() == 1 && two[0].best == 2 && two[0].others == std::vector<std::size_t>{0}, "a wider window finds the pair");
    pms_check(run({{"", h(1), -9'000'000'000'000'000'000, "", 0.5F, false}, {"", h(0xFFFF'FFFF'0000), 9'000'000'000'000'000'000, "", 0.6F, false}}, twins).empty(),
              "extreme capture times do not overflow into a pair");

    // Ruling R2: a burst star drops C (it looks unlike the best shot A); C then pairs with its PDQ copy D as Near.
    // A, B, C are 20 degrees apart in turn: A~B and B~C pass 0.93, A~C (cos 40 = 0.77) fails 0.90.
    const auto angle = [](const float degrees) {
        std::vector<float> v(8, 0.0F);
        v[0] = std::cos(degrees * 0.0174533F);
        v[1] = std::sin(degrees * 0.0174533F);
        return v;
    };
    const auto later = run({{"", h(0x0F), 1000, "", 0.9F, false}, {"", h(0xF0'0000), 2000, "", 0.5F, false}, {"", h(0xFFFF'0000'0000), 3000, "", 0.4F, false},
                            {"", h(0xFFFF'0000'0001), 900'000'000, "", 0.3F, false}},
                           {angle(0), angle(20), angle(40), angle(40)});
    pms_check(later.size() == 2 && later[0].kind == DupKind::Burst && later[0].best == 0 && later[0].others == std::vector<std::size_t>{1} &&
                  later[1].kind == DupKind::Near && later[1].best == 2 && later[1].others == std::vector<std::size_t>{3},
              "a member dropped by a burst star joins a later near-duplicate cluster");

    // Without an embedding row an item joins only through an exact hash or a burst id.
    {
        EmbeddingMatrix one(8);
        one.append(unit(8, 0));
        const std::vector<DupItem> partial{{"", h(3), 0, "", 0.9F, false}, {"", h(3), 0, "", 0.5F, false}, {"x", h(9), 0, "", 0.1F, false}, {"x", h(9), 0, "", 0.2F, false}};
        const auto clusters = find_duplicates(partial, one, {}, 1);
        pms_check(clusters.size() == 1 && clusters[0].kind == DupKind::Exact && clusters[0].best == 3, "missing embeddings only block cosine checks");
    }

    // Hot identical hashes: more members than the 256-neighbor cap still form one component for the star.
    {
        std::vector<DupItem> hot(1000, DupItem{"", h(0xABCD), 0, "", 0.5F, false});
        std::vector<std::vector<float>> vectors;
        for (std::size_t i = 0; i < hot.size(); ++i) vectors.push_back(unit(8, i % 4));
        const auto clusters = run(hot, vectors);
        pms_check(clusters.size() == 1 && clusters[0].kind == DupKind::Near && clusters[0].best == 0 && clusters[0].others.size() == 249 &&
                      clusters[0].others.front() == 4 && clusters[0].others.back() == 996,
                  "the star uses the whole component beyond the neighbor cap");
    }

    // Thread-count invariance on a structured gallery that holds every kind.
    {
        const Gallery g = structured_gallery(1000, 7);
        const auto one = find_duplicates(g.items, g.embeddings, {}, 1);
        bool kinds[3]{};
        std::size_t members = 0;
        for (const auto& c : one) {
            kinds[static_cast<int>(c.kind)] = true;
            members += 1 + c.others.size();
        }
        pms_check(kinds[0] && kinds[1] && kinds[2] && one.size() > 100, "the structured gallery holds every kind");
        std::vector<bool> seen(g.items.size());
        for (const auto& c : one)
            for (std::size_t i = 0; i <= c.others.size(); ++i) {
                const std::size_t m = i == 0 ? c.best : c.others[i - 1];
                pms_check(!seen[m], "each item sits in at most one cluster");
                seen[m] = true;
                pms_check(g.items[m].is_video == g.items[c.best].is_video, "clusters never mix videos and photos");
                pms_check(m == c.best || g.items[m].keep_score <= g.items[c.best].keep_score, "best has the highest keep score");
            }
        pms_check(find_duplicates(g.items, g.embeddings, {}, 2) == one, "2 threads give the same clusters as 1");
        pms_check(find_duplicates(g.items, g.embeddings, {}, 8) == one, "8 threads give the same clusters as 1");
        pms_check(find_duplicates(g.items, g.embeddings, {}, 0) == one, "hardware threads give the same clusters as 1");
        std::cout << "duplicates: structured gallery of " << g.items.size() << " items -> " << one.size() << " clusters, " << members
                  << " members, identical for 1, 2, 8 and hardware threads\n";
    }

    // Scale: random PDQ hashes and unit embeddings (dim 64) over 30 days, with planted near-duplicate pairs a day apart.
#ifdef NDEBUG
    constexpr std::size_t n = 250'000;
#else
    constexpr std::size_t n = 25'000;  // Debug is about 50x slower; the 250k figure is a Release measurement
#endif
    constexpr std::size_t dim = 64, stride = 1000;
    std::mt19937_64 rng(42);
    std::normal_distribution<float> normal;
    std::vector<DupItem> items(n);
    EmbeddingMatrix e(dim);
    std::vector<float> v(dim);
    for (std::size_t i = 0; i < n; ++i) {
        DupItem& item = items[i];
        item.keep_score = static_cast<float>(rng() % 1000) / 1000.0F;
        if (i % stride == 1) {  // a copy of item i - 1: 5 bits flipped, nearly the same embedding, a day later
            item.pdq = items[i - 1].pdq;
            for (int f = 0; f < 5; ++f) item.pdq.bits[static_cast<std::size_t>(f % 4)] ^= std::uint64_t{1} << (rng() % 64);
            item.captured_unix_ms = items[i - 1].captured_unix_ms + 86'400'000;
            for (float& x : v) x += 0.01F * normal(rng);
        } else {
            item.pdq = {{rng(), rng(), rng(), rng()}, 100};
            item.captured_unix_ms = 1'700'000'000'000 + static_cast<std::int64_t>(rng() % 2'592'000'000);
            for (float& x : v) x = normal(rng);
        }
        e.append(v);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto clusters = find_duplicates(items, e, {}, 8);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "duplicates: " << n << " random items, 8 threads: " << took << " s\n";
    pms_check(clusters.size() == n / stride, "every planted copy is found and nothing else");
    for (const auto& c : clusters) {
        const std::size_t base = std::min(c.best, c.others.front()) / stride * stride;
        pms_check(c.kind == DupKind::Near && c.others.size() == 1 && std::min(c.best, c.others[0]) == base && std::max(c.best, c.others[0]) == base + 1,
                  "planted copies pair with their original");
    }
}
}  // namespace

void run_duplicates_tests() {
    using namespace pms;
    pms::EmbeddingMatrix e(8);
    std::vector<DupItem> items;
    auto add = [&](DupItem it, std::vector<float> v) { items.push_back(std::move(it)); e.append(v); };
    add({"aa", h(0b1111), 1000, "", 0.9F, false}, unit(8, 0));          // 0 exact pair, best
    add({"aa", h(0b1111), 2000, "", 0.1F, false}, unit(8, 0));          // 1
    add({"", h(0xF0F0), 50'000, "", 0.5F, false}, unit(8, 2));          // 2 burst by time + cosine
    add({"", h(0xF0F1), 50'500, "", 0.8F, false}, unit(8, 2, 0.05F));   // 3 burst best
    add({"", h(0xAAAA'0000), 900'000'000, "", 0.4F, false}, unit(8, 4)); // 4 near (WhatsApp copy, far in time)
    add({"", h(0xAAAA'0003), 5, "", 0.7F, false}, unit(8, 4, 0.1F));     // 5 near best
    add({"", h(0xFFFF'FFFF'FFFF'FFFF), 7, "", 0.2F, false}, unit(8, 6)); // 6 unrelated
    const auto clusters = find_duplicates(items, e, {}, 2);
    auto find = [&](std::size_t i) -> const DupCluster* {
        for (const auto& c : clusters) { if (c.best == i) return &c; for (auto o : c.others) if (o == i) return &c; }
        return nullptr;
    };
    pms_check(find(0) && find(0)->kind == DupKind::Exact && find(0)->best == 0, "exact pair keeps the better copy");
    pms_check(find(2) && find(2)->kind == DupKind::Burst && find(2)->best == 3, "burst picks the best shot");
    pms_check(find(4) && find(4)->kind == DupKind::Near && find(4)->best == 5, "PDQ finds cross-time copies");
    pms_check(find(6) == nullptr, "unrelated photo stays alone");

    // Chain guard: A~B and B~C by PDQ, but A and C differ by embedding: C must not join A's star.
    pms::EmbeddingMatrix e2(8);
    std::vector<DupItem> chain{{"", h(0), 0, "", 0.9F, false}, {"", h(0xFFFF), 0, "", 0.5F, false}, {"", h(0xFFFF'FFFF), 0, "", 0.1F, false}};
    e2.append(unit(8, 0)); e2.append(unit(8, 0, 0.3F)); e2.append(unit(8, 5));
    const auto chained = find_duplicates(chain, e2, {.pdq_max_distance = 16}, 1);
    for (const auto& c : chained) for (auto o : c.others) pms_check(o != 2, "star clustering drops chained outliers");

    // Scale: 20,000 identical flat-ish hashes finish fast and form bounded work.
    std::vector<DupItem> many(20'000, DupItem{"", h(0x1234), 0, "", 0.5F, false});
    pms::EmbeddingMatrix e3(8);
    for (std::size_t i = 0; i < many.size(); ++i) e3.append(unit(8, i % 8));
    const auto t0 = std::chrono::steady_clock::now();
    (void)find_duplicates(many, e3, {}, 0);
    pms_check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20), "20k identical hashes finish quickly");

    run_extra_duplicates_tests();
}
