#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <vector>

namespace pms::desktop {

struct DeviceSummary {
    QString id;
    QString name;
    QString detail;
    bool canDelete{};
};

struct DeviceMedia {
    QString assetId;
    QString displayName;
    QString mimeType;
    std::uint64_t bytes{};
    std::uint64_t modifiedEpochMs{};
    int width{};
    int height{};
    std::uint64_t durationMs{};
    bool favorite{};
};

class DeviceConnector final {
public:
    [[nodiscard]] std::vector<DeviceSummary> devices();
    [[nodiscard]] std::vector<DeviceMedia> catalog(const QString& device_id);
    [[nodiscard]] QByteArray preview(const QString& device_id, const QString& asset_id, qsizetype byte_limit);
    [[nodiscard]] QString sha256(const QString& device_id, const QString& asset_id);
    [[nodiscard]] bool copyTo(
        const QString& device_id,
        const QString& asset_id,
        const QString& destination,
        std::uint64_t byte_limit = 0);
    [[nodiscard]] bool remove(const QString& device_id, const QString& asset_id);
    [[nodiscard]] QString errorString() const;

    [[nodiscard]] static QString folderDeviceId(const QUrl& folder);
    [[nodiscard]] static bool isFolderDevice(const QString& device_id);

private:
    void setError(QString error);

    QString error_;
};

}  // namespace pms::desktop
