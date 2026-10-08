# PDQ (vendored)

PDQ is Meta's 256-bit perceptual image hash. Phone Memory Slider uses it to find near-duplicate photos
(WhatsApp copies, re-saves) by Hamming distance. Only the hashing core is vendored; the wrapper is
`core/src/pdq.cpp`.

- Source: https://github.com/facebook/ThreatExchange, directory `pdq/cpp`
- Commit: `85978d7cabdf631c0e4be9cb2be2816b2b9a6911`
- Licence: BSD, `LICENSE` (copied unchanged from the repository root at that commit)
- Algorithm description: https://github.com/facebook/ThreatExchange/blob/main/hashing/hashing.pdf

Built as the static library `pms_pdq` (`core/CMakeLists.txt`) with warnings shown but not fatal.

## Files

Paths are relative to this folder; upstream path is `pdq/cpp/<same path>`. SHA-256 is of the committed
file (LF line endings; check with `git show HEAD:third_party/pdq/<path> | sha256sum`).

| File | SHA-256 | Patched |
|---|---|---|
| `LICENSE` (upstream: repository root `LICENSE`) | `68ecc6aafbd2a205a1077f86127030898f03091b7dae9d9017325a8702d8668f` | no |
| `common/pdqbasetypes.h` | `2f047049969b07351c935e41da1b4c7000af19a354929a3df28d9c4f22b99b4b` | no |
| `common/pdqhashtypes.h` | `a447a4de96cf36cec81e2fcb18d8bac263137501ad27dd914f254178c2053514` | 1, 2 |
| `common/pdqhashtypes.cpp` | `4da002bc0eea6a9db108946df9aceaafb027ef53beac7f16f9429923aeb4b840` | 1 |
| `common/pdqutils.h` | `4657dd34986dd4820564d5b76d403fce92eb00a787ef863e3efbeccc96c17d56` | 1 |
| `common/pdqutils.cpp` | `732bc0fb73e96aa452a4143a348dfc3bf6d1aeb3a330a5a1e7ca24918c4b5207` | 1 |
| `downscaling/downscaling.h` | `1238e00188a29035b3440d4405f64fdbe933c5a042cdfef572e8a5ef07e5923a` | no |
| `downscaling/downscaling.cpp` | `4f7cc18baaebf27650166b4c1569ef8b3faf615abc861d18250573995ba91688` | no |
| `hashing/pdqhashing.h` | `e7e35c86afcfb99f9949cbab16fe7c51cd78da239426a3ff03bba9e071952198` | 1 |
| `hashing/pdqhashing.cpp` | `6b4f8caed63bea61e26539a172cbc15a48c0a2e63d5bbb1c94790b6ac329b208` | 1 |
| `hashing/torben.h` | `d58b25e10d30de4137e53ea869008f911687851a19b5d25c091e56ec5030acf3` | no |
| `hashing/torben.cpp` | `64f1e95edb168a2e07216fae2f7da2f64b3246f6d8c752657eead3142353f09e` | no |

Not vendored: `common/pdqhamming.{h,cpp}` (the .cpp is a 900 KB lookup table), `io/` (needs CImg),
`index/`, `bin/`, `CImg.h`.

## Patches

1. **Include paths.** `#include <pdq/cpp/...>` became `#include <pdq/...>`, so the include root is
   `third_party/`. No other change in those lines.
2. **Hamming without `pdqhamming.h`** (`common/pdqhashtypes.h` only). The include of
   `pdq/cpp/common/pdqhamming.h` is replaced by `<bit>` and `<cstdio>`; `hammingNorm16(w)` in
   `Hash256::hammingNorm` becomes `std::popcount(w)`, and the four `hammingDistance64(a, b)` calls in
   `Hash256::hammingDistance` become `std::popcount(a ^ b)`. Upstream already computed these with a
   bit count (`USE_BUILTIN_POPCOUNT`), so results are unchanged. `<cstdio>` declares the `printf`
   used by the header's `dump*()` helpers, which upstream reached through `pdqhamming.h`'s includes.

To check: `git diff --no-index <upstream pdq/cpp> third_party/pdq` shows only the lines above.

## Cross-check against upstream

The wrapper reproduces upstream's own regression hash bit for bit. The image is not committed here (core has
no JPEG decoder), so the check is manual:

1. Fetch `pdq/data/reg-test-input/dih/bridge-1-original.jpg` (1600x1004) from ThreatExchange at the commit above.
2. Decode it with PIL: `numpy.asarray(Image.open(path).convert("RGB"))`.
3. Resize to 512x512 by nearest neighbour the way upstream's `pdqio.cpp` does through CImg `resize(512, 512)`:
   output pixel (x, y) takes source pixel (`floor(x * W / 512)`, `floor(y * H / 512)`). Index with numpy
   (`img[ys][:, xs]`); PIL's own `NEAREST` resize samples pixel centres and picks different pixels.
4. Hash the contiguous 512x512 RGB buffer with `pms::pdq_hash({data, 512, 512, 512 * 3})` and print
   `bits[3]`, `bits[2]`, `bits[1]`, `bits[0]` as 16 hex digits each.
5. Expected: `d8f8f0cee0f4a84f0637022a078f67f0b36e2ed596621e1d33e6339c4e9c9b22`, quality 100, the
   `bridge-1-original.jpg` line of upstream's `pdq/cpp/reg_test/expected/out`.
