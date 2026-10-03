#include "app_controller.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "pms/media_analysis.hpp"
#include "pms/preference_model.hpp"

namespace pms::desktop {
namespace {

QString default_database_path() {
    auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/PhoneMemorySlider");
    }
    QDir{}.mkpath(directory);
    return directory + QStringLiteral("/catalog.sqlite");
}

float storage_benefit(const std::uint64_t bytes) {
    constexpr auto ceiling = 8.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<float>(std::clamp(std::log1p(static_cast<double>(bytes)) / std::log1p(ceiling), 0.0, 1.0));
}

std::vector<ReviewItem> rank_records(const std::vector<CatalogRecord>& records) {
    std::vector<PreferenceExample> examples;
    examples.reserve(records.size());
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
        const auto score = preference.score(record.embedding);
        scores.push_back(score);
        observations.push_back({
            record.assetId.toStdString(),
            record.embedding,
            record.perceptualHash,
            {},
            record.blurProblem,
            record.exposureProblem,
            record.screenshotLikelihood,
            storage_benefit(record.bytes),
            record.mimeType.startsWith(QStringLiteral("video/")),
            record.favorite,
            record.label == QStringLiteral("keep"),
        });
    }
    const auto analysis = analyze_media(observations, preference.enabled() ? std::span<const float>{scores} : std::span<const float>{});
    std::vector<ReviewItem> items;
    items.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        if (record.favorite || record.label == QStringLiteral("keep")) {
            continue;
        }
        items.push_back({
            analysis[index].features,
            record.mimeType.startsWith(QStringLiteral("video/")) ? MediaKind::Video : MediaKind::Photo,
            record.bytes,
            record.mimeType.startsWith(QStringLiteral("video/")) ? "Video" : "Photo",
            analysis[index].reasons,
            {},
        });
    }
    return items;
}

}  // namespace

AppController::AppController(ReviewDeckModel* deck, QString database_path, QObject* parent)
    : QObject(parent),
      deck_(deck),
      client_(this),
      store_(database_path.isEmpty() ? default_database_path() : std::move(database_path)),
      preview_cache_(QDir::tempPath() + QStringLiteral("/PhoneMemorySlider-XXXXXX")),
      encoding_watcher_(this),
      ranking_watcher_(this) {
    Q_ASSERT(deck_ != nullptr);
    if (!store_.open()) {
        fail(QStringLiteral("The local catalog could not be opened"));
    }
    connect(&client_, &PhoneClient::paired, this, [this] {
        setStage(Stage::Syncing, QStringLiteral("Reading phone capabilities"));
        client_.fetchCapabilities();
    });
    connect(&client_, &PhoneClient::capabilitiesReceived, this, &AppController::handleCapabilities);
    connect(&client_, &PhoneClient::catalogPageReceived, this, &AppController::handleCatalogPage);
    connect(&client_, &PhoneClient::catalogInvalidated, this, [this] {
        if (stage_ != Stage::Syncing) {
            return;
        }
        records_.clear();
        synced_count_ = 0;
        status_text_ = QStringLiteral("Gallery changed; restarting the catalog safely");
        emit stateChanged();
        client_.fetchCatalog({}, 1000);
    });
    connect(&client_, &PhoneClient::thumbnailReceived, this, &AppController::handleThumbnail);
    connect(&client_, &PhoneClient::contentReady, this, &AppController::handleContent);
    connect(&client_, &PhoneClient::contentProgress, this, [this](const QString& asset_id, const double progress) {
        if (stage_ == Stage::Review && asset_id == deck_->currentAssetId()) {
            status_text_ = QStringLiteral("Caching video locally for playback: %1%").arg(qRound(progress * 100.0));
            emit stateChanged();
        }
    });
    connect(&client_, &PhoneClient::requestFailed, this, &AppController::fail);
    connect(deck_, &ReviewDeckModel::currentChanged, this, &AppController::requestCurrentPreview);
    connect(&encoding_watcher_, &QFutureWatcher<VisualEncoding>::finished, this, [this] {
        if (stage_ != Stage::Analyzing || analysis_index_ >= records_.size()) {
            return;
        }
        const auto encoded = encoding_watcher_.result();
        auto& record = records_[analysis_index_];
        record.embedding = encoded.embedding;
        record.perceptualHash = encoded.perceptualHash;
        record.blurProblem = encoded.blurProblem;
        record.exposureProblem = encoded.exposureProblem;
        record.screenshotLikelihood = encoded.screenshotLikelihood;
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
        progress_ = records_.empty() ? 1.0 : static_cast<double>(analysis_index_) / static_cast<double>(records_.size());
        status_text_ = QStringLiteral("Analyzing %1 of %2").arg(analysis_index_).arg(records_.size());
        emit stateChanged();
        fetchNextAnalysisPreview();
    });
    connect(&ranking_watcher_, &QFutureWatcher<std::vector<ReviewItem>>::finished, this, [this] {
        if (stage_ != Stage::Analyzing) {
            return;
        }
        deck_->loadItems(ranking_watcher_.result());
        progress_ = 1.0;
        setStage(Stage::Review, QStringLiteral("Your ranked review is ready"));
        requestCurrentPreview();
    });
    connect(&client_, &PhoneClient::trashPrepared, this, [this](const RemoteTrashPrepared& prepared) {
        setStage(Stage::Confirming, QStringLiteral("Approve the recoverable trash prompt on your phone"));
        client_.commitTrash(prepared.token);
    });
    connect(&client_, &PhoneClient::trashResultReceived, this, [this](const RemoteTrashResult& result) {
        if (result.pending) {
            QTimer::singleShot(1000, this, [this, token = result.token] { client_.pollTrash(token); });
            return;
        }
        if (result.userCancelled || !result.failedIds.empty()) {
            fail(result.userCancelled ? QStringLiteral("Trash confirmation was cancelled; nothing was permanently deleted")
                                      : QStringLiteral("Some items could not be moved to trash; you can retry"));
            return;
        }
        setStage(Stage::Complete, QStringLiteral("Selected items are now in Android's recoverable trash"));
    });
}

