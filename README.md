# Phone Memory Slider

Phone Memory Slider is a local-only Android-to-desktop gallery cleanup beta. The Android companion exposes only media the user has authorized, the Qt desktop learns a private preference model from favorites, ranks likely cleanup candidates, and presents photos and videos as an undoable swipe deck. Nothing moves until Android shows its recoverable system-trash confirmation.

## Implemented beta

- Authenticated local HTTPS session with a fresh six-digit pairing code and certificate pinning
- Resumable, revision-bound MediaStore catalog pages of at most 1,000 items
- Bounded photo thumbnails and chunked video transfer into a temporary desktop cache
- SQLite catalog persistence with non-destructive schema migration
- Desktop-only fixed visual features, duplicate/quality signals, and preference learning
- Ranked photo/video review with pointer, keyboard, buttons, undo, and reduced motion
- Favorite protection in ranking, trash preparation, and the final Android recheck
- Two-phase, replay-safe recoverable-trash request and result polling
- Windows-first Qt package with macOS/Linux CI coverage and an Android companion APK job

No account, cloud service, telemetry, remote inference, face identity, or permanent-delete fallback is present.

## Use

See [User Guide](docs/USER_GUIDE.md), [Privacy](docs/PRIVACY.md), and [Release Checklist](docs/RELEASE_CHECKLIST.md). The editable visual source is [Phone Memory Slider in Figma](https://www.figma.com/design/hVRuncNTA1D6rBb0zYT3CF).

## Build the desktop app

Requirements: CMake 3.24+, a C++23 compiler, and Qt 6.8+ with Quick, Quick Controls 2, Network, SQL, Concurrent, and Multimedia.

```powershell
cmake -S . -B build -DPMS_BUILD_DESKTOP=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -G ZIP
```

## Build the Android companion

Requirements: JDK 17, Android SDK 36 with accepted SDK licenses, and Gradle 8.13.

```powershell
gradle --project-dir android-companion testDebugUnitTest assembleDebug
```

The repository intentionally excludes generated Gradle wrapper binaries. CI pins Gradle 8.13 and publishes the debug APK as a workflow artifact.

## Verify the ranking budget

```powershell
cmake -S . -B build-core -DPMS_BUILD_DESKTOP=OFF
cmake --build build-core --config Release --parallel
ctest --test-dir build-core -C Release --output-on-failure
./build-core/tools/Release/pms_benchmark.exe
```

Licensed under Apache-2.0. Geist font notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
