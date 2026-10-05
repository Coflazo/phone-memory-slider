#include "recovery_vault.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUuid>

#include <array>
#include <utility>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace pms::desktop {
namespace {

constexpr std::array<char, 8> vault_magic{'P', 'M', 'S', 'V', 'A', 'U', 'L', 'T'};
constexpr qsizetype key_bytes = 32;
constexpr qsizetype nonce_bytes = 12;
constexpr qsizetype tag_bytes = 16;
constexpr qint64 tag_offset = static_cast<qint64>(vault_magic.size() + sizeof(quint32) + sizeof(quint64) + nonce_bytes);

[[nodiscard]] QString default_root() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::home().filePath(QStringLiteral(".phone-memory-slider"));
    }
    return root + QStringLiteral("/recovery-vault");
}

[[nodiscard]] QString file_digest(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash{QCryptographicHash::Sha256};
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

class VerificationSink final : public QIODevice {
public:
    explicit VerificationSink(QIODevice* destination) : destination_(destination) { open(QIODevice::WriteOnly); }

    [[nodiscard]] quint64 bytesWritten() const noexcept { return bytes_written_; }
    [[nodiscard]] QString digest() { return QString::fromLatin1(hash_.result().toHex()); }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, const qint64 length) override {
        if (destination_ != nullptr && destination_->write(data, length) != length) {
            return -1;
        }
        hash_.addData(QByteArrayView{data, length});
        bytes_written_ += static_cast<quint64>(length);
        return length;
    }

private:
    QIODevice* destination_{};
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    quint64 bytes_written_{};
};

#ifdef Q_OS_WIN
[[nodiscard]] bool success(const NTSTATUS status) noexcept {
    return status >= 0;
}

