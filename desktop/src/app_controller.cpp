#include "app_controller.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>
#include <utility>

#include "pms/media_analysis.hpp"
#include "pms/preference_model.hpp"

namespace pms::desktop {
namespace {

constexpr qsizetype seed_byte_limit = 32 * 1024 * 1024;
constexpr std::uint64_t video_preview_byte_limit = 2ULL * 1024ULL * 1024ULL * 1024ULL;

QString default_database_path() {
    auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) {
        directory = QDir::home().filePath(QStringLiteral(".phone-memory-slider"));
    }
    if (!QDir{}.mkpath(directory)) {
        return {};
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            directory,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return {};
    }
#endif
    return directory + QStringLiteral("/catalog.sqlite");
}

float storage_benefit(const std::uint64_t bytes) {
    constexpr auto ceiling = 8.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<float>(std::clamp(std::log1p(static_cast<double>(bytes)) / std::log1p(ceiling), 0.0, 1.0));
}

QString file_digest(const QString& path) {
    QFile file{path};
    QCryptographicHash hash{QCryptographicHash::Sha256};
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

std::vector<ReviewItem> rank_records(
    const std::vector<CatalogRecord>& records,
    const std::vector<FixedEmbedding>& seed_embeddings) {
    std::vector<PreferenceExample> examples;
    examples.reserve(records.size() + seed_embeddings.size());
    for (const auto& seed : seed_embeddings) {
        examples.push_back({seed, PreferenceLabel::Favorite});
    }
    for (const auto& record : records) {
        auto label = PreferenceLabel::Unlabeled;
        if (record.favorite) {
            label = PreferenceLabel::Favorite;
        } else if (record.label == QStringLiteral("keep")) {
            label = PreferenceLabel::Keep;
        } else if (record.label == QStringLiteral("delete")) {
            label = PreferenceLabel::Delete;
        }
        examples.push_back({record.embedding, label});
    }
    PreferenceModel preference;
    preference.train(examples);

    std::vector<MediaObservation> observations;
    std::vector<float> scores;
    observations.reserve(records.size());
    scores.reserve(records.size());
    for (const auto& record : records) {
        scores.push_back(preference.score(record.embedding));
        observations.push_back({
            record.assetId.toStdString(),
            record.embedding,
            record.perceptualHash,
            record.contentDigest.toStdString(),
            record.blurProblem,
            record.exposureProblem,
            record.screenshotLikelihood,
            storage_benefit(record.bytes),
            record.mimeType.startsWith(QStringLiteral("video/")),
            record.favorite,
            record.label == QStringLiteral("keep"),
        });
    }
    const auto analysis = analyze_media(
        observations,
        preference.enabled() ? std::span<const float>{scores} : std::span<const float>{});
    std::vector<ReviewItem> items;
    items.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        if (record.favorite || record.label == QStringLiteral("keep")) {
            continue;
        }
        const auto fallback_name = record.mimeType.startsWith(QStringLiteral("video/")) ? "Video" : "Photo";
        items.push_back({
            analysis[index].features,
            record.mimeType.startsWith(QStringLiteral("video/")) ? MediaKind::Video : MediaKind::Photo,
            record.bytes,
            record.displayName.isEmpty() ? fallback_name : record.displayName.toStdString(),
            analysis[index].reasons,
            {},
        });
    }
    return items;
}

}  // namespace