AppController::Stage AppController::stage() const noexcept { return stage_; }
QString AppController::statusText() const { return status_text_; }
QString AppController::errorText() const { return error_text_; }
QString AppController::deviceName() const { return device_name_; }
double AppController::progress() const noexcept { return progress_; }
bool AppController::canCancel() const noexcept {
    return stage_ == Stage::Pairing || stage_ == Stage::Syncing || stage_ == Stage::Analyzing;
}

void AppController::startPairing(const QString& address, const QString& code) {
    auto normalized = address.trimmed();
    if (!normalized.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        normalized.prepend(QStringLiteral("https://"));
    }
    const QUrl endpoint{normalized};
    if (!PhoneClient::isLocalEndpoint(endpoint) || code.size() != 6) {
        fail(QStringLiteral("Enter the local HTTPS address and six-digit code shown on your phone"));
        return;
    }
    error_text_.clear();
    progress_ = 0.0;
    setStage(Stage::Pairing, QStringLiteral("Verifying the phone certificate"));
    client_.pair(endpoint, code, QCoreApplication::applicationName());
}

void AppController::cancel() {
    client_.disconnectPhone();
    encoding_watcher_.cancel();
    ranking_watcher_.cancel();
    records_.clear();
    removeCachedVideo();
    deck_->loadItems({});
    device_id_.clear();
    device_name_.clear();
    error_text_.clear();
    progress_ = 0.0;
    setStage(Stage::Welcome, QStringLiteral("Connect your Android phone to begin"));
}

