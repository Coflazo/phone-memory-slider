# Phone Memory Slider End-to-End Beta Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the fixture-backed foundation into a verified local Android-to-desktop beta that catalogs real authorized media, learns preference locally, reviews photos and videos, and requests recoverable Android trash.

**Architecture:** The Android foreground service owns MediaStore access, an ephemeral authenticated TLS session, mDNS advertisement, bounded thumbnail/content streaming, and Android's trash confirmation. The Qt desktop pins the presented certificate with the six-digit code, synchronizes metadata and fixed visual features into SQLite, trains the C++ preference model off the UI thread, and drives the existing review queue. The protobuf file remains the canonical semantic schema; beta HTTP endpoints use a versioned JSON representation for simple Android/Qt interoperability.

**Tech Stack:** C++23, Qt 6.8 Quick/Network/SQL/Multimedia/Concurrent, Kotlin 2.2, Android MediaStore/Keystore/NSD, Jetpack Compose, CMake/CTest, Gradle/JUnit.

**Spec:** `docs/superpowers/specs/2026-10-02-phone-memory-slider-design.md`

## Global Constraints

- Android 11+; Windows is the primary desktop, with macOS and Linux build coverage.
- No account, internet service, telemetry, cloud inference, face identity, or permanent-delete fallback.
- Pair only over TLS after the user verifies the six-digit certificate fingerprint; reject unauthenticated and oversized requests.
- Favorites and explicit keeps are protected at ranking, preparation, and Android trash confirmation boundaries.
- Personal weighting stays disabled until 20 favorites or 30 explicit review labels exist.
- Catalog pages contain at most 1,000 assets; originals are streamed and not retained by default.
- Target 250,000 catalog assets, ranking refresh under two seconds after features exist, and steady memory under 1.5 GB.
- Every spatial animation has a reduced-motion equivalent; photos and videos remain operable by pointer and keyboard.

## Review Focus

- A stale catalog cursor after the phone gallery changes restarts sync without mixing revisions.
- Wrong pairing code, mismatched certificate, non-local address, or missing token exposes no catalog data.
- Permission revocation or disconnect mid-scan leaves a resumable, visibly incomplete catalog.
- Favorites added after desktop review never enter the Android trash request.
- Partial trash approval, vanished assets, and user cancellation stay visible and retryable.

---

### Task 1: Fixed media embeddings and preference learning

**Files:**
- Create: `core/include/pms/media_analysis.hpp`
- Create: `core/src/media_analysis.cpp`
- Create: `core/include/pms/preference_model.hpp`
- Create: `core/src/preference_model.cpp`
- Create: `core/tests/media_analysis_tests.cpp`
- Create: `core/tests/preference_model_tests.cpp`
- Modify: `core/CMakeLists.txt`, `core/tests/CMakeLists.txt`, `core/tests/test_main.cpp`, `tools/pms-benchmark.cpp`

**Interfaces:**
- Produces: `FixedEmbedding`, `MediaObservation`, `AnalysisResult analyze_media(...)`, and `PreferenceModel::{train,update,score,enabled}`.
- Consumes: existing `ReasonCode`, `AssetFeatures`, and ranking policy.

- [ ] Write failing tests for clamped finite embeddings, duplicate grouping, favorite/keep protection, the 20-favorite/30-label activation gate, unlabeled assets not becoming negatives, deterministic scores, and online updates.
- [ ] Run the core suite and confirm failures are caused by missing analysis/model interfaces.
- [ ] Implement the fixed 16-value embedding, O(n) hash buckets, reason derivation, and bounded online logistic preference model using contiguous storage and no per-comparison allocation.
- [ ] Extend the 250k benchmark to train and score all assets; require under two seconds and estimated working memory below 1.5 GB.
- [ ] Run Debug and Release core tests and the benchmark; expect zero failures and both budgets satisfied.
- [ ] Commit as `feat: add local media preference model`.

### Task 2: Authenticated Android local session

