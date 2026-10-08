#include "vault/vault.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <utility>

#include "platform/key_store.hpp"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
#include <QScopeGuard>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace pms::desktop {
namespace {

Q_LOGGING_CATEGORY(vault_log, "pms.vault")

using Magic = std::array<char, 8>;
constexpr Magic payload_magic{'P', 'M', 'S', 'V', 'A', 'U', 'L', 'T'};
constexpr Magic manifest_magic{'P', 'M', 'S', 'M', 'A', 'N', 'I', 'F'};
constexpr quint32 format_version = 2;
constexpr qsizetype key_bytes = 32;
constexpr qsizetype nonce_bytes = 12;
constexpr qsizetype tag_bytes = 16;
constexpr qint64 aad_header_bytes = 8 + 4 + 8 + nonce_bytes;   // magic | version | size | nonce
constexpr qint64 header_bytes = aad_header_bytes + tag_bytes;  // + tag
constexpr qint64 manifest_limit = 64 * 1024 * 1024;
constexpr qsizetype text_limit = 1024;

// Hashes and counts what passes through, forwarding it to `destination` (nullptr: nothing is written anywhere).
class VerificationSink final : public QIODevice {
public:
    explicit VerificationSink(QIODevice* destination) : destination_(destination) {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }

    [[nodiscard]] quint64 written() const noexcept { return written_; }
    [[nodiscard]] QByteArray digest() const { return hash_.result(); }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, const qint64 length) override {
        if (destination_ != nullptr && destination_->write(data, length) != length) {
            return -1;
        }
        hash_.addData(QByteArrayView{data, length});
        written_ += static_cast<quint64>(length);
        return length;
    }

private:
    QIODevice* destination_{};
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    quint64 written_{};
};

// Up to `size` bytes; fewer only at the end of the input. nullopt on a read error.
std::optional<QByteArray> read_full(QIODevice& input, const qint64 size) {
    QByteArray buffer(size, Qt::Uninitialized);
    qint64 filled = 0;
    while (filled < size) {
        const auto got = input.read(buffer.data() + filled, size - filled);
        if (got < 0) {
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        filled += got;
    }
    buffer.truncate(filled);
    return buffer;
}

bool fill_random(QByteArray& bytes) {
#ifdef Q_OS_WIN
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(bytes.size())) == 1;
#endif
}

#ifdef Q_OS_WIN
[[nodiscard]] bool success(const NTSTATUS status) noexcept {
    return status >= 0;
}