void AppController::showSummary() {
    if (stage_ == Stage::Review && deck_->complete()) {
        setStage(Stage::Summary, QStringLiteral("Review the selection before Android asks for confirmation"));
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

void AppController::confirmTrash() {
    if (stage_ != Stage::Summary) {
        return;
    }
    const auto batch = deck_->pendingTrashBatch();
    if (batch.asset_ids.empty()) {
        setStage(Stage::Complete, QStringLiteral("Nothing was queued for trash"));
        return;
    }
    std::vector<QString> ids;
    ids.reserve(batch.asset_ids.size());
    std::ranges::transform(batch.asset_ids, std::back_inserter(ids), [](const std::string& id) {
        return QString::fromStdString(id);
    });
    setStage(Stage::Confirming, QStringLiteral("Rechecking favorites and preparing Android trash"));
    client_.prepareTrash(ids);
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

void AppController::handleCapabilities(const RemoteCapabilities& capabilities) {
    if (capabilities.permissionCoverage == QStringLiteral("denied")) {
        fail(QStringLiteral("Grant photo and video access on the phone, then reconnect"));
        return;
    }
    device_id_ = capabilities.deviceId;
    device_name_ = capabilities.deviceName;
    records_.clear();
    const auto state = store_.syncState(device_id_);
    if (state.exists && !state.complete) {
        records_ = store_.assets(device_id_);
    }
    synced_count_ = records_.size();
    status_text_ = QStringLiteral("Syncing catalog from %1").arg(device_name_);
    emit stateChanged();
    client_.fetchCatalog(state.exists && !state.complete ? state.cursor : QString{}, 1000);
}

void AppController::handleCatalogPage(const RemoteCatalogPage& page) {
    const auto state = store_.syncState(device_id_);
    if (!state.exists || state.revision != page.revision) {
        if (!store_.beginSync(device_id_, page.revision)) {
            fail(QStringLiteral("The local catalog could not start synchronization"));
            return;
        }
        records_.clear();
    }
    std::vector<CatalogRecord> page_records;
    page_records.reserve(page.assets.size());
    for (const auto& asset : page.assets) {
        page_records.push_back({
            .assetId = asset.assetId,
            .mimeType = asset.mimeType,
            .bytes = asset.bytes,
            .modifiedEpochMs = asset.modifiedEpochMs,
            .width = asset.width,
            .height = asset.height,
            .durationMs = asset.durationMs,
            .favorite = asset.favorite,
        });
    }
    if (!store_.upsertPage(device_id_, page.revision, page_records, page.nextCursor, page.complete)) {
        fail(QStringLiteral("The catalog page could not be saved locally"));
        return;
    }
    synced_count_ += page.assets.size();
    status_text_ = QStringLiteral("Received %1 items").arg(synced_count_);
    emit stateChanged();
    if (!page.complete) {
        client_.fetchCatalog(page.nextCursor, 1000);
        return;
    }
    records_ = store_.assets(device_id_);
    startAnalysis();
}

void AppController::startAnalysis() {
    analysis_index_ = 0;
    progress_ = 0.0;
    setStage(Stage::Analyzing, records_.empty() ? QStringLiteral("No accessible photos or videos were found")
                                                : QStringLiteral("Starting local visual analysis"));
    if (records_.empty()) {
        finishAnalysis();
        return;
    }
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
    active_thumbnail_id_ = records_[analysis_index_].assetId;
    client_.fetchThumbnail(active_thumbnail_id_, 96);
}

void AppController::finishAnalysis() {
    status_text_ = QStringLiteral("Learning your local preference model and ranking the gallery");
    emit stateChanged();
    ranking_watcher_.setFuture(QtConcurrent::run([records = records_] { return rank_records(records); }));
}

void AppController::requestCurrentPreview() {
    if (stage_ != Stage::Review) {
        return;
    }
    if (deck_->complete()) {
        showSummary();
        return;
    }
    active_thumbnail_id_ = deck_->currentAssetId();
    const auto record = std::ranges::find(records_, active_thumbnail_id_, &CatalogRecord::assetId);
    if (record == records_.end()) {
        fail(QStringLiteral("The selected media is no longer in the local catalog"));
        return;
    }
    if (record->mimeType.startsWith(QStringLiteral("video/"))) {
        removeCachedVideo();
        const auto suffix = record->mimeType == QStringLiteral("video/webm") ? QStringLiteral(".webm")
                           : record->mimeType == QStringLiteral("video/quicktime") ? QStringLiteral(".mov")
                                                                                   : QStringLiteral(".mp4");
        cached_video_path_ = preview_cache_.filePath(QString::number(qHash(active_thumbnail_id_)) + suffix);
        status_text_ = QStringLiteral("Caching video locally for playback");
        emit stateChanged();
        client_.fetchContent(active_thumbnail_id_, record->bytes, cached_video_path_);
        return;
    }
    removeCachedVideo();
    client_.fetchThumbnail(active_thumbnail_id_, 1600);
}

void AppController::handleThumbnail(const QString& asset_id, const QByteArray& bytes) {
    if (asset_id != active_thumbnail_id_) {
        return;
    }
    if (stage_ == Stage::Analyzing && analysis_index_ < records_.size()) {
        const auto video = records_[analysis_index_].mimeType.startsWith(QStringLiteral("video/"));
        const auto size = records_[analysis_index_].bytes;
        const auto duration = records_[analysis_index_].durationMs;
        encoding_watcher_.setFuture(QtConcurrent::run([bytes, video, size, duration] {
            return VisualEncoder::encode(QImage::fromData(bytes), video, size, duration);
        }));
        return;
    }
    if (stage_ == Stage::Review && asset_id == deck_->currentAssetId() && preview_cache_.isValid()) {
        const auto path = preview_cache_.filePath(QString::number(qHash(asset_id)) + QStringLiteral(".jpg"));
        QSaveFile file{path};
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
            deck_->setCurrentPreview(asset_id, QUrl::fromLocalFile(path));
        }
    }
}

void AppController::handleContent(const QString& asset_id, const QUrl& local_url) {
    if (stage_ != Stage::Review || asset_id != deck_->currentAssetId()) {
        if (local_url.isLocalFile()) {
            QFile::remove(local_url.toLocalFile());
        }
        return;
    }
    status_text_ = QStringLiteral("Video ready — swipe left to queue or right to keep");
    deck_->setCurrentPreview(asset_id, local_url);
    emit stateChanged();
}

void AppController::removeCachedVideo() {
    client_.cancelContent();
    deck_->clearCurrentPreview();
    if (!cached_video_path_.isEmpty()) {
        QFile::remove(cached_video_path_);
        cached_video_path_.clear();
    }
}

}  // namespace pms::desktop
