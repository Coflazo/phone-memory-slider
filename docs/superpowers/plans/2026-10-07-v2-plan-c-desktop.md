# Plan C: the v2 desktop app

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the Qt Quick app around Plan A (ML core + engine) and Plan B (link): a pipeline that never blocks the UI or aborts on one bad file, honest ETAs, onboarding that verifies each step, a physical swipe deck, duplicate and category views, a recoverable removal transaction for USB, folder and Bluetooth sources, and a vault you can actually restore from.

**Architecture:** v1's 1,127-line `AppController` with 60 properties is replaced by small QObjects, each owning one concern and exposed to QML as a singleton: `Session` (stage + onboarding), `Sources` (USB/folder/companion), `Pipeline` (stages, progress, ETA, errors), `Learning` (training state + curves), `Deck`, `Categories`, `Duplicates`, `Removal`, `Vault`, `Settings`. Data lives in `Library` (SQLite catalog + fp16 embedding file). QML is rebuilt from `DESIGN.md`; `Theme.qml` is generated from `design/tokens.json`.

**Tech Stack:** Qt 6.8.3 (Quick, QuickControls2 Basic, Multimedia, Sql, Concurrent, Bluetooth), C++23, Plan A `pms_core` + `pms_engine`, Plan B `pms::link` + companion client, pywinauto (dev-only, E2E).

**Spec:** `C:\Users\Cagan\.claude\plans\okey-i-want-you-dapper-thacker.md`, `DESIGN.md` (v2), `docs/spikes.md`, Figma file "Phone Memory Slider v2" (mirror of DESIGN.md).

## Global Constraints

- The GUI thread never does file I/O, decoding, hashing, encoding, training or Bluetooth writes.
- One unreadable item is recorded (`ItemError`) and shown in Settings > Problems; the run continues.
- Favorites, seeds and explicit keeps never enter the deck, the duplicate "remove" side, or a removal batch.
- Deletion is always: user confirms -> (USB/folder) copy into the vault, verify by decrypting and hashing, re-check the source still matches what was reviewed, then remove; (Bluetooth) `TrashRequest` bound to (id, size, date_modified, sha256), Android's own dialog decides. Live Photo / AAE / edited-render units are removed together.
- No percentages in the UI unless `Learning.calibrated` is true (ECE on exploration cards <= 0.08).
- Motion per DESIGN.md: springs only on the deck; chrome transitions 120-180 ms opacity/colour; reduced motion from the OS.
- `design/tokens.json` is the source of truth; `Theme.qml` is generated; CI fails on drift.
- No network: no `QtNetwork` link, no URL loading in QML (`Image.source` only `file:` and `image://` providers), FFmpeg `protocol_whitelist=file`.
- Commits as Coflazo, no AI trailers.

## Review Focus

