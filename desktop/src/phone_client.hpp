#pragma once

#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace pms::desktop {

struct RemoteCatalogAsset {
    QString assetId;
    QString mimeType;
    std::uint64_t bytes{};
    std::uint64_t modifiedEpochMs{};
    int width{};
    int height{};
    std::uint64_t durationMs{};
    bool favorite{};
};

struct RemoteCatalogPage {
    std::uint64_t revision{};
    std::vector<RemoteCatalogAsset> assets;
    QString nextCursor;
    bool complete{};
};

struct RemoteCapabilities {
    QString deviceId;
    QString deviceName;
    QString permissionCoverage;
    bool supportsFavorites{};
    bool supportsRecoverableTrash{};
};

struct RemoteTrashPrepared {
    QString token;
    std::vector<QString> assetIds;
    std::uint64_t totalBytes{};
};

struct RemoteTrashResult {
    QString token;
    std::vector<QString> trashedIds;
    std::vector<QString> failedIds;
    bool userCancelled{};
    bool pending{};
};

class PhoneClient final : public QObject {
    Q_OBJECT

public:
    explicit PhoneClient(QObject* parent = nullptr);

    [[nodiscard]] QString qrPayload() const;
    [[nodiscard]] QVariantList qrModules() const;
    [[nodiscard]] int qrSize() const noexcept;
    [[nodiscard]] QString bluetoothAddress() const;
    [[nodiscard]] bool listening() const noexcept;

    [[nodiscard]] static QString makePairingPayload(
        const QString& address,
        const QString& secret,
        const QString& computer_name);
    [[nodiscard]] static bool validPairingHandshake(const QByteArray& line, const QString& secret) noexcept;
    [[nodiscard]] static std::expected<RemoteCatalogPage, QString> parseCatalogPage(const QByteArray& body);

    void startPairing();
    void fetchCapabilities();
    void fetchCatalog(const QString& cursor = {}, int limit = 1000);
    void fetchThumbnail(const QString& asset_id, int max_edge);
    void fetchContent(const QString& asset_id, std::uint64_t total_bytes, const QString& output_path);
    void cancelContent();
    void prepareTrash(const std::vector<QString>& asset_ids);
    void commitTrash(const QString& preparation_token);
    void pollTrash(const QString& preparation_token);
    void disconnectPhone();

signals:
    void pairingReady();
    void paired();
    void capabilitiesReceived(const pms::desktop::RemoteCapabilities& capabilities);
    void catalogPageReceived(const pms::desktop::RemoteCatalogPage& page);
    void catalogInvalidated();
    void thumbnailReceived(const QString& asset_id, const QByteArray& bytes);
    void contentReady(const QString& asset_id, const QUrl& local_url);
    void contentProgress(const QString& asset_id, double progress);
    void trashPrepared(const pms::desktop::RemoteTrashPrepared& prepared);
    void trashResultReceived(const pms::desktop::RemoteTrashResult& result);
    void requestFailed(const QString& safe_message);

private:
    struct PendingRequest {
        QByteArray bytes;
        qsizetype maxBodyBytes{};
        std::function<void(int, const QHash<QByteArray, QByteArray>&, const QByteArray&)> handler;
    };

    void acceptConnection();
    void readSocket();
    void readResponse();
    void enqueue(
        const QByteArray& method,
        const QString& target,
        const QByteArray& body,
        const QHash<QByteArray, QByteArray>& headers,
        qsizetype max_body_bytes,
        std::function<void(int, const QHash<QByteArray, QByteArray>&, const QByteArray&)> handler);
    void sendNext();
    void getJson(const QString& target, const std::function<void(const QJsonObject&)>& on_success);
    void postJson(
        const QString& target,
        const QJsonObject& payload,
        const std::function<void(const QJsonObject&)>& on_success);
    void fetchNextContentChunk();
    void cancelContentDownload();
    void resetResponseState();
    void fail(const QString& safe_message);

    std::unique_ptr<QBluetoothServer> server_;
    QBluetoothServiceInfo service_info_;
    QBluetoothSocket* socket_{};
    bool authenticated_{};
    QByteArray handshake_buffer_;
    QString bluetooth_address_;
    QString secret_;
    QString qr_payload_;
    QVariantList qr_modules_;
    int qr_size_{};
    std::deque<PendingRequest> requests_;
    std::optional<PendingRequest> active_request_;
    QByteArray response_buffer_;
    qint64 response_body_size_{-1};
    int response_status_{};
    QHash<QByteArray, QByteArray> response_headers_;
    std::unique_ptr<QSaveFile> content_file_;
    QString content_asset_id_;
    QString content_output_path_;
    std::uint64_t content_total_bytes_{};
    std::uint64_t content_offset_{};
    quint64 content_generation_{};
};

}  // namespace pms::desktop

Q_DECLARE_METATYPE(pms::desktop::RemoteCatalogPage)
Q_DECLARE_METATYPE(pms::desktop::RemoteCapabilities)
Q_DECLARE_METATYPE(pms::desktop::RemoteTrashPrepared)
Q_DECLARE_METATYPE(pms::desktop::RemoteTrashResult)
