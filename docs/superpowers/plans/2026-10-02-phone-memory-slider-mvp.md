# Phone Memory Slider MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a fixture-runnable Qt desktop review application, tested C++ ranking core, versioned local protocol, Android companion scaffold, design-system source, and cross-platform CI foundation.

**Architecture:** A dependency-light C++23 domain core is isolated from Qt and Android so ranking and queue safety can be tested on this Windows host. Qt/QML adapts the core for desktop; Kotlin/Compose adapts Android MediaStore. Protocol and persistence boundaries are explicit so later ONNX/OpenCV/SQLite integrations do not leak into UI state.

**Tech Stack:** C++23, CMake, Catch2, Qt 6 Quick/Multimedia, QML, Kotlin, Jetpack Compose, Gradle, Protobuf schema, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-02-phone-memory-slider-design.md`

## Global Constraints

- Local-only and offline-capable; no telemetry or external inference.
- Android 11+ companion; Windows, macOS, and Linux desktop artifacts.
- Favorites and explicit keeps can never enter a trash batch.
- All deletion is queued and recoverable.
- Target 250,000 assets without render-thread analysis.
- C++ warnings are errors; RAII, const-correctness, and no raw owning pointers.

## Review Focus

- Missing or revoked media permissions must reduce visible coverage without corrupting prior state.
- Duplicate or replayed decisions must remain idempotent.
- Non-finite feature values must not poison ranking order.
- Disconnect during trash preparation must not mark assets trashed.
- A 250,000-item fixture must not require quadratic ranking work.

---

### Task 1: Domain model, ranking, and safe review queue

**Files:** `core/include/pms/*.hpp`, `core/src/*.cpp`, `core/tests/*.cpp`, root `CMakeLists.txt`.

**Interfaces:** Produce `rank_assets(std::span<const AssetFeatures>, RankingPolicy)`, `ReviewQueue::decide`, `ReviewQueue::undo`, and `ReviewQueue::prepare_trash_batch`.

- [ ] Write ranking and queue tests first, including protected assets, NaN handling, idempotency, undo, and deterministic ordering.
- [ ] Run tests and observe missing-interface failures.
- [ ] Implement the minimum domain types, O(n log k) ranking path, and queue state machine.
- [ ] Run the complete core suite with warnings as errors.

### Task 2: Fixture catalog and 250,000-item benchmark

**Files:** `core/include/pms/fixture_catalog.hpp`, `core/src/fixture_catalog.cpp`, `core/tests/fixture_catalog_tests.cpp`, `tools/pms-benchmark.cpp`.

**Interfaces:** Produce `make_fixture_catalog(std::size_t, std::uint64_t)` and a benchmark executable reporting rank duration and memory estimate.

- [ ] Write deterministic fixture and scale tests first.
- [ ] Implement bounded, reproducible fixture generation.
- [ ] Verify 250,000 records rank without quadratic allocation or ordering drift.

### Task 3: Desktop Qt/QML vertical slice

**Files:** `desktop/src/*`, `desktop/qml/*`, `desktop/resources/*`, desktop CMake files.

**Interfaces:** Expose `ReviewDeckModel` roles for asset metadata, reasons, storage, state, media kind, and actions; expose `undo()` and `finishSession()`.

- [ ] Write C++ model tests before implementation.
- [ ] Implement fixture-backed model and application shell.
- [ ] Build the animated review card, photo/video states, keyboard controls, queue summary, theme tokens, and reduced-motion branch.
- [ ] Verify with Qt tests where Qt is available; otherwise run static QML/source checks and require CI to compile.

### Task 4: Versioned local protocol and transport boundaries

**Files:** `protocol/pms.proto`, `core/include/pms/phone_transport.hpp`, `core/tests/protocol_contract_tests.cpp`, `docs/protocol.md`.

**Interfaces:** Define capability, catalog, proxy, streaming, progress, permission, and trash messages with opaque asset IDs and resumable cursors.

- [ ] Write schema/contract tests first.
- [ ] Add protocol v1 and a deterministic in-memory fixture transport.
- [ ] Verify stale cursors, duplicate messages, invalid sizes, and disconnect-before-commit behavior.

### Task 5: Android companion scaffold

**Files:** `android-companion/*`.

**Interfaces:** Produce `MediaCatalogSource`, `PairingSession`, and `TrashCoordinator` boundaries plus Compose screens for pairing, permissions, connection, and trash confirmation.

- [ ] Write JVM tests for permission coverage, catalog paging, and trash preparation first.
- [ ] Implement MediaStore adapters and foreground-session state without silent deletion.
- [ ] Verify with Gradle when an Android SDK is available and through CI otherwise.

### Task 6: Design source, CI, packaging metadata, and release checks

**Files:** `design/tokens.json`, `.github/workflows/ci.yml`, `README.md`, `LICENSE`, packaging metadata.

**Interfaces:** Keep Figma variables, QML tokens, and documentation names aligned; build core on all desktop runners and Android on Linux.

- [x] Create the Figma foundations/components/flow file from the approved design system.
- [x] Add token consistency and no-external-endpoint checks.
- [x] Add Windows, macOS, Linux, and Android CI jobs.
- [x] Run all locally available tests and record unavailable SDK verification separately.