class AesGcmKey final {
public:
    explicit AesGcmKey(const QByteArray& bytes) {
        if (!success(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
            !success(BCryptSetProperty(
                algorithm_,
                BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                sizeof(BCRYPT_CHAIN_MODE_GCM),
                0))) {
            return;
        }
        ULONG object_size{};
        ULONG copied{};
        if (!success(BCryptGetProperty(
                algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0))) {
            return;
        }
        object_.resize(static_cast<qsizetype>(object_size));
        if (!success(BCryptGenerateSymmetricKey(
                algorithm_,
                &key_,
                reinterpret_cast<PUCHAR>(object_.data()),
                object_size,
                reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.constData())),
                static_cast<ULONG>(bytes.size()),
                0))) {
            key_ = nullptr;
        }
    }

    ~AesGcmKey() {
        if (key_ != nullptr) {
            BCryptDestroyKey(key_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return key_; }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_KEY_HANDLE key_{};
    QByteArray object_;
};

[[nodiscard]] bool crypt_stream(
    QIODevice& input,
    QIODevice& output,
    const QByteArray& key_bytes_value,
    QByteArray& nonce,
    QByteArray& tag,
    const bool encrypt) {
    AesGcmKey key{key_bytes_value};
    if (key.get() == nullptr) {
        return false;
    }
    QByteArray mac(tag_bytes, Qt::Uninitialized);
    QByteArray context_iv(16, '\0');
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = static_cast<ULONG>(tag.size());
    info.pbMacContext = reinterpret_cast<PUCHAR>(mac.data());
    info.cbMacContext = static_cast<ULONG>(mac.size());

    constexpr qint64 chunk_size = 4 * 1024 * 1024;
    QByteArray current = input.read(chunk_size);
    if (current.isEmpty() && !input.atEnd()) {
        return false;
    }
    auto first_call = true;
    for (;;) {
        const auto next = input.read(chunk_size);
        const auto final_chunk = next.isEmpty() && input.atEnd();
        if (first_call && final_chunk) {
            info.pbMacContext = nullptr;
            info.cbMacContext = 0;
        } else if (first_call) {
            info.dwFlags = BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        } else if (final_chunk) {
            info.dwFlags &= ~BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        }
        QByteArray transformed(current.size(), Qt::Uninitialized);
        ULONG written{};
        const auto status = encrypt
                                ? BCryptEncrypt(
                                      key.get(),
                                      reinterpret_cast<PUCHAR>(current.data()),
                                      static_cast<ULONG>(current.size()),
                                      &info,
                                      info.pbMacContext == nullptr ? nullptr : reinterpret_cast<PUCHAR>(context_iv.data()),
                                      info.pbMacContext == nullptr ? 0 : static_cast<ULONG>(context_iv.size()),
                                      reinterpret_cast<PUCHAR>(transformed.data()),
                                      static_cast<ULONG>(transformed.size()),
                                      &written,
                                      0)
                                : BCryptDecrypt(
                                      key.get(),
                                      reinterpret_cast<PUCHAR>(current.data()),
                                      static_cast<ULONG>(current.size()),
                                      &info,
                                      info.pbMacContext == nullptr ? nullptr : reinterpret_cast<PUCHAR>(context_iv.data()),
                                      info.pbMacContext == nullptr ? 0 : static_cast<ULONG>(context_iv.size()),
                                      reinterpret_cast<PUCHAR>(transformed.data()),
                                      static_cast<ULONG>(transformed.size()),
                                      &written,
                                      0);
        if (!success(status) || output.write(transformed.constData(), static_cast<qint64>(written)) != written) {
            return false;
        }
        if (final_chunk) {
            return true;
        }
        first_call = false;
        current = next;
    }
}
#else
[[nodiscard]] bool crypt_stream(
    QIODevice& input,
    QIODevice& output,
    const QByteArray& key,
    QByteArray& nonce,
    QByteArray& tag,
    const bool encrypt) {
    auto* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return false;
    }
    const auto cleanup = qScopeGuard([context] { EVP_CIPHER_CTX_free(context); });
    const auto initialized = encrypt
                                 ? EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)
                                 : EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    if (initialized != 1 || EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1) {
        return false;
    }
    const auto keyed = encrypt
                           ? EVP_EncryptInit_ex(
                                 context,
                                 nullptr,
                                 nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()),
                                 reinterpret_cast<const unsigned char*>(nonce.constData()))
                           : EVP_DecryptInit_ex(
                                 context,
                                 nullptr,
                                 nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()),
                                 reinterpret_cast<const unsigned char*>(nonce.constData()));
    if (keyed != 1) {
        return false;
    }
    constexpr qint64 chunk_size = 4 * 1024 * 1024;
    while (!input.atEnd()) {
        const auto chunk = input.read(chunk_size);
        if (chunk.isEmpty() && !input.atEnd()) {
            return false;
        }
        QByteArray transformed(chunk.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
        int written{};
        const auto updated = encrypt
                                 ? EVP_EncryptUpdate(
                                       context,
                                       reinterpret_cast<unsigned char*>(transformed.data()),
                                       &written,
                                       reinterpret_cast<const unsigned char*>(chunk.constData()),
                                       static_cast<int>(chunk.size()))
                                 : EVP_DecryptUpdate(
                                       context,
                                       reinterpret_cast<unsigned char*>(transformed.data()),
                                       &written,
                                       reinterpret_cast<const unsigned char*>(chunk.constData()),
                                       static_cast<int>(chunk.size()));
        if (updated != 1 || output.write(transformed.constData(), written) != written) {
            return false;
        }
    }
    if (!encrypt && EVP_CIPHER_CTX_ctrl(
                        context, EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<char*>(tag.constData())) != 1) {
        return false;
    }
    std::array<unsigned char, EVP_MAX_BLOCK_LENGTH> final_bytes{};
    int final_size{};
    const auto finalized = encrypt ? EVP_EncryptFinal_ex(context, final_bytes.data(), &final_size)
                                   : EVP_DecryptFinal_ex(context, final_bytes.data(), &final_size);
    if (finalized != 1 || output.write(reinterpret_cast<const char*>(final_bytes.data()), final_size) != final_size) {
        return false;
    }
    return !encrypt || EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
}
#endif

}  // namespace

RecoveryVault::RecoveryVault(QString root)
    : root_(root.isEmpty() ? default_root() : std::move(root)) {}

