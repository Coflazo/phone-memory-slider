# Gallery evaluation

This evaluation exercises the production C++ `VisualEncoder`, `PreferenceModel`, and `analyze_media` path. It does not substitute a public benchmark or real-user study.

## Dataset

The generator fetches 100 random, image-only Wikimedia Commons files, then creates 100 controlled variants across exact copies, resize/re-encode, tone shifts, and crops. `manifest.json` records the source page, author/license fields returned by the Commons API, transform, and SHA-256 for each file. Images and manifests remain git-ignored to avoid redistributing a random changing dataset.

The fetcher is a developer tool and requires network access. The product never calls it.

## Reproduce

```powershell
python scripts/fetch-evaluation-gallery.py --output evaluation/gallery --count 100
cmake --build build --config Release --target pms_evaluate_gallery
build/Release/pms_evaluate_gallery.exe evaluation/gallery evaluation/report.json
```

## Metrics

- **Exact-group recall:** proportion of expected exact-copy groups detected by SHA-256.
- **Near-group recall:** proportion of expected transformed groups connected by the dHash/Hamming grouping path.
- **Duplicate precision estimate:** detected duplicate groups that contain only members sharing an expected source.
- **Synthetic preference AUC:** probability that a positive item outranks a negative item for a controlled warm/red preference.
- **Favorite protection violations:** favorite fixtures that receive non-zero cleanup confidence.

The committed baseline in [`report.json`](report.json) was generated on Windows on 2026-10-04. Timing is machine-specific. Exact and transformed group statistics measure this generated corpus only and must not be presented as general gallery accuracy.
