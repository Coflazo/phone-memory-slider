# Phone Memory Slider

Phone Memory Slider is a local-only gallery cleanup prototype. An Android companion exposes user-authorized media to a cross-platform Qt desktop app; the desktop ranks likely cleanup candidates and presents them as a keyboard- and swipe-driven review deck. Nothing is deleted automatically, and favorites are protected again when the final trash batch is created.

> Current status: production-shaped MVP foundation. Ranking, review state, safety rules, protocol, a fixture-backed animated desktop flow, and the Android MediaStore/permission/trash scaffolds are implemented. The authenticated device transport and on-device gallery-to-desktop wiring are the next integration milestone.

## What is here

- Deterministic C++23 ranking and reversible review queue
- 250,000-item benchmark fixture
- Qt Quick desktop review deck with spring motion, keyboard controls, reduced motion, and system light/dark themes
- Android 11+ companion scaffold with partial-gallery awareness and one system trash confirmation
- Versioned local transport contract in [protocol/pms.proto](protocol/pms.proto)
- Shared design tokens in [design/tokens.json](design/tokens.json)
- Product decisions in [PRODUCT.md](PRODUCT.md) and interaction rules in [DESIGN.md](DESIGN.md)

Design source: [Phone Memory Slider in Figma](https://www.figma.com/design/hVRuncNTA1D6rBb0zYT3CF)

## Safety and privacy

- No accounts, cloud APIs, telemetry, external inference, or internet endpoints in runtime source.
- Favorites and explicitly kept items cannot enter a trash batch.
- Delete choices remain queued and undoable until the Android system confirmation.
- The model uses face presence only; it does not identify or cluster people.
- Cloud-only, locked, secure-folder, and otherwise unavailable media stay out of scope and must be reported to the user.

## Build the verified core

Requirements: CMake 3.24+ and a C++23 compiler.

```powershell
cmake -S . -B build -DPMS_BUILD_DESKTOP=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/tools/Release/pms_benchmark.exe
```

On single-configuration generators, the benchmark is `./build/tools/pms_benchmark`.

## Build the desktop app

Install Qt 6.8+ with Quick, Quick Controls 2, and Multimedia, then run:

```powershell
cmake -S . -B build -DPMS_BUILD_DESKTOP=ON
cmake --build build --config Release --parallel
```

The app currently uses generated fixture media, so its interaction and motion can be reviewed before live device transport is connected.

## Build the Android companion

Requirements: JDK 17, Android SDK 36, and Gradle 8.13.

```powershell
gradle --project-dir android-companion testDebugUnitTest assembleDebug
```

The repository intentionally does not include generated Gradle wrapper binaries. CI pins the Gradle version and builds the companion.

## Remaining integration work

- Authenticated local HTTPS transport and mDNS discovery over Wi-Fi or USB tethering
- Resumable MediaStore catalog/proxy transfer into the desktop client
- Durable SQLite catalog and real thumbnail/video playback
- Local embedding inference and incremental preference updates
- Platform signing and installer polish

Licensed under Apache-2.0. See [LICENSE](LICENSE).