bool RecoveryVault::open() {
    error_.clear();
    if (!QDir{}.mkpath(root_)) {
        setError(QStringLiteral("The encrypted recovery vault could not be created"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            root_,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        setError(QStringLiteral("The recovery vault could not be restricted to the current user"));
        return false;
    }
#endif
    if (!loadOrCreateKey()) {
        if (error_.isEmpty()) {
            setError(QStringLiteral("The encrypted recovery vault could not be created"));
        }
        return false;
    }
    return true;
}

bool RecoveryVault::archive(
    const QString& plain_path,
    const QString& asset_id,
    const QString& original_name,
    VaultEntry& entry) {
    error_.clear();
    const QFileInfo source{plain_path};
    if (key_.size() != key_bytes || !source.isFile()) {
        setError(QStringLiteral("The recovery source is unavailable"));
        return false;
    }
    Q_UNUSED(asset_id);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto destination = QDir{root_}.filePath(id + QStringLiteral(".pmsvault"));
    QString digest;
    if (!encryptFile(plain_path, destination, digest)) {
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QFile::remove(destination);
        setError(QStringLiteral("The recovery payload could not be restricted to the current user"));
        return false;
    }
#endif
    if (!decryptAndVerify(destination, nullptr, digest)) {
        QFile::remove(destination);
        return false;
    }
    entry = {id, original_name, destination, digest, static_cast<std::uint64_t>(source.size())};
    if (!appendManifest(entry)) {
        QFile::remove(destination);
        return false;
    }
    return true;
}

bool RecoveryVault::restore(const VaultEntry& entry, const QString& destination) {
    error_.clear();
    return decryptFile(entry.encryptedPath, destination, entry.sha256);
}

QString RecoveryVault::errorString() const {
    return error_;
}

QString RecoveryVault::rootPath() const {
    return root_;
}

bool RecoveryVault::loadOrCreateKey() {
#ifdef Q_OS_WIN
    const auto path = QDir{root_}.filePath(QStringLiteral("vault.key"));
    QFile existing{path};
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) {
            setError(existing.errorString());
            return false;
        }
        const auto protected_key = existing.readAll();
        DATA_BLOB input{static_cast<DWORD>(protected_key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(protected_key.constData()))};
        DATA_BLOB plain{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
            setError(QStringLiteral("Windows could not unlock the recovery-vault key"));
            return false;
        }
        key_ = QByteArray{reinterpret_cast<const char*>(plain.pbData), static_cast<qsizetype>(plain.cbData)};
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
        if (key_.size() != key_bytes) {
            key_.clear();
            setError(QStringLiteral("The recovery-vault key is invalid"));
            return false;
        }
        return true;
    }

    key_.resize(key_bytes);
    if (!success(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key_.data()), key_bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        key_.clear();
        setError(QStringLiteral("Windows could not generate a recovery-vault key"));
        return false;
    }
    DATA_BLOB input{static_cast<DWORD>(key_.size()), reinterpret_cast<BYTE*>(key_.data())};
    DATA_BLOB protected_key{};
    if (!CryptProtectData(
            &input,
            L"Phone Memory Slider recovery vault",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &protected_key)) {
        key_.clear();
        setError(QStringLiteral("Windows could not protect the recovery-vault key"));
        return false;
    }
    QSaveFile file{path};
    const auto opened = file.open(QIODevice::WriteOnly);
    const auto written = opened && file.write(reinterpret_cast<const char*>(protected_key.pbData), protected_key.cbData) == protected_key.cbData;
    SecureZeroMemory(protected_key.pbData, protected_key.cbData);
    LocalFree(protected_key.pbData);
    if (!written || !file.commit()) {
        setError(file.errorString());
        return false;
    }
#ifdef Q_OS_WIN
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#else
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        setError(QStringLiteral("The recovery-vault key could not be restricted to the current user"));
        return false;
    }
#endif
    return true;
#else
    const auto path = QDir{root_}.filePath(QStringLiteral("vault.key"));
    QFile existing{path};
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) {
            setError(existing.errorString());
            return false;
        }
        key_ = existing.readAll();
        if (key_.size() != key_bytes) {
            key_.clear();
            setError(QStringLiteral("The recovery-vault key is invalid"));
            return false;
        }
        return true;
    }
    key_.resize(key_bytes);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key_.data()), key_.size()) != 1) {
        key_.clear();
        setError(QStringLiteral("The recovery-vault key could not be generated"));
        return false;
    }
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(key_) != key_.size() || !file.commit()) {
        setError(file.errorString());
        return false;
    }
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        setError(QStringLiteral("The recovery-vault key could not be restricted to the current user"));
        return false;
    }
    return true;
#endif
}

bool RecoveryVault::encryptFile(const QString& source, const QString& destination, QString& digest) {
    QFile input{source};
    QSaveFile output{destination};
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        setError(input.isOpen() ? output.errorString() : input.errorString());
        return false;
    }
    QByteArray nonce(nonce_bytes, Qt::Uninitialized);
    QByteArray tag(tag_bytes, '\0');
