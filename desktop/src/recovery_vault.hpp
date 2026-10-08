#pragma once

#include <QString>

#include <cstdint>

class QIODevice;

namespace pms::desktop {

struct RecoveryVaultEntry {
    QString id;
    QString originalName;
    QString encryptedPath;
    QString sha256;
    std::uint64_t bytes{};
};

class RecoveryVault final {
public:
    explicit RecoveryVault(QString root = {});

    [[nodiscard]] bool open();
    [[nodiscard]] bool archive(
        const QString& plain_path,
        const QString& asset_id,
        const QString& original_name,
        RecoveryVaultEntry& entry);
    [[nodiscard]] bool restore(const RecoveryVaultEntry& entry, const QString& destination);
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString rootPath() const;

private:
    [[nodiscard]] bool loadOrCreateKey();
    [[nodiscard]] bool encryptFile(const QString& source, const QString& destination, QString& digest);
    [[nodiscard]] bool decryptAndVerify(
        const QString& source,
        QIODevice* destination,
        const QString& expected_digest);
    [[nodiscard]] bool decryptFile(const QString& source, const QString& destination, const QString& expected_digest);
    [[nodiscard]] bool appendManifest(const RecoveryVaultEntry& entry);
    void setError(QString message);

    QString root_;
    QByteArray key_;
    QString error_;
};

}  // namespace pms::desktop