1. The user closes the app mid-analysis and reopens it: completed work (catalog, embeddings, labels, decisions) is kept and the pipeline resumes from where it stopped. Pinned in Tasks 2 and 4.
2. The phone is unplugged or Bluetooth drops mid-review: the deck keeps working on what is already analysed; removal is disabled with a clear reason until the source is back. Pinned in Tasks 3 and 9.
3. A file changed on the phone between review and removal (edited, replaced): it is skipped and reported "Changed since you reviewed it", never removed. Pinned in Task 6.
4. A removal batch partly fails (vault disk full, user declines Android's dialog for some items): every item ends in exactly one reported state; nothing is removed without a verified vault copy (USB) or Android's confirmation (Bluetooth). Pinned in Task 6.
5. Keyboard-only use: every screen is operable with Tab/Enter/arrows, the deck with Left/Right/K/D/Z, and focus is always visible. Pinned in Tasks 7-10.

---

## File structure

```
design/tokens.json                    tools/tokens_to_qml.py            desktop/qml/Theme.qml (generated)
desktop/src/platform/reduced_motion.{hpp,cpp}   desktop/src/platform/key_store.{hpp,cpp}
desktop/src/library/library.{hpp,cpp}           (SQLite schema v2 + embeddings.f16 file)
desktop/src/sources/media_source.hpp            desktop/src/sources/folder_source.{hpp,cpp}
desktop/src/sources/wpd_source.{hpp,cpp}        (from v1 device_connector.cpp)  desktop/src/sources/companion_source.{hpp,cpp}
desktop/src/pipeline/pipeline.{hpp,cpp}         desktop/src/pipeline/learning.{hpp,cpp}
desktop/src/vault/vault.{hpp,cpp}               (from v1 recovery_vault.cpp)    desktop/src/removal/removal.{hpp,cpp}
desktop/src/ui/session.{hpp,cpp} ui/sources_model.{hpp,cpp} ui/deck_model.{hpp,cpp} ui/categories_model.{hpp,cpp}
desktop/src/ui/duplicates_model.{hpp,cpp} ui/vault_model.{hpp,cpp} ui/settings.{hpp,cpp} ui/thumbnail_provider.{hpp,cpp}
desktop/qml/  App.qml TopBar.qml screens/{Onboarding,Connect,Learning,Categories,Review,Duplicates,Done,Settings}.qml
              components/{Button,TagChip,KindBadge,StepRow,ProgressLine,MediaCard,SwipeDeck,WhyPanel,DuplicateRow,QrPanel,CodeConfirm,LossChart,Skeleton}.qml
desktop/tests/ (QtTest per class)    tests/e2e/ (pywinauto UIA)
```

---

### Task 1: Tokens, Theme generation, fonts, reduced motion

**Files:** Modify `design/tokens.json` (v2 values from `DESIGN.md`: colours incl. `brand`, `info`; radius 6/10/16/6; motion {deckSettle 260, deckThrow 220, deckRise 220, chrome 150, strike 220, thumbFade 160, skeletonPeriod 1200, reduced 120}); Create `tools/tokens_to_qml.py` (writes `desktop/qml/Theme.qml` with a header "generated from design/tokens.json, do not edit", `dark`/`light` selected by `Qt.styleHints.colorScheme`), `desktop/src/platform/reduced_motion.{hpp,cpp}`, `desktop/tests/reduced_motion_tests.cpp`; Modify `scripts/check-repository.ps1` (replace v1's token parity with: regenerate Theme.qml in memory and diff against the committed file)

**Interfaces:**
```cpp
// QML singleton "Platform"
class Platform : public QObject { Q_OBJECT QML_ELEMENT QML_SINGLETON
    Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY reducedMotionChanged)
public:
    [[nodiscard]] bool reducedMotion() const noexcept;   // Windows: SPI_GETCLIENTAREAANIMATION == FALSE; macOS: NSWorkspace accessibilityDisplayShouldReduceMotion; Linux: org.gnome.desktop.interface enable-animations / KDE AnimationDurationFactor == 0; env PMS_REDUCED_MOTION=1 forces true (tests)
signals: void reducedMotionChanged();
};
```
- [ ] Test: with `PMS_REDUCED_MOTION=1` the property is true; Theme generation is deterministic (run twice, identical bytes); `check-repository.ps1` fails when Theme.qml is hand-edited (test by editing a temp copy).
- [ ] Commit `feat(ui): generate Theme from tokens and read reduced motion from the OS`.

### Task 2: Library (catalog schema v2 + embeddings file)

**Files:** Create `desktop/src/library/library.{hpp,cpp}`, `desktop/tests/library_tests.cpp`; delete `desktop/src/catalog_store.*` and its test in the same commit (the v1 schema is not migrated: v1 never shipped a release; the old DB file is renamed `catalog-v1.db.bak` on first start).

**Interfaces:**
```cpp
namespace pms::desktop {
struct ItemRow {
    qint64 id{};                 // local row id
    QString source_id;           // stable id from the source (device object id, MediaStore _ID, relative path)
    QString relative_path; QString mime; pms::MediaFormat format{}; pms::MediaKind kind{};
    quint64 bytes{}; quint32 width{}, height{}; double duration_s{}; qint64 captured_ms{}, modified_s{};
    QByteArray sha256;           // empty until known
    bool favorite{}; bool seed{};
    qint64 unit_id{};            // deletion unit (Live Photo, AAE): 0 = alone
    int embedding_row{-1};       // row in embeddings.f16, -1 = not yet embedded
    float sharpness{}, clipped{}, noise{}; std::array<quint64, 4> pdq{}; int pdq_quality{};
    QString error;               // non-empty = unreadable, with reason
};
enum class Decision : quint8 { None, Keep, Delete };
class Library {
public:
    explicit Library(QString directory);                 // <dir>/library.db + <dir>/embeddings.f16
    [[nodiscard]] bool open(QString* error);
    // Upsert by (source_key, source_id); returns ids. Items not present in a full sync are marked removed.
    QList<qint64> upsertItems(const QString& source_key, const QList<ItemRow>& rows, bool full_sync);
    void setAnalysis(qint64 id, float sharpness, float clipped, float noise, const pms::PdqHash& pdq);
    int appendEmbedding(qint64 id, std::span<const float> unit_vector);   // writes fp16 row, stores index
    [[nodiscard]] pms::EmbeddingMatrix loadEmbeddings() const;
    void setError(qint64 id, const QString& reason);
    void setDecision(qint64 id, Decision d, qint64 at_ms);  // keeps history for undo
    [[nodiscard]] std::optional<std::pair<qint64, Decision>> undoLast();
    [[nodiscard]] QList<ItemRow> items(const QString& source_key) const;
    [[nodiscard]] QHash<qint64, Decision> decisions() const;
    void setSourceCursor(const QString& source_key, const QString& store_version, quint64 generation);
    [[nodiscard]] std::pair<QString, quint64> sourceCursor(const QString& source_key) const;
};
}
```
Schema: `meta(user_version=2)`, `sources(key PK, cursor_version, cursor_generation)`, `items(... columns above, removed INTEGER, UNIQUE(source_key, source_id))`, `decisions(item_id, decision, at_ms)` append-only, `errors` folded into `items.error`. WAL, foreign keys, bound parameters only (reuse v1 `bind_uint64` and `ensure_column` helpers from `catalog_store.cpp`). Embeddings file: header `PMSEMB1\0`, u32 dim, then fp16 rows; appends are fsync'd every 256 rows.

- [ ] Tests: upsert twice is idempotent; a full sync without an item marks it removed (Review Focus 1, Plan B Focus 3); embeddings survive reopen (close, reopen, same cosine); undo returns decisions in reverse order and never undoes past the first; reopening after a crash mid-append (truncate the file to a half row) keeps all whole rows.
- [ ] Commit `feat(desktop): library schema v2 with resumable embeddings`.

### Task 3: Media sources (folder, WPD/USB, companion)

**Files:** Create `desktop/src/sources/media_source.hpp`, `folder_source.{hpp,cpp}`, `wpd_source.{hpp,cpp}` (paths handed to Library are storage-relative: strip the MTP storage root such as "Internal shared storage/" so DCIM is the first segment; move and adapt v1 `device_connector.cpp`: keep `create_device`, bounded `enumerate_children`, streaming `read_wpd_resource`, `contained_folder_asset_path`, `HashSink`, `remove`; add the manufacturer/model properties for onboarding), `companion_source.{hpp,cpp}` (wraps Plan B `CompanionClient`); tests `desktop/tests/folder_source_tests.cpp`, `desktop/tests/companion_source_tests.cpp` (with Plan B `fake_phone`). Delete `desktop/src/device_connector.*`, `phone_client.*` and their tests.

**Interfaces:**
```cpp
namespace pms::desktop {
struct SourceItem { QString source_id; QString relative_path; QString mime; quint64 bytes{}; qint64 captured_ms{}, modified_s{};
                    quint32 width{}, height{}; double duration_s{}; bool favorite{}; QByteArray head; /* first <= 64 KiB, may be empty for companion */ };
enum class SourceKind : quint8 { Folder, Usb, Companion };
struct RemovalTarget { QString source_id; quint64 bytes{}; qint64 modified_s{}; QByteArray sha256; };
enum class RemovalOutcome : quint8 { Removed, Trashed, Declined, Changed, Missing, Protected, Failed };
class MediaSource : public QObject { Q_OBJECT
public:
    [[nodiscard]] virtual SourceKind kind() const = 0;
    [[nodiscard]] virtual QString key() const = 0;                 // stable across sessions (device serial / folder path / phone key)
    [[nodiscard]] virtual QString displayName() const = 0;         // "Pixel 9", "Pictures folder"
    [[nodiscard]] virtual bool available() const = 0;              // false when unplugged / link down
    virtual void enumerate() = 0;                                  // emits items in pages, then enumerated()
    // Worker-thread safe: proxy pixels for the pipeline (folder/USB: decode via engine; companion: preview JPEG / keyframes).
    [[nodiscard]] virtual std::vector<pms::engine::Proxy> proxies(const QString& source_id, pms::MediaFormat fmt, int frames) = 0;
    [[nodiscard]] virtual std::optional<QByteArray> sha256(const QString& source_id) = 0;  // streaming hash
    virtual bool copyTo(const QString& source_id, const QString& destination, QString* error) = 0;  // folder/USB only
    virtual void remove(const QList<RemovalTarget>& targets) = 0;                         // emits removed()
signals:
    void itemsFound(QList<pms::desktop::SourceItem> page); void enumerated(bool full_sync);
    void availabilityChanged(bool available);
    void removed(QList<QPair<QString, pms::desktop::RemovalOutcome>> outcomes);
    void problem(QString source_id, QString reason);
};
}
```
- [ ] Tests (folder): a tree with a PNG named `.jpg`, a zero-byte file, a symlink escaping the root, and a 2-level nested folder: kinds come from bytes, the zero-byte file reports `problem`, the escaping link is refused, nothing aborts. Companion: fake phone with 2,500 items -> paged `itemsFound`; `available()` flips false when the pipe closes (Review Focus 2).
- [ ] Commit `feat(desktop): one MediaSource interface for folders, USB and the companion`.

### Task 4: Pipeline and learning

**Files:** Create `desktop/src/pipeline/pipeline.{hpp,cpp}`, `desktop/src/pipeline/learning.{hpp,cpp}`, `desktop/tests/pipeline_tests.cpp` (folder source over `engine/tests/fixtures` + generated items; encoder optional, falls back to a deterministic fake encoder injected in tests)

**Interfaces:**
```cpp
namespace pms::desktop {
class Pipeline : public QObject { Q_OBJECT QML_ELEMENT QML_SINGLETON
    Q_PROPERTY(QVariantList stages READ stages NOTIFY progressChanged)      // [{name, done, total, totalKnown, unit}]
    Q_PROPERTY(int etaPhase READ etaPhase NOTIFY progressChanged)           // pms::EtaPhase
    Q_PROPERTY(QString etaText READ etaText NOTIFY progressChanged)         // pms::eta_text or "Estimating..." / "N found"
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int problemCount READ problemCount NOTIFY progressChanged)
public:
    void setEncoder(std::unique_ptr<EmbeddingBackend> encoder);   // real ImageEncoder or test fake
    Q_INVOKABLE void start(MediaSource* source);                  // resumes from Library state
    Q_INVOKABLE void cancel();
signals: void progressChanged(); void runningChanged(); void analysisReady(); };
class Learning : public QObject { Q_OBJECT QML_ELEMENT QML_SINGLETON
    Q_PROPERTY(int favoriteCount READ favoriteCount NOTIFY changed)
    Q_PROPERTY(QVariantList lossCurve READ lossCurve NOTIFY changed)        // per L-BFGS iteration
    Q_PROPERTY(double heldOutRecall READ heldOutRecall NOTIFY changed)      // 5-fold OOF recall@20%
    Q_PROPERTY(bool calibrated READ calibrated NOTIFY changed)
public:
    void retrain();                                              // taste (Plan A T7) + delete model (T8), off the GUI thread
    void onDecision(qint64 item, Decision d);                    // retrain every 20 decisions
signals: void changed(); void scoresUpdated(); };
}
```
Stages and units: Catalog (items, total unknown until enumerated), Fetch+Hash (bytes; companion skips full hashes except dup candidates), Understand (items: decode -> encode batches of 16 -> quality -> PDQ), Duplicates (pairs checked), Taste (iterations). Bounded queues of 64 items between stages on `QThreadPool` (max threads = cores - 2, at least 2); embedding batches go to one encoder thread. ETA from `pms::EtaEstimator`, ticked every 500 ms on the GUI thread.
- [ ] Tests: a run over 60 fixture items reaches `analysisReady`; one corrupt file increments `problemCount` without stopping; cancel then start again finishes without re-embedding already-embedded items (Review Focus 1); `etaText` goes Estimating -> "about N minutes" -> done; `Learning.retrain` with 0 favorites leaves taste neutral.
- [ ] Commit `feat(desktop): resumable bounded pipeline with live ETA and learning`.

### Task 5: Vault v2

**Files:** Create `desktop/src/vault/vault.{hpp,cpp}`, `desktop/src/platform/key_store.{hpp,cpp}`; tests `desktop/tests/vault_tests.cpp`; delete `desktop/src/recovery_vault.*` + test after porting `crypt_stream` (BCrypt + EVP) and `VerificationSink`.

**Interfaces:**
```cpp
namespace pms::desktop {
class KeyStore { public:  // DPAPI file (Windows), Keychain item (macOS), libsecret (Linux; 0600 file fallback logged)
    [[nodiscard]] static std::optional<QByteArray> loadOrCreate(const QString& name, QString* error); };
struct VaultEntry { QString id; QString source_key; QString source_id; QString original_name; quint64 bytes{}; QByteArray sha256; qint64 archived_ms{}; };
class Vault { public:
    explicit Vault(QString directory);
    [[nodiscard]] bool open(QString* error);
    // Streams the file in, encrypts with AES-256-GCM using AAD = header || entry id, verifies by decrypting into a hash sink,
    // appends an entry to the manifest. The manifest is one AEAD-sealed blob rewritten atomically (QSaveFile).
    [[nodiscard]] std::optional<VaultEntry> archive(const QString& file, const QString& source_key, const QString& source_id, const QString& name, QString* error);
    [[nodiscard]] bool restore(const QString& entry_id, const QString& destination_dir, QString* restored_path, QString* error);
    [[nodiscard]] QList<VaultEntry> entries() const;
    [[nodiscard]] quint64 bytesUsed() const;
    int purgeOlderThan(qint64 cutoff_ms);           // returns count removed
    [[nodiscard]] bool hasRoomFor(quint64 bytes) const;   // free space >= bytes * 1.1
};
}
```
- [ ] Tests: multi-chunk round trip; flipping any header byte, nonce, tag, ciphertext byte, or swapping two payload files fails restore (AAD binds id); editing the manifest file fails `open`; purge removes only old entries; `hasRoomFor` respects 10% headroom; no plaintext `verify-*` files are ever written (port v1 check).
- [ ] Commit `feat(desktop): authenticated vault with restore, retention and platform key store`.

### Task 6: Removal transaction

**Files:** Create `desktop/src/removal/removal.{hpp,cpp}`, `desktop/tests/removal_tests.cpp`

**Interfaces:**
```cpp
namespace pms::desktop {
class Removal : public QObject { Q_OBJECT QML_ELEMENT QML_SINGLETON
    Q_PROPERTY(int queuedItems READ queuedItems NOTIFY changed)  Q_PROPERTY(quint64 queuedBytes READ queuedBytes NOTIFY changed)
    Q_PROPERTY(bool canRemove READ canRemove NOTIFY changed)     Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY finished)   // [{name, outcome, reason}]
public:
    Q_INVOKABLE void removeQueued();     // expands deletion units; USB/folder: vault -> verify -> re-hash source == reviewed sha256 -> remove; companion: TrashRequest chunks
signals: void changed(); void progress(int done, int total); void finished(); };
}
```
- [ ] Tests (folder source + vault in a temp dir, companion via fake phone): a 3-item batch where one file was edited after review -> that one `Changed`, others `Removed`, edited file untouched (Review Focus 3); vault without room -> `canRemove` false with reason, nothing removed (Focus 4); a Live Photo unit removes both files; fake phone declines 2 of 5 -> `Declined` for those, `Trashed` for 3; favorites in the queue are refused as `Protected`; source unavailable -> `canRemove` false, reason "Phone disconnected" (Focus 2).
- [ ] Commit `feat(desktop): removal that verifies before it deletes, for every source`.

### Task 7: App shell, top bar, onboarding, connect (QML + Session + Sources)

**Files:** Create `desktop/src/ui/session.{hpp,cpp}`, `desktop/src/ui/sources_model.{hpp,cpp}`, `desktop/qml/App.qml`, `TopBar.qml`, `screens/Onboarding.qml`, `screens/Connect.qml`, components `Button.qml`, `StepRow.qml`, `QrPanel.qml`, `CodeConfirm.qml`; replace `desktop/src/main.cpp` (no demo env hooks; `--selftest <folder>` headless mode for CI that runs pipeline + learning and exits 0/1); tests `desktop/tests/session_tests.cpp`

**Interfaces:**
```cpp
class Session : public QObject { Q_OBJECT QML_ELEMENT QML_SINGLETON
    Q_PROPERTY(int screen READ screen NOTIFY screenChanged)            // Onboarding, Connect, Learning, Categories, Review, Duplicates, Done, Settings
    Q_PROPERTY(QString phoneModel READ phoneModel WRITE setPhoneModel NOTIFY changed)
    Q_PROPERTY(int phoneFamily READ phoneFamily WRITE setPhoneFamily NOTIFY changed)   // Pixel, Samsung, OtherAndroid, iPhone
    Q_PROPERTY(QVariantList steps READ steps NOTIFY changed)           // [{id, title, help, state: Waiting|Checking|Done}]
    Q_INVOKABLE void go(int screen); };
```
Steps per family (detector in brackets): Android USB: "Plug in with a data cable" [WPD device appears], "On the phone, choose File transfer" [WPD storage enumerable], "Tap Allow on the phone" [no E_ACCESSDENIED]; iPhone USB: "Plug in and unlock", "Tap Trust on the iPhone" [WPD Apple device readable], "Set Photos > Transfer to Mac or PC to Keep Originals" [first HEIC read is HEIF, not JPEG]; Android Bluetooth: "Install Memory Slider from the APK" [first HELLO], "Turn on Bluetooth" [adapter powered], "Scan this code" [handshake established], "Check the code on both screens" [user confirms SAS], "Allow full photo access" [companion capabilities: full media], "Mark favorites or pick 20 photos you love" [favorites >= 20 or seeds >= 20]. A step strikes through only when its detector fires (v1 bug: it struck on typing).
- [ ] Tests: steps advance only on detector signals (fake sources); family switch rebuilds steps; SAS mismatch ("No") unpairs and returns to the QR.
- [ ] QML: TopBar, Onboarding (family choice + model field + StepRow list), Connect (USB list or QrPanel with countdown; CodeConfirm). Keyboard: Tab order top-to-bottom, Enter activates, visible focus ring (`brand` 2 px).
- [ ] Commit `feat(ui): onboarding that checks every step, QR pairing and code confirm`.

### Task 8: Learning and Categories screens

**Files:** Create `screens/Learning.qml`, `screens/Categories.qml`, components `ProgressLine.qml`, `Skeleton.qml`, `LossChart.qml` (QtQuick.Shapes polyline, axes labelled, no fake data: draws `Learning.lossCurve` only), `desktop/src/ui/categories_model.{hpp,cpp}`, `desktop/src/ui/thumbnail_provider.{hpp,cpp}` (`QQuickAsyncImageProvider` reading proxies from a cache file per item; never decodes on the GUI thread)
- [ ] Tests: `CategoriesModel` sorts by expected reclaimable bytes (Plan A `rank_categories`); thumbnail provider returns a placeholder for an unreadable item instead of failing.
- [ ] Commit `feat(ui): learning screen with live progress, honest chart and categories`.

### Task 9: Review deck and duplicates

**Files:** Create `screens/Review.qml`, `screens/Duplicates.qml`, components `SwipeDeck.qml`, `MediaCard.qml`, `KindBadge.qml`, `TagChip.qml`, `WhyPanel.qml`, `DuplicateRow.qml`, `desktop/src/ui/deck_model.{hpp,cpp}`, `desktop/src/ui/duplicates_model.{hpp,cpp}`; tests `deck_model_tests.cpp`, `duplicates_model_tests.cpp`, and a frame-time check `tests/perf/deck_frames.qml` run by CI on Windows (p99 <= 16.7 ms over 100 flings, using the spike harness from `tools/spikes/deck/`)

**Interfaces:**
```cpp
class DeckModel : public QAbstractListModel { Q_OBJECT QML_ELEMENT
    // roles: itemId, imageUrl ("image://proxy/<id>"), videoUrl (file: for USB/folder; empty for companion), keyframeUrls, kind, kindLabel,
    // durationText, sizeText, dateText, tags (<= 3), reasons (strings), nearestFavorites (3 image urls), reason (LikelyDelete|HelpMeLearn|Exploration)
    Q_PROPERTY(int reviewed READ reviewed NOTIFY progress)  Q_PROPERTY(QString queuedText READ queuedText NOTIFY progress)
public:
    Q_INVOKABLE void keep(int row); Q_INVOKABLE void remove(int row); Q_INVOKABLE void undo();
    Q_INVOKABLE void filterCategory(const QString& name);   // "" = all
};
```
Deck model pulls cards from Plan A `next_cards` in batches of 20, keeps 3 live delegates, prefetches proxies for the next 5. `MediaCard` uses DESIGN.md motion values from Theme; one shared `MediaPlayer` (`videoUrl` set 150 ms after settle); companion videos show the keyframe flipbook (frame every 250 ms). Reduced motion swaps throws for a 120 ms cross-fade. Buttons (Delete left of the card, Keep right, Undo below) and keys Right/K keep, Left/D delete, Z undo mirror the gestures (swipe right keeps, left deletes) (Review Focus 5).
- [ ] Tests: keep/remove/undo update Library decisions and `Learning.onDecision`; protected items never appear; filterCategory limits cards; duplicate model marks best shot and lets the user change it; the frame-time harness passes.
- [ ] Commit `feat(ui): physical swipe deck, why panel and duplicate picker`.

### Task 10: Done, Settings, vault restore, problems list

**Files:** Create `screens/Done.qml`, `screens/Settings.qml`, `desktop/src/ui/vault_model.{hpp,cpp}`, `desktop/src/ui/settings.{hpp,cpp}` (paired phones + unpair, model card: name, file size, device "GPU: NVIDIA GeForce RTX 4050 Laptop GPU" or "CPU", offline explanation, problems list, licenses viewer reading `THIRD_PARTY_NOTICES.md`)
- [ ] Tests: restore from `VaultModel` writes the original bytes to the chosen folder; purge updates bytesUsed; unpair removes the pin and the next reconnect without QR is refused.
- [ ] Commit `feat(ui): removal summary, restore from vault and settings`.

### Task 11: Remove v1 leftovers, packaging and offline/package checks

**Files:** Delete `desktop/src/app_controller.*`, `review_deck_model.*`, `visual_encoder.*`, `face_presence_detector.*`, `core/src/{media_analysis,preference_model,review_session}.cpp` + headers + tests (after confirming no references), `desktop/resources/fixture-*.svg`, `desktop/tests/fixtures/test-*.{p12,der}`, `brag-output/`, `tools/pms-evaluate-gallery.cpp`, stale `docs/superpowers/specs/*` and v1 plans; Modify `desktop/CMakeLists.txt` (install models from `.tools/models` via `models/manifest.json`, ORT DLLs, WebGPU plugin DLLs, `heif.dll`, Qt FFmpeg DLLs, LGPL texts into `licenses/`), `scripts/check-package.ps1` (fail on: any `*azure*` provider DLL, `Qt6Network`-dependent plugins `networkinformation`/`tls`/`qmltooling`; require model SHA-256 match, `licenses/LGPL-*.txt`, `THIRD_PARTY_NOTICES.md`; scan OUR binaries' import tables for `ws2_32`/`winhttp`/`wininet` and fail if present), `scripts/check-repository.ps1` (scan `core/`, `engine/`, `desktop/`, `android/` for socket/URL APIs), `.github/workflows/ci.yml` (cache models by SHA, `timeout-minutes` on apt steps, release APK, Linux `--selftest` under `sudo unshare -n` with failing canary), `THIRD_PARTY_NOTICES.md`
- [ ] Commit `chore: remove v1 code paths and package v2 with license and offline checks`.

### Task 12: End-to-end tests

**Files:** Create `tests/e2e/test_folder_flow.py` (pywinauto UIA backend: launch the built app with `--library <temp>`, pick "Use a folder", select the generated small gallery, wait for Learning to finish (ETA text gone), review 30 cards with keyboard (Right/K keep, Left/D delete), open Done, confirm removal, assert files moved to the vault and gone from the folder, restore one from Settings and assert bytes match), `tests/e2e/conftest.py`, `tests/e2e/README.md`; CI job `e2e-windows` (windows-latest, deny-all outbound firewall rule for the app's process during the run)
- [ ] Run locally; record duration; commit `test(e2e): folder flow end to end through the real UI`.

## Self-review notes

- Spec coverage: GUI-thread rule (4, 8), per-item errors (3, 4, 10), protection (6, 9), verified removal for all sources + units (6), ETA/loading (4, 8), onboarding detectors (7), deck motion/keyboard/reduced motion (1, 9), vault restore (5, 10), tokens/Theme (1), offline + package checks (11), E2E (12).
- Depends on Plan A Tasks 1-14 and Plan B Tasks 1-5; Task 11 must run last.
