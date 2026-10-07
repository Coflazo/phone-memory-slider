# S0 spikes

Measurements taken before writing v2, on the development laptop (Ryzen 7 7735HS, RTX 4050 Laptop 6 GB,
Radeon 680M, 16 GB, Windows 11, 165 Hz panel). Each spike has a script under `tools/spikes/` so the
numbers can be reproduced. Dates are 2026-10-07.

## 1. Baseline

v1 (`b172144`) configured and built with the project-local Qt 6.8.3 and VS 2022 Build Tools in 116 s;
`ctest` passed 7/7. Anaconda's Qt 6.9 tools must be off `PATH`, and `Qt6_DIR`, `Qt6QmlTools_DIR`,
`Qt6QuickTools_DIR` passed explicitly. Decision: keep Qt 6 + C++23.

## 4. Image encoder and runtime (`encoder_bench.py`, `taste_spike.py`)

ONNX Runtime 1.30 with the WebGPU plugin EP 0.4.0. The plugin accepts exactly one device, so we pick the
adapter Windows ranks first for high performance (`DxgiHighPerformanceIndex == 0`, the RTX 4050 here).

| Model (SigLIP2 vision tower) | Precision | Device | img/s | Cosine to fp32 |
|---|---|---|---:|---:|
| B/16-224 | fp32 | WebGPU | 61 | 1.000 |
| B/16-224 | fp32 | CPU | 9 | 1.000 |
| B/16-224 | fp16 | WebGPU | 85 | 0.55 |
| B/16-224 | int8 (published) | CPU | 15 | 0.63-0.91 |
| B/16-224 | int8 (ours, per-channel MatMul) | CPU | 15 | 0.61-0.76 |
| B/32-256 | fp32 | WebGPU | 200 | 1.000 |
| B/32-256 | fp32 | CPU | 33 | 1.000 |
| B/32-256 | fp16 | WebGPU | 264 | 0.55 |
| B/32-256 | int8 (ours) | CPU | 49 | 0.58-0.71 |

The WebGPU EP's fp16 kernels and every int8 export we tried bend SigLIP embeddings far off the fp32
ones. fp16 on the CPU EP matches fp32 exactly, so the model is fine and the fp16 GPU kernels are not.
Decision: ship **SigLIP2 B/32-256 in fp32** (one 378 MB file, same embedding space on GPU and CPU).
B/16 misses both speed bars and was not better at taste (below).

Also found: the PyPI `onnxruntime` wheel registers an `AzureExecutionProvider` that can call remote
endpoints. The runtime we ship must not contain it; the package check looks for it.

## 5. Which taste head (`taste_spike.py`, `coco_subset.py`)

Data: 1,182 COCO val2017 images whose dominant subject (largest human-annotated mask area, at least 25%
of the frame) is one of 12 supercategories. Labels come from people, not from the encoder. A persona
"loves" one supercategory; Favorites are `n_P` of those images, everything else is unlabeled. 30 random
splits; scores on held-out items; ROC-AUC (loved vs not) with the 2.5-97.5 percentile band.

B/32-256 fp32, n_P = 20:

| Persona | Centroid cosine | Top-5 kNN | L2-logistic (P vs U) |
|---|---|---|---|
| animal | 0.972 [0.928, 0.985] | 0.969 [0.931, 0.985] | **0.987** [0.965, 0.996] |
| food | 0.892 [0.850, 0.927] | 0.913 [0.872, 0.950] | **0.944** [0.915, 0.965] |
| person | 0.821 [0.769, 0.881] | 0.846 [0.789, 0.907] | **0.974** [0.955, 0.985] |
| vehicle | 0.938 [0.897, 0.971] | 0.960 [0.933, 0.978] | **0.987** [0.972, 0.998] |

Logistic wins every persona at n_P = 20, 50 and 100 (full table in `.tools/spikes/taste_spike_report.json`
when re-run). Decision: L2-logistic positive-vs-unlabeled head; centroid and nearest favorites are used
only to explain a score. These personas are single-supercategory tastes, which is easier than real taste;
the S1 evaluation adds skewed favorites, a never-favorited "useful keep" class and label noise.

Zero-shot tags (6 coarse classes, prompt ensembles): single-label top-1 is 0.73, but 96.7% of predicted
tags name something a person annotated in the photo (a dining table with food gets "food"). Tags are
multi-label in the product, so the gate is tag precision. Decision: multi-label tags with per-tag
thresholds tuned in S1.

## 6. Decoders (`tools/spikes/decode`)

One FFmpeg: the LGPL FFmpeg 7.1 (Lavc 61.19) that Qt Multimedia already ships. Qt gives DLLs only, so
`tools/qt_ffmpeg_sdk.py` stages the public headers from the official `ffmpeg-7.1` tarball and makes MSVC
import libraries from the DLL export tables. libheif 1.23.6 is built as a DLL that decodes HEVC, AVC and
AV1 through that same FFmpeg; libde265, the GPL x265/x264 encoders and runtime plugin loading are off.
Qt's FFmpeg is built with network protocols, so every `avformat_open_input` sets
`protocol_whitelist=file`.

| Input (phone-sized) | Path | Time |
|---|---|---:|
| 12 MP JPEG | `QImageReader::setScaledSize` (DCT scaling) | 11-25 ms |
| 12 MP HEIC | embedded thumbnail item | 6-9 ms |
| 12 MP HEIC | full primary decode | 150-300 ms |
| 1080p H.264, 12 s | 8 keyframes + swscale to 256 px | 0.19 s |
| 4K HEVC, 20 s | 8 keyframes + swscale to 256 px | 0.64 s |

Decision: HEIC uses the thumbnail item when present (iPhones always write one), full decode on worker
threads otherwise. `qtimageformats` (WebP, TIFF) still has to be added to the project Qt.

## 7. Swipe deck frame pacing (`tools/spikes/deck/DeckSpike.qml`)

Three live card delegates, 12 MP JPEGs decoded asynchronously with `sourceSize`, every fourth card a
1080p H.264 or HEVC clip on one shared `MediaPlayer` that starts 150 ms after the swipe settles, 200
scripted flings, frame intervals from `FrameAnimation`:

13,574 frames, p50 6.06 ms (the 165 Hz panel), p95 6.18 ms, p99 8.71 ms, worst 38 ms (a video source
switch). Gate was p99 <= 16.7 ms. Decision: built-in Qt Quick animation is enough; no custom render path.

## 10. Figma

Full seat on a student plan: 200 read calls per day, 10 per minute; writes through `use_figma` are not
counted during the beta. Decision: build the real design system in Figma, with `design/tokens.json` as
the source of truth for code.

## Still open

Spikes 2, 3 and 8 need a phone on the desk (insecure RFCOMM against Windows, how many photos carry
`IS_FAVORITE`, WPD enumeration speed). Spike 9 (tests inside a network namespace with a failing canary)
runs in CI.
