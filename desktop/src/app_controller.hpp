#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>

#include <cstddef>
#include <utility>
#include <vector>

#include "catalog_store.hpp"
#include "device_connector.hpp"
#include "recovery_vault.hpp"
#include "review_deck_model.hpp"
#include "visual_encoder.hpp"

namespace pms::desktop {

struct ScanResult {
    std::vector<DeviceMedia> assets;
    QString error;
};

struct AnalysisSample {
    VisualEncoding encoding;
    QString contentDigest;
    QString error;
};

struct PreviewResult {
    QString assetId;
    QByteArray bytes;
    QUrl localUrl;
    QString error;
};

struct DeleteResult {
    std::size_t removed{};
    QString error;
};

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Stage stage READ stage NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged)
    Q_PROPERTY(QString coverageText READ coverageText NOTIFY stateChanged)
    Q_PROPERTY(QString vaultPath READ vaultPath CONSTANT)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int seedCount READ seedCount NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)

public:
    enum class Stage { Welcome, Syncing, Seeding, Analyzing, Review, Summary, Recovering, Complete, Error };
    Q_ENUM(Stage)

    explicit AppController(
        ReviewDeckModel* deck,
        QString database_path = {},
        QString vault_path = {},
        QObject* parent = nullptr);

    [[nodiscard]] Stage stage() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] QString coverageText() const;
    [[nodiscard]] QString vaultPath() const;
    [[nodiscard]] QVariantList devices() const;
    [[nodiscard]] int seedCount() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool canCancel() const noexcept;

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void scanDevice(int index);
    Q_INVOKABLE void scanFolder(const QUrl& folder);
    Q_INVOKABLE void analyzeWithSeeds(const QList<QUrl>& files);
    Q_INVOKABLE void analyzeWithoutSeeds();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void showSummary();
    Q_INVOKABLE void returnToReview();
    Q_INVOKABLE void confirmDelete();

signals:
    void stateChanged();
    void devicesChanged();

private:
    void setStage(Stage stage, const QString& status = {});
    void fail(const QString& safe_message);
    void beginScan(QString device_id, QString device_name, QString coverage);
    void beginAnalysis();
    void fetchNextAnalysisPreview();
    void finishAnalysis();
    void requestCurrentPreview();
    void removeCachedVideo();

    ReviewDeckModel* deck_;
    CatalogStore store_;
    RecoveryVault vault_;
    QTemporaryDir preview_cache_;
    QFutureWatcher<ScanResult> scan_watcher_;
    QFutureWatcher<AnalysisSample> encoding_watcher_;
    QFutureWatcher<std::vector<ReviewItem>> ranking_watcher_;
    QFutureWatcher<PreviewResult> preview_watcher_;
    QFutureWatcher<DeleteResult> delete_watcher_;
    std::vector<DeviceSummary> device_summaries_;
    std::vector<CatalogRecord> records_;
    std::vector<FixedEmbedding> seed_embeddings_;
    std::vector<std::pair<QString, QString>> decision_label_history_;
    Stage stage_{Stage::Welcome};
    QString status_text_{QStringLiteral("Connect a phone by cable, then scan its USB-visible gallery")};
    QString error_text_;
    QString device_id_;
    QString device_name_;
    QString coverage_text_{QStringLiteral("USB-visible photos and videos only")};
    QString cached_video_path_;
    std::size_t analysis_index_{};
    double progress_{};
};

}  // namespace pms::desktop