AppController::AppController(
    ReviewDeckModel* deck,
    QString database_path,
    QString vault_path,
    QObject* parent)
    : QObject(parent),
      deck_(deck),
      store_(database_path.isEmpty() ? default_database_path() : std::move(database_path)),
      vault_(std::move(vault_path)),
      preview_cache_(QDir::tempPath() + QStringLiteral("/PhoneMemorySlider-XXXXXX")),
      scan_watcher_(this),
      encoding_watcher_(this),
      ranking_watcher_(this),
      preview_watcher_(this),
      delete_watcher_(this) {
    Q_ASSERT(deck_ != nullptr);
    QImageReader::setAllocationLimit(256);
    if (!store_.open()) {
        fail(QStringLiteral("The local catalog could not be opened"));
        return;
    }
    if (!vault_.open()) {
        fail(QStringLiteral("The encrypted recovery vault could not be opened"));
        return;
    }

    connect(deck_, &ReviewDeckModel::currentChanged, this, &AppController::requestCurrentPreview);
    connect(deck_, &ReviewDeckModel::decisionApplied, this, [this](const QString& asset_id, const QString& label) {
        const auto record = std::ranges::find(records_, asset_id, &CatalogRecord::assetId);
        if (record == records_.end()) {
            fail(QStringLiteral("The reviewed item is no longer in the local catalog"));
            return;
        }
        decision_label_history_.emplace_back(asset_id, record->label);
        if (label.isEmpty()) {
            return;
        }
        record->label = label;
        if (!store_.updateLabel(device_id_, asset_id, label)) {
            fail(QStringLiteral("Your review choice could not be saved locally"));
        }
    });
    connect(deck_, &ReviewDeckModel::decisionUndone, this, [this](const QString& asset_id) {
        if (decision_label_history_.empty() || decision_label_history_.back().first != asset_id) {
            fail(QStringLiteral("The local review history could not be restored"));
            return;
        }
        const auto previous_label = decision_label_history_.back().second;
        decision_label_history_.pop_back();
        const auto record = std::ranges::find(records_, asset_id, &CatalogRecord::assetId);
        if (record == records_.end()) {
            fail(QStringLiteral("The reviewed item is no longer in the local catalog"));
            return;
        }
        record->label = previous_label;
        if (!store_.updateLabel(device_id_, asset_id, previous_label)) {
            fail(QStringLiteral("Your undone review choice could not be saved locally"));
        }
    });
    connect(&scan_watcher_, &QFutureWatcher<ScanResult>::finished, this, [this] {
        if (stage_ != Stage::Syncing) {
            return;
        }
        const auto result = scan_watcher_.result();
        if (!result.error.isEmpty()) {
            fail(result.error);
            return;
        }
        if (result.assets.empty()) {
            fail(QStringLiteral("No USB-visible photos or videos were found. Cloud-only and hidden albums are not available over cable."));
            return;
        }
        const auto previous_records = store_.assets(device_id_);
        QHash<QString, QString> previous_labels;
        previous_labels.reserve(static_cast<qsizetype>(previous_records.size()));
        for (const auto& record : previous_records) {
            if (record.label == QStringLiteral("keep") || record.label == QStringLiteral("delete")) {
                previous_labels.insert(record.assetId, record.label);
            }
        }
        records_.clear();
        records_.reserve(result.assets.size());
        for (const auto& asset : result.assets) {
            CatalogRecord record{
                .assetId = asset.assetId,
                .mimeType = asset.mimeType,
                .bytes = asset.bytes,
                .modifiedEpochMs = asset.modifiedEpochMs,
                .width = asset.width,
                .height = asset.height,
                .durationMs = asset.durationMs,
                .favorite = asset.favorite,
            };
            record.displayName = asset.displayName;
            record.label = previous_labels.value(asset.assetId);
            records_.push_back(std::move(record));
        }
        const auto revision = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        if (!store_.beginSync(device_id_, revision) ||
            !store_.upsertPage(device_id_, revision, records_, QStringLiteral(""), true)) {
            fail(QStringLiteral("The local catalog could not be saved: %1").arg(store_.errorString()));
            return;
        }
        progress_ = 0.0;
        const auto favorite_count = std::ranges::count(records_, true, &CatalogRecord::favorite);
        setStage(
            Stage::Seeding,
            favorite_count > 0
                ? QStringLiteral("%1 items found, including %2 in Favorites. Analyze now or add more photos you love.")
                      .arg(records_.size())
                      .arg(favorite_count)
                : QStringLiteral("%1 items found. Add 20–50 photos you love so ranking can learn your taste.")
                      .arg(records_.size()));
    });

    connect(&encoding_watcher_, &QFutureWatcher<AnalysisSample>::finished, this, [this] {
        if (stage_ != Stage::Analyzing || analysis_index_ >= records_.size()) {
            return;
        }
        const auto sample = encoding_watcher_.result();
        if (!sample.error.isEmpty()) {
            fail(sample.error);
            return;
        }
        auto& record = records_[analysis_index_];
        record.embedding = sample.encoding.embedding;
        record.perceptualHash = sample.encoding.perceptualHash;
        record.blurProblem = sample.encoding.blurProblem;
        record.exposureProblem = sample.encoding.exposureProblem;
        record.screenshotLikelihood = sample.encoding.screenshotLikelihood;
        record.contentDigest = sample.contentDigest;
        if (!store_.updateAnalysis(
                device_id_,
                record.assetId,
                record.embedding,
                record.perceptualHash,
                record.blurProblem,
                record.exposureProblem,
                record.screenshotLikelihood)) {
            fail(QStringLiteral("Analysis could not be saved locally"));
            return;
        }
        ++analysis_index_;
        progress_ = static_cast<double>(analysis_index_) / static_cast<double>(records_.size());
        status_text_ = QStringLiteral("Analyzing locally · %1 of %2").arg(analysis_index_).arg(records_.size());
        emit stateChanged();
        fetchNextAnalysisPreview();
    });

    connect(&ranking_watcher_, &QFutureWatcher<std::vector<ReviewItem>>::finished, this, [this] {
        if (stage_ != Stage::Analyzing) {
            return;
        }
        deck_->loadItems(ranking_watcher_.result());
        progress_ = 1.0;
        const auto favorite_count = seed_embeddings_.size() +
                                    static_cast<std::size_t>(std::ranges::count(records_, true, &CatalogRecord::favorite));
        const auto explicit_label_count = static_cast<std::size_t>(std::ranges::count_if(
            records_, [](const CatalogRecord& record) {
                return record.label == QStringLiteral("keep") || record.label == QStringLiteral("delete");
            }));
        setStage(
            Stage::Review,
            favorite_count >= 20 || explicit_label_count >= 30
                ? QStringLiteral("Ranked by cleanup value and resemblance to your keep set")
                : QStringLiteral("General cleanup ranking · add 20 favorites next scan to personalize it"));
        requestCurrentPreview();
    });

    connect(&preview_watcher_, &QFutureWatcher<PreviewResult>::finished, this, [this] {
        if (stage_ != Stage::Review) {
            return;
        }
        const auto result = preview_watcher_.result();
        if (result.assetId != deck_->currentAssetId()) {
            if (result.localUrl.isLocalFile()) {
                QFile::remove(result.localUrl.toLocalFile());
            }
            requestCurrentPreview();
            return;
        }
        if (!result.error.isEmpty()) {
            status_text_ = QStringLiteral("Preview unavailable: %1. You can still keep or remove this item.").arg(result.error);
            emit stateChanged();
            return;
        }
        if (result.localUrl.isValid()) {
            cached_video_path_ = result.localUrl.toLocalFile();
            deck_->setCurrentPreview(result.assetId, result.localUrl);
            status_text_ = QStringLiteral("Video cached only for this review session");
            emit stateChanged();
            return;
        }
        const auto path = preview_cache_.filePath(QString::number(qHash(result.assetId)) + QStringLiteral(".jpg"));
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly) || file.write(result.bytes) != result.bytes.size() || !file.commit()) {
            fail(QStringLiteral("The local preview could not be displayed"));
            return;
        }
        deck_->setCurrentPreview(result.assetId, QUrl::fromLocalFile(path));
    });

    connect(&delete_watcher_, &QFutureWatcher<DeleteResult>::finished, this, [this] {
        if (stage_ != Stage::Recovering) {
            return;
        }
        const auto result = delete_watcher_.result();
        if (!result.error.isEmpty()) {
            fail(result.error);
            return;
        }
        progress_ = 1.0;
        setStage(
            Stage::Complete,
            QStringLiteral("%1 items removed after encrypted, SHA-256 verified recovery copies were created")
                .arg(result.removed));
    });

    refreshDevices();
}