#ifdef Q_OS_WIN
    if (!success(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce.data()), nonce.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        setError(QStringLiteral("Windows could not generate an encryption nonce"));
        return false;
    }
#else
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1) {
        setError(QStringLiteral("An encryption nonce could not be generated"));
        return false;
    }
#endif
    QDataStream header{&output};
    header.setByteOrder(QDataStream::LittleEndian);
    header.writeRawData(vault_magic.data(), static_cast<qint64>(vault_magic.size()));
    header << quint32{1} << static_cast<quint64>(input.size());
    header.writeRawData(nonce.constData(), nonce.size());
    header.writeRawData(tag.constData(), tag.size());
    if (header.status() != QDataStream::Ok || !crypt_stream(input, output, key_, nonce, tag, true) || !output.commit()) {
        setError(QStringLiteral("The recovery copy could not be encrypted"));
        return false;
    }
    QFile finalized{destination};
    if (!finalized.open(QIODevice::ReadWrite) || !finalized.seek(tag_offset) || finalized.write(tag) != tag.size()) {
        finalized.close();
        QFile::remove(destination);
        setError(QStringLiteral("The recovery authentication tag could not be saved"));
        return false;
    }
    finalized.close();
    digest = file_digest(source);
    if (digest.isEmpty()) {
        QFile::remove(destination);
        setError(QStringLiteral("The recovery copy checksum could not be calculated"));
        return false;
    }
    return true;
}

bool RecoveryVault::decryptFile(const QString& source, const QString& destination, const QString& expected_digest) {
    QSaveFile output{destination};
    if (!output.open(QIODevice::WriteOnly)) {
        setError(output.errorString());
        return false;
    }
    if (!decryptAndVerify(source, &output, expected_digest)) {
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        setError(QStringLiteral("Recovery output could not be finalized: %1").arg(output.errorString()));
        return false;
    }
    return true;
}

bool RecoveryVault::decryptAndVerify(
    const QString& source,
    QIODevice* destination,
    const QString& expected_digest) {
    QFile input{source};
    if (!input.open(QIODevice::ReadOnly)) {
        setError(input.errorString());
        return false;
    }
    QDataStream header{&input};
    header.setByteOrder(QDataStream::LittleEndian);
    std::array<char, vault_magic.size()> magic{};
    quint32 version{};
    quint64 original_size{};
    QByteArray nonce(nonce_bytes, Qt::Uninitialized);
    QByteArray tag(tag_bytes, Qt::Uninitialized);
    header.readRawData(magic.data(), static_cast<qint64>(magic.size()));
    header >> version >> original_size;
    header.readRawData(nonce.data(), nonce.size());
    header.readRawData(tag.data(), tag.size());
    if (header.status() != QDataStream::Ok || magic != vault_magic || version != 1) {
        setError(QStringLiteral("The recovery copy header is invalid"));
        return false;
    }
    VerificationSink output{destination};
    if (!crypt_stream(input, output, key_, nonce, tag, false)) {
        setError(QStringLiteral("Recovery authentication or output verification failed"));
        return false;
    }
    if (output.bytesWritten() != original_size) {
        setError(QStringLiteral("Recovery size verification failed (%1 of %2 bytes)")
                     .arg(output.bytesWritten())
                     .arg(original_size));
        return false;
    }
    if (output.digest() != expected_digest) {
        setError(QStringLiteral("Recovery checksum verification failed"));
        return false;
    }
    return true;
}

bool RecoveryVault::appendManifest(const VaultEntry& entry) {
    QFile manifest{QDir{root_}.filePath(QStringLiteral("manifest.jsonl"))};
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Append)) {
        setError(manifest.errorString());
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(manifest.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        setError(QStringLiteral("The recovery manifest could not be restricted to the current user"));
        return false;
    }
#endif
    const QJsonObject object{
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("originalName"), entry.originalName},
        {QStringLiteral("encryptedPath"), entry.encryptedPath},
        {QStringLiteral("sha256"), entry.sha256},
        {QStringLiteral("bytes"), static_cast<qint64>(entry.bytes)},
    };
    const auto line = QJsonDocument{object}.toJson(QJsonDocument::Compact) + '\n';
    if (manifest.write(line) != line.size()) {
        setError(manifest.errorString());
        return false;
    }
    return true;
}

void RecoveryVault::setError(QString message) {
    error_ = std::move(message);
}

}  // namespace pms::desktop