**Files:**
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/HttpCodec.kt`
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/SessionAuthenticator.kt`
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/TlsIdentity.kt`
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/ConnectionRuntime.kt`
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/LocalGalleryServer.kt`
- Create: `android-companion/app/src/test/kotlin/app/pms/companion/HttpCodecTest.kt`
- Create: `android-companion/app/src/test/kotlin/app/pms/companion/SessionAuthenticatorTest.kt`
- Modify: `PairingForegroundService.kt`, `CompanionBoundaries.kt`, `AndroidManifest.xml`

**Interfaces:**
- Produces: TLS server on an OS-selected port, certificate-derived six-digit code, `SessionAuthenticator`, `ConnectionRuntime.state`, and `_pms._tcp` advertisement.
- Consumes: Android Keystore identity and the existing foreground pairing session.

- [ ] Write failing JVM tests for header/body limits, malformed requests, constant-time code verification behavior, token rotation, unauthorized routes, and non-local peer rejection.
- [ ] Run Android unit tests and confirm the new tests fail for missing production types.
- [ ] Implement the bounded HTTP/1.1 codec, Keystore-backed self-signed TLS identity, session token, local-address enforcement, server lifecycle, and NSD registration.
- [ ] Update the foreground session/UI state so the phone shows address, port, and the certificate-derived code from the running service.
- [ ] Run Android unit tests and assemble the debug APK; expect success.
- [ ] Commit as `feat: add secure Android pairing service`.

### Task 3: Real gallery, proxy, hash, and recoverable-trash API

**Files:**
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/GalleryApi.kt`
- Create: `android-companion/app/src/main/kotlin/app/pms/companion/TrashRequestRuntime.kt`
- Create: `android-companion/app/src/test/kotlin/app/pms/companion/GalleryApiTest.kt`
- Create: `android-companion/app/src/test/kotlin/app/pms/companion/TrashRequestRuntimeTest.kt`
- Modify: `GalleryDomain.kt`, `MediaStoreCatalogSource.kt`, `AndroidTrashCoordinator.kt`, `LocalGalleryServer.kt`, `MainActivity.kt`, `protocol/pms.proto`, `docs/protocol.md`

**Interfaces:**
- Produces: authenticated v1 capabilities/catalog/thumbnail/content/hash/trash endpoints and pollable trash outcomes.
- Consumes: Task 2 request/session primitives and existing MediaStore/trash boundaries.

- [ ] Write failing tests for 1,000-item paging, stable JSON fields, bounded proxy dimensions, single byte ranges, opaque IDs, deduplicated trash IDs, cancellation, partial failure, and a live favorite recheck.
- [ ] Run Android tests and confirm failure for the missing API/runtime behavior.
- [ ] Implement bounded MediaStore thumbnail/range/hash reads, versioned JSON responses, and strict request validation; visual analysis remains on the desktop.
- [ ] Implement activity-delivered `MediaStore.createTrashRequest`, result polling, replay-safe tokens, disconnect invalidation, and visible retryable failures.
- [ ] Run Android tests and assemble the debug APK; expect success.
- [ ] Commit as `feat: expose local Android gallery API`.

### Task 4: Pinned desktop client and durable catalog

**Files:**
- Create: `desktop/src/phone_client.hpp`, `desktop/src/phone_client.cpp`
- Create: `desktop/src/catalog_store.hpp`, `desktop/src/catalog_store.cpp`
- Create: `desktop/src/visual_encoder.hpp`, `desktop/src/visual_encoder.cpp`
- Create: `desktop/src/face_presence_detector.hpp`, `desktop/src/face_presence_detector.cpp`
- Create: `desktop/src/app_controller.hpp`, `desktop/src/app_controller.cpp`
- Create: `desktop/tests/phone_client_tests.cpp`, `desktop/tests/catalog_store_tests.cpp`, `desktop/tests/CMakeLists.txt`
- Modify: `desktop/src/main.cpp`, `desktop/CMakeLists.txt`, root `CMakeLists.txt`

**Interfaces:**
- Produces: `PhoneClient::{pair,syncCatalog,fetchPreview,fetchContent,prepareTrash,commitTrash,pollTrash}`, desktop-only fixed visual encoding/face-presence detection, SQLite `CatalogStore`, and observable `AppController` stages.
- Consumes: Task 3 v1 endpoints and Task 1 embedding/model types.