AppController::Stage AppController::stage() const noexcept { return stage_; }
QString AppController::statusText() const { return status_text_; }
QString AppController::errorText() const { return error_text_; }
QString AppController::deviceName() const { return device_name_; }
QString AppController::coverageText() const { return coverage_text_; }
QString AppController::vaultPath() const { return vault_.rootPath(); }
int AppController::seedCount() const noexcept { return static_cast<int>(seed_embeddings_.size()); }
double AppController::progress() const noexcept { return progress_; }
bool AppController::canCancel() const noexcept {
    return stage_ == Stage::Syncing || stage_ == Stage::Seeding || stage_ == Stage::Analyzing;
}

QVariantList AppController::devices() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(device_summaries_.size()));
    for (const auto& device : device_summaries_) {
        result.push_back(QVariantMap{{QStringLiteral("name"), device.name}, {QStringLiteral("detail"), device.detail}});
    }
    return result;
}

void AppController::refreshDevices() {
    if (stage_ != Stage::Welcome && stage_ != Stage::Error) {
        return;
    }
    DeviceConnector connector;
    device_summaries_ = connector.devices();
    status_text_ = device_summaries_.empty()
                       ? QStringLiteral("No unlocked phone found yet. Reconnect USB or choose a mounted DCIM folder.")
                       : QStringLiteral("%1 cable-connected device(s) ready").arg(device_summaries_.size());
    emit devicesChanged();
    emit stateChanged();
}

