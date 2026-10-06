#pragma once

#include <QFutureWatcher>
#include <QHash>
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
#include "phone_client.hpp"
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
    Q_PROPERTY(QString phoneModel READ phoneModel NOTIFY stateChanged)
    Q_PROPERTY(QString qrPayload READ qrPayload NOTIFY stateChanged)
    Q_PROPERTY(QVariantList qrModules READ qrModules NOTIFY stateChanged)
    Q_PROPERTY(int qrSize READ qrSize NOTIFY stateChanged)
    Q_PROPERTY(QString bluetoothAddress READ bluetoothAddress NOTIFY stateChanged)
    Q_PROPERTY(bool pairingReady READ pairingReady NOTIFY stateChanged)
    Q_PROPERTY(bool phoneConnected READ phoneConnected NOTIFY stateChanged)
    Q_PROPERTY(bool galleryPermissionReady READ galleryPermissionReady NOTIFY stateChanged)
    Q_PROPERTY(bool catalogReady READ catalogReady NOTIFY stateChanged)
    Q_PROPERTY(bool bluetoothMode READ bluetoothMode NOTIFY stateChanged)
    Q_PROPERTY(QString vaultPath READ vaultPath CONSTANT)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int seedCount READ seedCount NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)

public:
    enum class Stage { Welcome, Pairing, Syncing, Seeding, Analyzing, Review, Summary, Recovering, Complete, Error };
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
    [[nodiscard]] QString phoneModel() const;
    [[nodiscard]] QString qrPayload() const;
    [[nodiscard]] QVariantList qrModules() const;
    [[nodiscard]] int qrSize() const noexcept;
    [[nodiscard]] QString bluetoothAddress() const;
    [[nodiscard]] bool pairingReady() const noexcept;
    [[nodiscard]] bool phoneConnected() const noexcept;
    [[nodiscard]] bool galleryPermissionReady() const noexcept;
    [[nodiscard]] bool catalogReady() const noexcept;
    [[nodiscard]] bool bluetoothMode() const noexcept;
    [[nodiscard]] QString vaultPath() const;
    [[nodiscard]] QVariantList devices() const;
    [[nodiscard]] int seedCount() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool canCancel() const noexcept;

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void setPhoneModel(const QString& model);
    Q_INVOKABLE void startBluetoothPairing();
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
    void handleRemoteCapabilities(const RemoteCapabilities& capabilities);
    void handleRemoteCatalogPage(const RemoteCatalogPage& page);
    void handleRemoteThumbnail(const QString& asset_id, const QByteArray& bytes);
    void handleRemoteContent(const QString& asset_id, const QUrl& local_url);
    void fetchNextAnalysisPreview();
    void finishAnalysis();
    void requestCurrentPreview();
    void removeCachedVideo();

    ReviewDeckModel* deck_;
    PhoneClient phone_client_;
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
    QHash<QString, QString> previous_remote_labels_;
    Stage stage_{Stage::Welcome};
    QString status_text_{QStringLiteral("Tell us which phone you have, then connect privately")};
    QString error_text_;
    QString device_id_;
    QString device_name_;
    QString coverage_text_{QStringLiteral("USB-visible photos and videos only")};
    QString phone_model_;
    QString cached_video_path_;
    std::size_t analysis_index_{};
    std::size_t remote_synced_count_{};
    double progress_{};
    bool remote_device_{};
    bool phone_connected_{};
    bool gallery_permission_ready_{};
    bool catalog_ready_{};
    QString active_remote_asset_id_;
};

}  // namespace pms::desktop
