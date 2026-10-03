#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
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

    [[nodiscard]] static bool isLocalEndpoint(const QUrl& endpoint);
    [[nodiscard]] static std::expected<RemoteCatalogPage, QString> parseCatalogPage(const QByteArray& body);

    void pair(const QUrl& endpoint, const QString& code, const QString& desktop_name);
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
    [[nodiscard]] QNetworkRequest authenticatedRequest(const QUrl& url) const;
    void pinReply(QNetworkReply* reply);
    void getJson(const QUrl& url, const std::function<void(const QJsonObject&)>& on_success);
    void postJson(
        const QUrl& url,
        const QJsonObject& payload,
        const std::function<void(const QJsonObject&)>& on_success);
    void fetchNextContentChunk();
    void cancelContentDownload();
    void fail(const QString& safe_message);

    QNetworkAccessManager network_;
    QUrl endpoint_;
    QByteArray pinned_digest_;
    QString pending_code_;
    QString token_;
    std::unique_ptr<QSaveFile> content_file_;
    QString content_asset_id_;
    QString content_output_path_;
    std::uint64_t content_total_bytes_{};
    std::uint64_t content_offset_{};
    QPointer<QNetworkReply> content_reply_;
    quint64 session_generation_{};
};

}  // namespace pms::desktop

Q_DECLARE_METATYPE(pms::desktop::RemoteCatalogPage)
Q_DECLARE_METATYPE(pms::desktop::RemoteCapabilities)
Q_DECLARE_METATYPE(pms::desktop::RemoteTrashPrepared)
Q_DECLARE_METATYPE(pms::desktop::RemoteTrashResult)