void AppController::scanDevice(const int index) {
    if (stage_ != Stage::Welcome || index < 0 || static_cast<std::size_t>(index) >= device_summaries_.size()) {
        return;
    }
    const auto& device = device_summaries_[static_cast<std::size_t>(index)];
    beginScan(device.id, device.name, QStringLiteral("USB-visible camera media; cloud-only, hidden and protected albums excluded"));
}

void AppController::scanFolder(const QUrl& folder) {
    if (stage_ != Stage::Welcome) {
        return;
    }
    const auto id = DeviceConnector::folderDeviceId(folder);
    if (id.isEmpty()) {
        fail(QStringLiteral("Choose a local or mounted gallery folder"));
        return;
    }
    beginScan(id, QFileInfo{folder.toLocalFile()}.fileName(), QStringLiteral("All supported photos and videos under the selected folder"));
}

void AppController::analyzeWithSeeds(const QList<QUrl>& files) {
    if (stage_ != Stage::Seeding) {
        return;
    }
    seed_embeddings_.clear();
    seed_embeddings_.reserve(static_cast<std::size_t>(std::min<qsizetype>(files.size(), 50)));
    for (const auto& file_url : files | std::views::take(50)) {
        if (!file_url.isLocalFile()) {
            continue;
        }
        const QFileInfo info{file_url.toLocalFile()};
        if (!info.isFile() || info.size() <= 0 || info.size() > seed_byte_limit) {
            continue;
        }
        QFile file{info.absoluteFilePath()};
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QImage image = QImage::fromData(file.readAll());
        if (!image.isNull()) {
            seed_embeddings_.push_back(VisualEncoder::encode(
                image, false, static_cast<std::uint64_t>(std::max<qint64>(0, info.size())), 0).embedding);
        }
    }
    if (seed_embeddings_.empty()) {
        fail(QStringLiteral("None of the selected keep photos could be read"));
        return;
    }
    beginAnalysis();
}

void AppController::analyzeWithoutSeeds() {
    if (stage_ != Stage::Seeding) {
        return;
    }
    seed_embeddings_.clear();
    beginAnalysis();
}

void AppController::cancel() {
    scan_watcher_.cancel();
    encoding_watcher_.cancel();
    ranking_watcher_.cancel();
    preview_watcher_.cancel();
    records_.clear();
    seed_embeddings_.clear();
    decision_label_history_.clear();
    removeCachedVideo();
    deck_->loadItems({});
    device_id_.clear();
    device_name_.clear();
    error_text_.clear();
    progress_ = 0.0;
    setStage(Stage::Welcome, QStringLiteral("Connect a phone by cable, then scan its USB-visible gallery"));
    refreshDevices();
}

void AppController::showSummary() {
    if (stage_ == Stage::Review && deck_->complete()) {
        setStage(Stage::Summary, QStringLiteral("Nothing has been removed. Review the encrypted recovery step first."));
    }
}

