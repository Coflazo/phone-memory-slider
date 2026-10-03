#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <cstddef>
#include <memory>
#include <vector>

#include "catalog_store.hpp"
#include "phone_client.hpp"
#include "review_deck_model.hpp"
#include "visual_encoder.hpp"

namespace pms::desktop {

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Stage stage READ stage NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)

public:
    enum class Stage { Welcome, Pairing, Syncing, Analyzing, Review, Summary, Confirming, Complete, Error };
    Q_ENUM(Stage)

    explicit AppController(ReviewDeckModel* deck, QString database_path = {}, QObject* parent = nullptr);

    [[nodiscard]] Stage stage() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool canCancel() const noexcept;

    Q_INVOKABLE void startPairing(const QString& address, const QString& code);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void showSummary();
    Q_INVOKABLE void returnToReview();
    Q_INVOKABLE void confirmTrash();

signals:
    void stateChanged();

private:
    void setStage(Stage stage, const QString& status = {});
    void fail(const QString& safe_message);
    void handleCapabilities(const RemoteCapabilities& capabilities);
    void handleCatalogPage(const RemoteCatalogPage& page);
    void startAnalysis();
    void fetchNextAnalysisPreview();
    void finishAnalysis();
    void requestCurrentPreview();
    void handleThumbnail(const QString& asset_id, const QByteArray& bytes);
    void handleContent(const QString& asset_id, const QUrl& local_url);
    void removeCachedVideo();

    ReviewDeckModel* deck_;
    PhoneClient client_;
    CatalogStore store_;
    QTemporaryDir preview_cache_;
    QFutureWatcher<VisualEncoding> encoding_watcher_;
    QFutureWatcher<std::vector<ReviewItem>> ranking_watcher_;
    Stage stage_{Stage::Welcome};
    QString status_text_{QStringLiteral("Connect your Android phone to begin")};
    QString error_text_;
    QString device_id_;
    QString device_name_;
    QString active_thumbnail_id_;
    QString cached_video_path_;
    std::vector<CatalogRecord> records_;
    std::size_t analysis_index_{};
    std::size_t synced_count_{};
    double progress_{};
};

}  // namespace pms::desktop