class AesGcmKey final {
public:
    explicit AesGcmKey(const QByteArray& bytes) {
        if (!success(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
            !success(BCryptSetProperty(algorithm_, BCRYPT_CHAINING_MODE,
                                       reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                       sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            return;
        }
        ULONG object_size{};
        ULONG copied{};
        if (!success(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                       sizeof(object_size), &copied, 0))) {
            return;
        }
        object_.resize(static_cast<qsizetype>(object_size));
        if (!success(BCryptGenerateSymmetricKey(algorithm_, &key_, reinterpret_cast<PUCHAR>(object_.data()), object_size,
                                                reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.constData())),
                                                static_cast<ULONG>(bytes.size()), 0))) {
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
        SecureZeroMemory(object_.data(), static_cast<SIZE_T>(object_.size()));
    }
    AesGcmKey(const AesGcmKey&) = delete;
    AesGcmKey& operator=(const AesGcmKey&) = delete;

    [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return key_; }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_KEY_HANDLE key_{};
    QByteArray object_;
};

bool gcm_stream(QIODevice& input, QIODevice& output, const QByteArray& key_value, const QByteArray& nonce,
                const QByteArray& aad, QByteArray& tag, const bool encrypt, const qint64 chunk_bytes,
                QCryptographicHash* input_hash) {
    const AesGcmKey key{key_value};
    if (key.get() == nullptr) {
        return false;
    }
    auto nonce_copy = nonce;
    auto aad_copy = aad;
    QByteArray mac(tag_bytes, Qt::Uninitialized);
    QByteArray chain_iv(16, '\0');
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(nonce_copy.data());
    info.cbNonce = static_cast<ULONG>(nonce_copy.size());
    info.pbAuthData = aad_copy.isEmpty() ? nullptr : reinterpret_cast<PUCHAR>(aad_copy.data());
    info.cbAuthData = static_cast<ULONG>(aad_copy.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = static_cast<ULONG>(tag.size());
    info.pbMacContext = reinterpret_cast<PUCHAR>(mac.data());
    info.cbMacContext = static_cast<ULONG>(mac.size());
    const auto call = encrypt ? BCryptEncrypt : BCryptDecrypt;

    auto current = read_full(input, chunk_bytes);
    if (!current) {
        return false;
    }
    for (auto first = true;; first = false) {
        auto next = read_full(input, chunk_bytes);
        if (!next) {
            return false;
        }
        const auto last = next->isEmpty();
        if (first && last) {  // one call, no chaining
            info.pbMacContext = nullptr;
            info.cbMacContext = 0;
        } else if (first) {
            info.dwFlags = BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        } else if (last) {
            info.dwFlags &= ~BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        }
        if (encrypt && input_hash != nullptr) {
            input_hash->addData(*current);
        }
        const auto chained = info.pbMacContext != nullptr;
        // Never hand BCrypt a null output buffer: that turns the call into a size query.
        QByteArray transformed(std::max<qsizetype>(current->size(), tag_bytes), Qt::Uninitialized);
        ULONG written{};
        const auto status = call(key.get(), reinterpret_cast<PUCHAR>(current->data()), static_cast<ULONG>(current->size()),
                                 &info, chained ? reinterpret_cast<PUCHAR>(chain_iv.data()) : nullptr,
                                 chained ? static_cast<ULONG>(chain_iv.size()) : 0,
                                 reinterpret_cast<PUCHAR>(transformed.data()), static_cast<ULONG>(current->size()),
                                 &written, 0);
        if (!success(status) || output.write(transformed.constData(), static_cast<qint64>(written)) != written) {
            return false;
        }
        if (last) {
            return true;
        }
        // The AAD goes into the first call only; the MAC context carries it from there.
        info.pbAuthData = nullptr;
        info.cbAuthData = 0;
        current = std::move(next);
    }
}
#else
bool gcm_stream(QIODevice& input, QIODevice& output, const QByteArray& key, const QByteArray& nonce,
                const QByteArray& aad, QByteArray& tag, const bool encrypt, const qint64 chunk_bytes,
                QCryptographicHash* input_hash) {
    const auto bytes = [](const QByteArray& value) { return reinterpret_cast<const unsigned char*>(value.constData()); };
    auto* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return false;
    }
    const auto cleanup = qScopeGuard([context] { EVP_CIPHER_CTX_free(context); });
    const auto init = encrypt ? EVP_EncryptInit_ex : EVP_DecryptInit_ex;
    const auto update = encrypt ? EVP_EncryptUpdate : EVP_DecryptUpdate;
    const auto finish = encrypt ? EVP_EncryptFinal_ex : EVP_DecryptFinal_ex;
    if (init(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
        init(context, nullptr, nullptr, bytes(key), bytes(nonce)) != 1) {
        return false;
    }
    int written{};
    if (!aad.isEmpty() && update(context, nullptr, &written, bytes(aad), static_cast<int>(aad.size())) != 1) {
        return false;
    }
    for (;;) {
        const auto chunk = read_full(input, chunk_bytes);
        if (!chunk) {
            return false;
        }
        if (chunk->isEmpty()) {
            break;
        }
        if (encrypt && input_hash != nullptr) {
            input_hash->addData(*chunk);
        }
        QByteArray transformed(chunk->size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
        if (update(context, reinterpret_cast<unsigned char*>(transformed.data()), &written, bytes(*chunk),
                   static_cast<int>(chunk->size())) != 1 ||
            output.write(transformed.constData(), written) != written) {
            return false;
        }
    }
    if (!encrypt && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
        return false;
    }
    std::array<unsigned char, EVP_MAX_BLOCK_LENGTH> final_bytes{};
    if (finish(context, final_bytes.data(), &written) != 1 ||
        output.write(reinterpret_cast<const char*>(final_bytes.data()), written) != written) {
        return false;
    }
    return !encrypt || EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
}
#endif

QByteArray sealed_header(const Magic& magic, const quint64 size, const QByteArray& nonce) {
    QByteArray header(aad_header_bytes, Qt::Uninitialized);
    std::copy(magic.begin(), magic.end(), header.begin());
    qToLittleEndian(format_version, header.data() + 8);
    qToLittleEndian(size, header.data() + 12);
    std::copy(nonce.begin(), nonce.end(), header.begin() + 20);
    return header;
}

// Writes header, tag and ciphertext of exactly `size` plaintext bytes; the caller commits `output`.
bool seal(QIODevice& input, const quint64 size, QSaveFile& output, const Magic& magic, const QByteArray& context,
          const QByteArray& key, QCryptographicHash* hash, QString& error) {
    QByteArray nonce(nonce_bytes, Qt::Uninitialized);
    if (!fill_random(nonce)) {
        error = QStringLiteral("A random nonce could not be generated");
        return false;
    }
    const auto header = sealed_header(magic, size, nonce);
    QByteArray tag(tag_bytes, '\0');
    if (output.write(header) != header.size() || output.write(tag) != tag.size() ||
        !detail::aes_gcm_stream(input, output, key, nonce, header + context, tag, true, detail::gcm_chunk_bytes, hash)) {
        error = QStringLiteral("The encrypted copy could not be written: %1").arg(output.errorString());
        return false;
    }
    if (output.pos() != header_bytes + static_cast<qint64>(size)) {
        error = QStringLiteral("The file changed while it was being copied");
        return false;
    }
    if (!output.seek(aad_header_bytes) || output.write(tag) != tag.size()) {
        error = QStringLiteral("The encrypted copy could not be finished: %1").arg(output.errorString());
        return false;
    }
    return true;
}

// Decrypts a sealed file into `sink` and returns its authenticated plaintext size.
std::optional<quint64> unseal(QFile& input, const Magic& magic, const QByteArray& context, const QByteArray& key,
                              QIODevice& sink, QString& error) {
    const auto header = input.read(header_bytes);
    if (header.size() != header_bytes || !std::equal(magic.begin(), magic.end(), header.begin()) ||
        qFromLittleEndian<quint32>(header.constData() + 8) != format_version) {
        error = QStringLiteral("The encrypted file's header is not valid");
        return std::nullopt;
    }
    const auto size = qFromLittleEndian<quint64>(header.constData() + 12);
    if (static_cast<quint64>(input.size() - header_bytes) != size) {
        error = QStringLiteral("The encrypted file's length does not match its header");
        return std::nullopt;
    }
    auto tag = header.mid(aad_header_bytes, tag_bytes);
    if (!detail::aes_gcm_stream(input, sink, key, header.mid(20, nonce_bytes), header.left(aad_header_bytes) + context,
                                tag, false)) {
        error = QStringLiteral("The encrypted file failed its integrity check");
        return std::nullopt;
    }
    return size;
}

bool restrict_to_owner(const QString& path) {
#ifdef Q_OS_WIN
    Q_UNUSED(path);
    return true;
#else
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

QJsonObject to_json(const VaultEntry& entry) {
    return {
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("sourceKey"), entry.source_key},
        {QStringLiteral("sourceId"), entry.source_id},
        {QStringLiteral("originalName"), entry.original_name},
        {QStringLiteral("bytes"), QString::number(entry.bytes)},
        {QStringLiteral("sha256"), QString::fromLatin1(entry.sha256.toHex())},
        {QStringLiteral("archivedMs"), QString::number(entry.archived_ms)},
    };
}

std::optional<VaultEntry> from_json(const QJsonObject& object) {
    static const QRegularExpression uuid{
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")};
    VaultEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.source_key = object.value(QStringLiteral("sourceKey")).toString();
    entry.source_id = object.value(QStringLiteral("sourceId")).toString();
    entry.original_name = object.value(QStringLiteral("originalName")).toString();
    entry.sha256 = QByteArray::fromHex(object.value(QStringLiteral("sha256")).toString().toLatin1());
    bool bytes_ok = false;
    bool time_ok = false;
    entry.bytes = object.value(QStringLiteral("bytes")).toString().toULongLong(&bytes_ok);
    entry.archived_ms = object.value(QStringLiteral("archivedMs")).toString().toLongLong(&time_ok);
    if (!uuid.match(entry.id).hasMatch() || entry.sha256.size() != 32 || !bytes_ok || !time_ok) {
        return std::nullopt;
    }
    return entry;
}

// A plain file name from whatever the phone called the file: no folders, no characters Windows rejects,
// no device names.
QString safe_file_name(const QString& original, const QString& fallback) {
    static const QRegularExpression separators{QStringLiteral("[/\\\\]")};
    static const QRegularExpression invalid{QStringLiteral("[<>:\"|?*\\x00-\\x1f]")};
    static const QRegularExpression reserved{QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\\..*)?$"),
                                             QRegularExpression::CaseInsensitiveOption};
    auto name = original.section(separators, -1);
    name.replace(invalid, QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        return fallback;
    }
    if (reserved.match(name).hasMatch()) {
        name.prepend(QLatin1Char('_'));
    }
    return name;
}

// "IMG_1.JPG", then "IMG_1 2.JPG", "IMG_1 3.JPG"...: an existing file is never replaced.
QString unique_target(const QDir& directory, const QString& name) {
    if (!QFileInfo::exists(directory.filePath(name))) {
        return directory.filePath(name);
    }
    const QFileInfo info{name};
    const auto base = info.completeBaseName();
    const auto suffix = info.suffix();
    for (int copy = 2; copy < 10000; ++copy) {
        const auto candidate = suffix.isEmpty()
                                   ? QStringLiteral("%1 %2").arg(base, QString::number(copy))
                                   : QStringLiteral("%1 %2.%3").arg(base, QString::number(copy), suffix);
        if (!QFileInfo::exists(directory.filePath(candidate))) {
            return directory.filePath(candidate);
        }
    }
    return {};
}

}  // namespace

bool has_room(const quint64 free_bytes, const quint64 bytes) noexcept {
    if (bytes > free_bytes) {
        return false;
    }
    const auto headroom = bytes / 10 + (bytes % 10 != 0 ? 1 : 0);
    return free_bytes - bytes >= headroom;
}

bool detail::aes_gcm_stream(QIODevice& input, QIODevice& output, const QByteArray& key, const QByteArray& nonce,
                            const QByteArray& aad, QByteArray& tag, const bool encrypt, const qint64 chunk_bytes,
                            QCryptographicHash* input_hash) {
    if (key.size() != key_bytes || nonce.size() != nonce_bytes || tag.size() != tag_bytes || chunk_bytes <= 0 ||
        chunk_bytes % 16 != 0) {
        return false;
    }
    return gcm_stream(input, output, key, nonce, aad, tag, encrypt, chunk_bytes, input_hash);
}

Vault::Vault(QString directory) : directory_(std::move(directory)) {}

Vault::~Vault() {
    key_.fill('\0');
}

QString Vault::payloadPath(const QString& id) const {
    return QDir{directory_}.filePath(id + QStringLiteral(".pmsv"));
}

bool Vault::open(QString* error) {
    key_.fill('\0');
    key_.clear();
    entries_.clear();
    const auto fail = [error](const QString& message) {
        qCWarning(vault_log).noquote() << message;
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (directory_.isEmpty() || !QDir{}.mkpath(directory_)) {
        return fail(QStringLiteral("The recovery vault folder could not be created"));
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return fail(QStringLiteral("The recovery vault folder could not be restricted to the current user"));
    }
#endif
    QString message;
    auto key = KeyStore::loadOrCreate(QStringLiteral("vault"), &message);
    if (!key) {
        return fail(message);
    }

    QList<VaultEntry> entries;
    QFile manifest{QDir{directory_}.filePath(QStringLiteral("manifest.pmsm"))};
    if (manifest.exists()) {
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > header_bytes + manifest_limit) {
            return fail(QStringLiteral("The recovery vault index could not be read"));
        }
        QBuffer plain;
        plain.open(QIODevice::WriteOnly);
        if (!unseal(manifest, manifest_magic, QByteArrayLiteral("manifest"), *key, plain, message)) {
            return fail(QStringLiteral("The recovery vault index was changed outside the app or is damaged (%1); "
                                       "the vault is not used")
                            .arg(message));
        }
        const auto document = QJsonDocument::fromJson(plain.data());
        for (const auto& value : document.object().value(QStringLiteral("entries")).toArray()) {
            const auto entry = from_json(value.toObject());
            if (!entry) {
                return fail(QStringLiteral("The recovery vault index has an invalid entry"));
            }
            entries.push_back(*entry);
        }
    }
    key_ = *key;
    entries_ = std::move(entries);
    return true;
}

std::optional<VaultEntry> Vault::archive(const QString& file, const QString& source_key, const QString& source_id,
                                         const QString& name, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<VaultEntry> {
        qCWarning(vault_log).noquote() << message;
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (key_.size() != key_bytes) {
        return fail(QStringLiteral("The recovery vault is not open"));
    }
    if (source_key.size() > text_limit || source_id.size() > text_limit || name.size() > text_limit) {
        return fail(QStringLiteral("The item's name or id is too long for the recovery vault"));
    }
    QFile input{file};
    if (!QFileInfo{file}.isFile() || !input.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("The file to keep in the recovery vault could not be read"));
    }
    VaultEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.source_key = source_key;
    entry.source_id = source_id;
    entry.original_name = name.isEmpty() ? QFileInfo{file}.fileName() : name;
    entry.bytes = static_cast<quint64>(input.size());
    entry.archived_ms = QDateTime::currentMSecsSinceEpoch();
    const auto path = payloadPath(entry.id);

    QString message;
    {
        QSaveFile output{path};
        QCryptographicHash hash{QCryptographicHash::Sha256};
        if (!output.open(QIODevice::WriteOnly) ||
            !seal(input, entry.bytes, output, payload_magic, entry.id.toUtf8(), key_, &hash, message) ||
            !output.commit()) {
            if (message.isEmpty()) {
                message = QStringLiteral("The encrypted copy could not be saved: %1").arg(output.errorString());
            }
            output.cancelWriting();
            return fail(message);
        }
        entry.sha256 = hash.result();
    }
    if (!restrict_to_owner(path)) {
        QFile::remove(path);
        return fail(QStringLiteral("The encrypted copy could not be restricted to the current user"));
    }
    // Check what is on disk, not what was in memory: decrypt it into a hash sink. Nothing is written.
    if (!decryptPayload(entry, nullptr, message)) {
        QFile::remove(path);
        return fail(message);
    }
    auto updated = entries_;
    updated.push_back(entry);
    if (!writeManifest(updated, message)) {
        QFile::remove(path);
        return fail(message);
    }
    entries_ = std::move(updated);
    return entry;
}

bool Vault::decryptPayload(const VaultEntry& entry, QIODevice* destination, QString& error) const {
    QFile input{payloadPath(entry.id)};
    if (!input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("The archived copy is missing from the recovery vault");
        return false;
    }
    VerificationSink sink{destination};
    const auto size = unseal(input, payload_magic, entry.id.toUtf8(), key_, sink, error);
    if (!size) {
        return false;
    }
    if (*size != entry.bytes || sink.written() != entry.bytes || sink.digest() != entry.sha256) {
        error = QStringLiteral("The archived copy does not match its record");
        return false;
    }
    return true;
}

bool Vault::restore(const QString& entry_id, const QString& destination_dir, QString* restored_path, QString* error) {
    const auto fail = [error](const QString& message) {
        qCWarning(vault_log).noquote() << message;
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (key_.size() != key_bytes) {
        return fail(QStringLiteral("The recovery vault is not open"));
    }
    const auto entry = std::find_if(entries_.cbegin(), entries_.cend(),
                                    [&entry_id](const VaultEntry& candidate) { return candidate.id == entry_id; });
    if (entry == entries_.cend()) {
        return fail(QStringLiteral("This item is not in the recovery vault"));
    }
    if (destination_dir.isEmpty() || !QDir{}.mkpath(destination_dir)) {
        return fail(QStringLiteral("The restore folder could not be created"));
    }
    const auto target = unique_target(
        QDir{destination_dir}, safe_file_name(entry->original_name, QStringLiteral("restored-") + entry->id));
    if (target.isEmpty()) {
        return fail(QStringLiteral("No free file name was found in the restore folder"));
    }
    // QSaveFile writes a temporary file and only renames it into place after every check passed.
    QSaveFile output{target};
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("The restored file could not be created: %1").arg(output.errorString()));
    }
    QString message;
    if (!decryptPayload(*entry, &output, message)) {
        output.cancelWriting();
        return fail(message);
    }
    if (!output.commit()) {
        return fail(QStringLiteral("The restored file could not be saved: %1").arg(output.errorString()));
    }
    if (restored_path != nullptr) {
        *restored_path = target;
    }
    return true;
}

bool Vault::writeManifest(const QList<VaultEntry>& entries, QString& error) const {
    QJsonArray array;
    for (const auto& entry : entries) {
        array.append(to_json(entry));
    }
    auto json = QJsonDocument{QJsonObject{{QStringLiteral("entries"), array}}}.toJson(QJsonDocument::Compact);
    QBuffer plain{&json};
    plain.open(QIODevice::ReadOnly);
    const auto path = QDir{directory_}.filePath(QStringLiteral("manifest.pmsm"));
    QSaveFile output{path};
    if (!output.open(QIODevice::WriteOnly) ||
        !seal(plain, static_cast<quint64>(json.size()), output, manifest_magic, QByteArrayLiteral("manifest"), key_,
              nullptr, error) ||
        !output.commit()) {
        if (error.isEmpty()) {
            error = QStringLiteral("The recovery vault index could not be saved: %1").arg(output.errorString());
        }
        return false;
    }
    if (!restrict_to_owner(path)) {
        error = QStringLiteral("The recovery vault index could not be restricted to the current user");
        return false;
    }
    return true;
}

QList<VaultEntry> Vault::entries() const {
    return entries_;
}

quint64 Vault::bytesUsed() const {
    quint64 total = 0;
    for (const auto& entry : entries_) {
        total += entry.bytes + static_cast<quint64>(header_bytes);
    }
    return total;
}

int Vault::purgeOlderThan(const qint64 cutoff_ms) {
    if (key_.size() != key_bytes) {
        return 0;
    }
    QList<VaultEntry> keep;
    QList<VaultEntry> drop;
    for (const auto& entry : entries_) {
        (entry.archived_ms < cutoff_ms ? drop : keep).push_back(entry);
    }
    if (drop.isEmpty()) {
        return 0;
    }
    // Index first: a crash after this leaves unreferenced encrypted files, never entries without payloads.
    QString message;
    if (!writeManifest(keep, message)) {
        qCWarning(vault_log).noquote() << "purge stopped:" << message;
        return 0;
    }
    entries_ = std::move(keep);
    for (const auto& entry : drop) {
        if (!QFile::remove(payloadPath(entry.id))) {
            qCWarning(vault_log).noquote() << "an expired encrypted copy could not be deleted:" << entry.id;
        }
    }
    return static_cast<int>(drop.size());
}

bool Vault::hasRoomFor(const quint64 bytes) const {
    const QStorageInfo storage{directory_};
    const auto available = storage.bytesAvailable();
    return storage.isValid() && available >= 0 && has_room(static_cast<quint64>(available), bytes);
}

}  // namespace pms::desktop