- [ ] Write failing Qt tests against an in-process TLS fixture for correct/wrong certificate codes, token auth, stale-cursor restart, interrupted sync resume, SQLite replacement by revision, and malformed payload rejection.
- [ ] Run desktop tests and confirm failure for missing client/store/controller types.
- [ ] Implement certificate pinning before ignoring the self-signed error, local-address validation, bounded JSON parsing, cancellable requests, and redacted diagnostics.
- [ ] Implement the fixed desktop visual encoder and face-presence-only detector, plus a transactional SQLite schema for devices, assets, FP16 embeddings, scores, labels, decisions, and cursors; run decoding/analysis/training through bounded QtConcurrent work.
- [ ] Run desktop tests plus the core suite; expect zero failures.
- [ ] Commit as `feat: connect and persist phone galleries`.

### Task 5: Live pairing, scan, review, video, and trash UX

**Files:**
- Create: `desktop/qml/PairingPanel.qml`, `desktop/qml/ScanPanel.qml`, `desktop/qml/ReviewSummary.qml`
- Modify: `desktop/qml/Main.qml`, `desktop/qml/ReviewCard.qml`, `desktop/src/review_deck_model.hpp`, `desktop/src/review_deck_model.cpp`, `desktop/src/app_controller.*`, `desktop/CMakeLists.txt`
- Modify: `android-companion/app/src/main/kotlin/app/pms/companion/MainActivity.kt`

**Interfaces:**
- Produces: one end-to-end user flow from pairing to system-trash result with accessible/reduced-motion states.
- Consumes: Task 4 controller/client/store and the existing swipe/undo review session.

- [ ] Write failing model/controller tests for stage transitions, scan cancellation, preview readiness, video cache cleanup, summary totals/categories/samples, trash cancellation, and retry.
- [ ] Run desktop tests and confirm the intended failures.
- [ ] Replace fixture startup with pairing and progress surfaces; load ranked catalog rows into the deck and keep gesture/keyboard/button parity.
- [ ] Add asynchronous photo previews and bounded temporary video downloads feeding `MediaPlayer`/`VideoOutput`; never decode or train on the render thread.
- [ ] Add final desktop summary and phone-side confirmation/result states; preserve undo until commit begins.
- [ ] Run the Impeccable detector on every edited QML/Compose surface, then run desktop and Android tests/builds.
- [ ] Commit as `feat: complete live gallery review flow`.

### Task 6: End-to-end fixture and failure recovery

**Files:**
- Create: `tools/pms-fixture-phone.cpp`
- Create: `tests/e2e/local_session_test.ps1`
- Modify: `tools/CMakeLists.txt`, `.github/workflows/ci.yml`, `scripts/check-repository.ps1`

**Interfaces:**
- Produces: a deterministic TLS fixture phone and automated desktop-local session smoke test.
- Consumes: Tasks 2–5 protocol semantics and desktop client.

- [ ] Write the failing smoke test for pair, multi-page sync, reconnect, photo fetch, ranged video fetch, protected favorite rejection, trash prepare/commit, and replay.
- [ ] Run it and confirm failure because the fixture executable is absent.
- [ ] Implement the minimal fixture server using generated media and the same contract; add cross-platform CI coverage and sanitizer jobs where supported.
- [ ] Run the smoke test, Debug/Release suites, Android tests, desktop tests, and 250k benchmark; expect zero failures.
- [ ] Commit as `test: verify end-to-end local session`.

### Task 7: Release packaging and honest beta handoff

**Files:**
- Create: `docs/USER_GUIDE.md`, `docs/PRIVACY.md`, `docs/RELEASE_CHECKLIST.md`
- Modify: `README.md`, `.github/workflows/ci.yml`, desktop/Android packaging metadata.

**Interfaces:**
- Produces: signed-build inputs, installable CI artifacts, first-run guidance, privacy disclosure, and a device acceptance checklist.
- Consumes: the complete application and all verification commands.

- [ ] Add release checks that reject internet endpoints, missing license/privacy assets, unsafe Android backup/export settings, or absent platform artifacts.
- [ ] Run the check and confirm it fails before the required release assets/metadata exist.
- [ ] Add Windows/macOS/Linux packages, Android APK artifact, user guide, privacy model, troubleshooting, and explicit hardware acceptance steps.
- [ ] Execute every locally available build/test/smoke/benchmark command and record unavailable physical-device checks separately without claiming them passed.
- [ ] Commit as `docs: prepare end-to-end beta release`.