void AppController::returnToReview() {
    if (stage_ == Stage::Summary || stage_ == Stage::Error) {
        if (stage_ == Stage::Summary && deck_->complete()) {
            deck_->undo();
        }
        error_text_.clear();
        setStage(Stage::Review, QStringLiteral("Your ranked review is ready"));
    }
}

void AppController::confirmDelete() {
    if (stage_ != Stage::Summary || delete_watcher_.isRunning()) {
        return;
    }
    const auto batch = deck_->pendingTrashBatch();
    if (batch.asset_ids.empty()) {
        setStage(Stage::Complete, QStringLiteral("Nothing was queued for removal"));
        return;
    }
    struct PendingAsset {
        QString id;
        QString name;
        QString digest;
    };
    std::vector<PendingAsset> pending;
    pending.reserve(batch.asset_ids.size());
    for (const auto& id : batch.asset_ids) {
        const auto qid = QString::fromStdString(id);
        const auto record = std::ranges::find(records_, qid, &CatalogRecord::assetId);
        if (record != records_.end() && !record->favorite && record->label != QStringLiteral("keep") &&
            !record->contentDigest.isEmpty()) {
            pending.push_back({qid, record->displayName, record->contentDigest});
        }
    }
    if (pending.size() != batch.asset_ids.size()) {
        fail(QStringLiteral("The selection changed; no files were removed"));
        return;
    }
    progress_ = 0.0;
    setStage(Stage::Recovering, QStringLiteral("Encrypting and verifying recovery copies before removal"));
    const auto device_id = device_id_;
    const auto vault_path = vault_.rootPath();
    delete_watcher_.setFuture(QtConcurrent::run([device_id, vault_path, pending = std::move(pending)] {
        DeviceConnector connector;
        RecoveryVault vault{vault_path};
        DeleteResult result;
        if (!vault.open()) {
            result.error = vault.errorString();
            return result;
        }
        QTemporaryDir staging{QDir::tempPath() + QStringLiteral("/PhoneMemorySlider-delete-XXXXXX")};
        if (!staging.isValid()) {
            result.error = QStringLiteral("A local recovery staging folder could not be created");
            return result;
        }
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const auto& asset = pending[index];
            const auto plain = staging.filePath(QString::number(index) + QStringLiteral(".media"));
            if (!connector.copyTo(device_id, asset.id, plain)) {
                result.error = QStringLiteral("Could not copy %1 into the recovery transaction: %2")
                                   .arg(asset.name, connector.errorString());
                return result;
            }
            if (file_digest(plain) != asset.digest) {
                result.error = QStringLiteral("%1 changed since review and was not removed").arg(asset.name);
                return result;
            }
            VaultEntry entry;
            if (!vault.archive(plain, asset.id, asset.name, entry)) {
                result.error = vault.errorString();
                return result;
            }
            if (connector.sha256(device_id, asset.id) != asset.digest) {
                result.error = QStringLiteral("%1 changed during recovery preparation and was not removed").arg(asset.name);
                return result;
            }
            if (!connector.remove(device_id, asset.id)) {
                result.error = QStringLiteral("%1 remains on the phone; its verified recovery copy is in %2. %3")
                                   .arg(asset.name, vault.rootPath(), connector.errorString());
                return result;
            }
            ++result.removed;
        }
        return result;
    }));
}

void AppController::setStage(const Stage stage, const QString& status) {
    stage_ = stage;
    if (!status.isEmpty()) {
        status_text_ = status;
    }
    emit stateChanged();
}

void AppController::fail(const QString& safe_message) {
    error_text_ = safe_message;
    setStage(Stage::Error, safe_message);
}

void AppController::beginScan(QString device_id, QString device_name, QString coverage) {
    error_text_.clear();
    records_.clear();
    seed_embeddings_.clear();
    decision_label_history_.clear();
    device_id_ = std::move(device_id);
    device_name_ = std::move(device_name);
    coverage_text_ = std::move(coverage);
    progress_ = 0.03;
    setStage(Stage::Syncing, QStringLiteral("Reading the USB-visible media catalog"));
    const auto id = device_id_;
    scan_watcher_.setFuture(QtConcurrent::run([id] {
        DeviceConnector connector;
        ScanResult result;
        result.assets = connector.catalog(id);
        if (result.assets.empty()) {
            result.error = connector.errorString();
        }
        return result;
    }));
}

void AppController::beginAnalysis() {
    analysis_index_ = 0;
    progress_ = 0.0;
    setStage(Stage::Analyzing, QStringLiteral("Starting local visual analysis"));
    fetchNextAnalysisPreview();
}

void AppController::fetchNextAnalysisPreview() {
    if (stage_ != Stage::Analyzing || encoding_watcher_.isRunning()) {
        return;
    }
    if (analysis_index_ >= records_.size()) {
        finishAnalysis();
        return;
    }
    const auto record = records_[analysis_index_];
    const auto device_id = device_id_;
    encoding_watcher_.setFuture(QtConcurrent::run([device_id, record] {
        DeviceConnector connector;
        const auto bytes = connector.preview(device_id, record.assetId, 32 * 1024 * 1024);
        AnalysisSample sample;
        if (bytes.isEmpty()) {
            sample.error = QStringLiteral("Could not read %1 from the phone: %2")
                               .arg(record.displayName, connector.errorString());
            return sample;
        }
        sample.encoding = VisualEncoder::encode(
            QImage::fromData(bytes),
            record.mimeType.startsWith(QStringLiteral("video/")),
            record.bytes,
            record.durationMs);
        sample.contentDigest = connector.sha256(device_id, record.assetId);
        if (sample.contentDigest.isEmpty()) {
            sample.error = QStringLiteral("Could not verify %1 from the phone: %2")
                               .arg(record.displayName, connector.errorString());
        }
        return sample;
    }));
}

void AppController::finishAnalysis() {
    status_text_ = QStringLiteral("Learning your preference profile and ranking locally");
    emit stateChanged();
    ranking_watcher_.setFuture(QtConcurrent::run(
        [records = records_, seeds = seed_embeddings_] { return rank_records(records, seeds); }));
}

void AppController::requestCurrentPreview() {
    if (stage_ != Stage::Review || preview_watcher_.isRunning()) {
        return;
    }
    if (deck_->complete()) {
        showSummary();
        return;
    }
    const auto asset_id = deck_->currentAssetId();
    const auto record = std::ranges::find(records_, asset_id, &CatalogRecord::assetId);
    if (record == records_.end()) {
        fail(QStringLiteral("The selected media is no longer in the local catalog"));
        return;
    }
    removeCachedVideo();
    const auto device_id = device_id_;
    const auto media = *record;
    const auto video_path = preview_cache_.filePath(QString::number(qHash(asset_id)) + QStringLiteral(".video"));
    if (media.mimeType.startsWith(QStringLiteral("video/"))) {
        status_text_ = QStringLiteral("Caching this video locally for playback");
        emit stateChanged();
    }
    preview_watcher_.setFuture(QtConcurrent::run([device_id, media, video_path] {
        DeviceConnector connector;
        PreviewResult result{.assetId = media.assetId};
        if (media.mimeType.startsWith(QStringLiteral("video/"))) {
            if (media.bytes > video_preview_byte_limit) {
                result.error = QStringLiteral("video exceeds the 2 GiB local preview limit");
            } else if (connector.copyTo(device_id, media.assetId, video_path, video_preview_byte_limit)) {
                result.localUrl = QUrl::fromLocalFile(video_path);
            } else {
                result.error = connector.errorString();
            }
            return result;
        }
        result.bytes = connector.preview(device_id, media.assetId, 64 * 1024 * 1024);
        if (result.bytes.isEmpty()) {
            result.error = connector.errorString();
        }
        return result;
    }));
}

void AppController::removeCachedVideo() {
    deck_->clearCurrentPreview();
    if (!cached_video_path_.isEmpty()) {
        QFile::remove(cached_video_path_);
        cached_video_path_.clear();
    }
}

}  // namespace pms::desktop
