#include "vault/vault.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <utility>

#include "platform/durable.hpp"
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
constexpr Magic journal_magic{'P', 'M', 'S', 'J', 'R', 'N', 'L', '2'};
constexpr quint32 format_version = 2;
constexpr qsizetype key_size = 32;
constexpr qsizetype nonce_bytes = 12;
constexpr qsizetype tag_bytes = 16;
constexpr qint64 aad_header_bytes = 8 + 4 + 8 + nonce_bytes;   // payload: magic | version | size | nonce
constexpr qint64 header_bytes = aad_header_bytes + tag_bytes;  // + tag
constexpr qint64 journal_header_bytes = 8 + 4 + 16;           // magic | version | random journal id
constexpr qsizetype id_bytes = 36;                             // a UUID without braces
constexpr qint64 record_limit = 64 * 1024;                     // ciphertext of one entry
constexpr qsizetype text_limit = 1024;
constexpr qint64 orphan_age_ms = 24LL * 60 * 60 * 1000;
constexpr auto uuid_pattern = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

void wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        volatile auto* data = bytes.data();
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            data[index] = 0;
        }
    }
    bytes.clear();
}

// Hashes and counts what passes through, forwarding it to `destination` (nullptr: nothing is written anywhere).
// A failed destination write is remembered, so callers can tell "could not write" from "did not authenticate".
class VerificationSink final : public QIODevice {
public:
    explicit VerificationSink(QIODevice* destination) : destination_(destination) {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }

    [[nodiscard]] quint64 written() const noexcept { return written_; }
    [[nodiscard]] QByteArray digest() const { return hash_.result(); }
    [[nodiscard]] const QString& destinationError() const noexcept { return destination_error_; }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, const qint64 length) override {
        if (destination_ != nullptr && destination_->write(data, length) != length) {
            destination_error_ = destination_->errorString().isEmpty() ? QStringLiteral("the write failed")
                                                                       : destination_->errorString();
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
    QString destination_error_;
};

// Up to `size` bytes; fewer only at the end of the input. nullopt on a read error. A file or buffer knows what is
// left, so a 300-byte journal record never costs a 4 MiB allocation.
std::optional<QByteArray> read_full(QIODevice& input, const qint64 size) {
    const auto want = input.isSequential() ? size : std::clamp<qint64>(input.size() - input.pos(), 0, size);
    QByteArray buffer(want, Qt::Uninitialized);
    qint64 filled = 0;
    while (filled < want) {
        const auto got = input.read(buffer.data() + filled, want - filled);
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

// An AES-256-GCM key handle, made once and used for many calls (opening a 10k-record journal).
class GcmKey final {
public:
    explicit GcmKey(const QByteArray& bytes) {
        if (bytes.size() != key_size ||
            !success(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
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
    ~GcmKey() {
        if (key_ != nullptr) {
            BCryptDestroyKey(key_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
        wipe(object_);
    }
    GcmKey(const GcmKey&) = delete;
    GcmKey& operator=(const GcmKey&) = delete;

    [[nodiscard]] bool valid() const noexcept { return key_ != nullptr; }
    [[nodiscard]] BCRYPT_KEY_HANDLE handle() const noexcept { return key_; }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_KEY_HANDLE key_{};
    QByteArray object_;
};

bool gcm_stream(const GcmKey& key, QIODevice& input, QIODevice& output, const QByteArray& nonce, const QByteArray& aad,
                QByteArray& tag, const bool encrypt, const qint64 chunk_bytes, QCryptographicHash* input_hash) {
    if (!key.valid()) {
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
        const auto status = call(key.handle(), reinterpret_cast<PUCHAR>(current->data()),
                                 static_cast<ULONG>(current->size()), &info,
                                 chained ? reinterpret_cast<PUCHAR>(chain_iv.data()) : nullptr,
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
class GcmKey final {
public:
    explicit GcmKey(const QByteArray& bytes) : bytes_(bytes) {}
    [[nodiscard]] bool valid() const noexcept { return bytes_.size() == key_size; }
    [[nodiscard]] const unsigned char* data() const noexcept {
        return reinterpret_cast<const unsigned char*>(bytes_.constData());
    }

private:
    const QByteArray& bytes_;
};

bool gcm_stream(const GcmKey& key, QIODevice& input, QIODevice& output, const QByteArray& nonce, const QByteArray& aad,
                QByteArray& tag, const bool encrypt, const qint64 chunk_bytes, QCryptographicHash* input_hash) {
    if (!key.valid()) {
        return false;
    }
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
        init(context, nullptr, nullptr, key.data(), bytes(nonce)) != 1) {
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

std::optional<QByteArray> gcm_buffer(const GcmKey& key, const QByteArray& in, const QByteArray& nonce, const QByteArray& aad,
                                     QByteArray& tag, const bool encrypt) {
    QBuffer input;
    input.setData(in);
    input.open(QIODevice::ReadOnly);
    QByteArray out;
    QBuffer output{&out};
    output.open(QIODevice::WriteOnly);
    if (!gcm_stream(key, input, output, nonce, aad, tag, encrypt, detail::gcm_chunk_bytes, nullptr)) {
        wipe(out);
        return std::nullopt;
    }
    output.close();
    return out;
}

// POSIX: 0600 on the temporary file before it gets its final name (Windows inherits the profile's ACL).
bool restrict_open_file(QFileDevice& file) {
#ifdef Q_OS_WIN
    Q_UNUSED(file);
    return true;
#else
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

// ---- payload files ----

QByteArray payload_header(const quint64 size, const QByteArray& nonce) {
    QByteArray header(aad_header_bytes, Qt::Uninitialized);
    std::copy(payload_magic.begin(), payload_magic.end(), header.begin());
    qToLittleEndian(format_version, header.data() + 8);
    qToLittleEndian(size, header.data() + 12);
    std::copy(nonce.begin(), nonce.end(), header.begin() + 20);
    return header;
}

// Writes header, tag and ciphertext of exactly `size` plaintext bytes; the caller commits `output`.
bool seal_payload(const GcmKey& key, QIODevice& input, const quint64 size, QSaveFile& output, const QByteArray& id,
                  QCryptographicHash* hash, QString& error) {
    QByteArray nonce(nonce_bytes, Qt::Uninitialized);
    if (!fill_random(nonce)) {
        error = QStringLiteral("A random nonce could not be generated");
        return false;
    }
    const auto header = payload_header(size, nonce);
    QByteArray tag(tag_bytes, '\0');
    if (output.write(header) != header.size() || output.write(tag) != tag.size() ||
        !gcm_stream(key, input, output, nonce, header + id, tag, true, detail::gcm_chunk_bytes, hash)) {
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

// Decrypts a payload into `sink` and returns its authenticated plaintext size.
std::optional<quint64> unseal_payload(const GcmKey& key, QFile& input, const QByteArray& id, QIODevice& sink,
                                      QString& error) {
    const auto header = input.read(header_bytes);
    if (header.size() != header_bytes || !std::equal(payload_magic.begin(), payload_magic.end(), header.begin()) ||
        qFromLittleEndian<quint32>(header.constData() + 8) != format_version) {
        error = QStringLiteral("its header is not valid");
        return std::nullopt;
    }
    const auto size = qFromLittleEndian<quint64>(header.constData() + 12);
    if (static_cast<quint64>(input.size() - header_bytes) != size) {
        error = QStringLiteral("its length does not match its header");
        return std::nullopt;
    }
    auto tag = header.mid(aad_header_bytes, tag_bytes);
    if (!gcm_stream(key, input, sink, header.mid(20, nonce_bytes), header.left(aad_header_bytes) + id, tag, false,
                    detail::gcm_chunk_bytes, nullptr)) {
        error = QStringLiteral("it does not authenticate");
        return std::nullopt;
    }
    return size;
}

// ---- journal ----

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

std::optional<VaultEntry> parse_entry(const QByteArray& json) {
    static const QRegularExpression uuid{QStringLiteral("^%1$").arg(QLatin1StringView{uuid_pattern})};
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto object = document.object();
    for (const auto* field : {"id", "sourceKey", "sourceId", "originalName", "bytes", "sha256", "archivedMs"}) {
        if (!object.value(QLatin1StringView{field}).isString()) {
            return std::nullopt;
        }
    }
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

QByteArray new_journal_header() {
    QByteArray header(journal_header_bytes, Qt::Uninitialized);
    std::copy(journal_magic.begin(), journal_magic.end(), header.begin());
    qToLittleEndian(format_version, header.data() + 8);
    QByteArray journal_id(16, Qt::Uninitialized);
    if (!fill_random(journal_id)) {
        return {};
    }
    std::copy(journal_id.begin(), journal_id.end(), header.begin() + 12);
    return header;
}

QByteArray record_aad(const QByteArray& header, const quint64 index, const QByteArray& id) {
    std::array<char, 8> raw{};
    qToLittleEndian(index, raw.data());
    return header + QByteArray{raw.data(), static_cast<qsizetype>(raw.size())} + id;
}

// u32 ciphertext length | u8 36 | entry id | nonce | tag | ciphertext
std::optional<QByteArray> seal_record(const GcmKey& key, const QByteArray& header, const quint64 index,
                                      const VaultEntry& entry) {
    const auto id = entry.id.toLatin1();
    const auto json = QJsonDocument{to_json(entry)}.toJson(QJsonDocument::Compact);
    QByteArray nonce(nonce_bytes, Qt::Uninitialized);
    QByteArray tag(tag_bytes, '\0');
    if (id.size() != id_bytes || json.size() > record_limit || !fill_random(nonce)) {
        return std::nullopt;
    }
    const auto sealed = gcm_buffer(key, json, nonce, record_aad(header, index, id), tag, true);
    if (!sealed) {
        return std::nullopt;
    }
    std::array<char, 4> length{};
    qToLittleEndian(static_cast<quint32>(sealed->size()), length.data());
    QByteArray record;
    record.reserve(4 + 1 + id_bytes + nonce_bytes + tag_bytes + sealed->size());
    record.append(length.data(), static_cast<qsizetype>(length.size()));
    record.append(static_cast<char>(id_bytes));
    record.append(id);
    record.append(nonce);
    record.append(tag);
    record.append(*sealed);
    return record;
}

struct JournalRead {
    QList<VaultEntry> entries;  // every record up to the first bad one
    QByteArray header;
    bool damaged{};
    QString problem;
};

JournalRead read_journal(const QString& path, const QByteArray& key_bytes) {
    JournalRead result;
    QFile file{path};
    result.header = file.open(QIODevice::ReadOnly) ? file.read(journal_header_bytes) : QByteArray{};
    if (result.header.size() != journal_header_bytes ||
        !std::equal(journal_magic.begin(), journal_magic.end(), result.header.begin()) ||
        qFromLittleEndian<quint32>(result.header.constData() + 8) != format_version) {
        result.damaged = true;
        result.problem = QStringLiteral("its header is not valid");
        result.header.clear();
        return result;
    }
    const GcmKey key{key_bytes};
    constexpr qint64 fixed = 1 + id_bytes + nonce_bytes + tag_bytes;
    for (quint64 index = 0;; ++index) {
        const auto length_bytes = file.read(4);
        if (length_bytes.isEmpty()) {
            break;  // a clean end
        }
        const auto bad = [&result, index](const char* why) {
            result.damaged = true;
            result.problem = QStringLiteral("record %1 %2").arg(index).arg(QLatin1StringView{why});
        };
        const auto length = length_bytes.size() == 4 ? qFromLittleEndian<quint32>(length_bytes.constData()) : 0U;
        if (length_bytes.size() != 4 || length == 0 || length > record_limit) {
            bad(length_bytes.size() != 4 ? "is cut off" : "has an impossible length");
            break;
        }
        const auto rest = file.read(fixed + static_cast<qint64>(length));
        if (rest.size() != fixed + static_cast<qint64>(length)) {
            bad("is cut off");
            break;
        }
        if (static_cast<unsigned char>(rest[0]) != id_bytes) {
            bad("is malformed");
            break;
        }
        const auto id = rest.mid(1, id_bytes);
        auto tag = rest.mid(1 + id_bytes + nonce_bytes, tag_bytes);
        auto json = gcm_buffer(key, rest.mid(fixed), rest.mid(1 + id_bytes, nonce_bytes),
                               record_aad(result.header, index, id), tag, false);
        if (!json) {
            bad("does not authenticate");
            break;
        }
        auto entry = parse_entry(*json);
        if (!entry || entry->id.toLatin1() != id) {
            bad("is not a valid entry");
            break;
        }
        result.entries.push_back(std::move(*entry));
    }
    return result;
}

// ---- restore names ----

// A plain file name from whatever the phone called the file: no folders, no characters Windows rejects,
// no device names (CON, NUL, COM1, COM¹, CONIN$, "NUL .jpg"...).
QString safe_file_name(const QString& original, const QString& fallback) {
    static const QRegularExpression separators{QStringLiteral("[/\\\\]")};
    static const QRegularExpression invalid{QStringLiteral("[<>:\"|?*\\x00-\\x1f]")};
    static const QRegularExpression reserved{
        QStringLiteral("^(?:con|prn|aux|nul|conin\\$|conout\\$|com[0-9\\x{00B9}\\x{00B2}\\x{00B3}]|"
                       "lpt[0-9\\x{00B9}\\x{00B2}\\x{00B3}])(?:[ .].*)?$"),
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

// Creates "IMG_1.JPG", else "IMG_1 2.JPG", "IMG_1 3.JPG"... with an exclusive create, so the name is ours and
// no existing file is ever replaced, even one that appears while we look.
QString reserve_target(const QDir& directory, const QString& name, QString& error) {
    const QFileInfo info{name};
    const auto base = info.completeBaseName();
    const auto suffix = info.suffix();
    for (int copy = 1; copy < 10000; ++copy) {
        const auto candidate = copy == 1          ? name
                               : suffix.isEmpty() ? QStringLiteral("%1 %2").arg(base, QString::number(copy))
                                                  : QStringLiteral("%1 %2.%3").arg(base, QString::number(copy), suffix);
        const auto path = directory.filePath(candidate);
        QFile file{path};
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return path;
        }
        if (!QFileInfo::exists(path)) {
            error = QStringLiteral("The restored file could not be created: %1").arg(file.errorString());
            return {};
        }
    }
    error = QStringLiteral("No free file name was found in the restore folder");
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
    if (key.size() != key_size || nonce.size() != nonce_bytes || tag.size() != tag_bytes || chunk_bytes <= 0 ||
        chunk_bytes % 16 != 0) {
        return false;
    }
    const GcmKey prepared{key};
    return gcm_stream(prepared, input, output, nonce, aad, tag, encrypt, chunk_bytes, input_hash);
}

bool detail::write_journal(const QString& path, const QByteArray& key_bytes, const QList<VaultEntry>& entries,
                           QString& error, QByteArray* header) {
    const GcmKey key{key_bytes};
    const auto fresh = new_journal_header();
    if (!key.valid() || fresh.isEmpty()) {
        error = QStringLiteral("The recovery vault key is not usable");
        return false;
    }
    QSaveFile output{path};
    if (!output.open(QIODevice::WriteOnly) || !restrict_open_file(output) || output.write(fresh) != fresh.size()) {
        error = QStringLiteral("The recovery vault index could not be written: %1").arg(output.errorString());
        return false;
    }
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto record = seal_record(key, fresh, static_cast<quint64>(index), entries[index]);
        if (!record || output.write(*record) != record->size()) {
            error = QStringLiteral("The recovery vault index could not be written: %1").arg(output.errorString());
            return false;
        }
    }
    if (!output.commit() || !sync_directory(QFileInfo{path}.absolutePath())) {
        error = QStringLiteral("The recovery vault index could not be saved: %1").arg(output.errorString());
        return false;
    }
    if (header != nullptr) {
        *header = fresh;
    }
    return true;
}

Vault::Vault(QString directory) : directory_(std::move(directory)) {}

Vault::~Vault() {
    wipe(key_);
}

bool Vault::ready() const noexcept {
    return lock_ != nullptr && key_.size() == key_size && !needs_recovery_;
}

QString Vault::payloadPath(const QString& id) const {
    return QDir{directory_}.filePath(id + QStringLiteral(".pmsv"));
}

QString Vault::journalPath() const {
    return QDir{directory_}.filePath(QStringLiteral("journal.pmsj"));
}

bool Vault::open(QString* error) {
    wipe(key_);
    lock_.reset();
    entries_.clear();
    journal_header_.clear();
    journal_records_ = 0;
    needs_recovery_ = false;
    // keep: the lock and key stay so recoverIndex() can run.
    const auto fail = [this, error](const QString& message, const bool keep) {
        qCWarning(vault_log).noquote() << message;
        if (error != nullptr) {
            *error = message;
        }
        if (!keep) {
            wipe(key_);
            lock_.reset();
        }
        return false;
    };
    if (directory_.isEmpty() || !QDir{}.mkpath(directory_)) {
        return fail(QStringLiteral("The recovery vault folder could not be created"), false);
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return fail(QStringLiteral("The recovery vault folder could not be restricted to the current user"), false);
    }
#endif
    lock_ = std::make_unique<QLockFile>(QDir{directory_}.filePath(QStringLiteral("vault.lock")));
    lock_->setStaleLockTime(0);  // held for the object's lifetime; only a dead owner makes it stale
    if (!lock_->tryLock(0)) {
        return fail(QStringLiteral("The recovery vault is open in another window of Phone Memory Slider"), false);
    }
    QString message;
    auto key = KeyStore::loadOrCreate(QStringLiteral("vault"), &message);
    if (!key) {
        return fail(message, false);
    }
    key_ = std::move(*key);

    const auto journal = journalPath();
    if (!QFile::exists(journal)) {
        const auto payloads = QDir{directory_}.entryList({QStringLiteral("*.pmsv")}, QDir::Files);
        if (!payloads.isEmpty()) {
            needs_recovery_ = true;
            return fail(QStringLiteral("The recovery vault's index is missing but the vault holds %1 encrypted files; "
                                       "they can be indexed again")
                            .arg(payloads.size()),
                        true);
        }
        if (!detail::write_journal(journal, key_, {}, message, &journal_header_)) {
            return fail(message, false);
        }
        return true;
    }
    auto read = read_journal(journal, key_);
    if (read.damaged) {
        needs_recovery_ = true;
        return fail(QStringLiteral("The recovery vault's index is damaged (%1); the %2 entries before that point are "
                                   "intact and the rest can be indexed again from the encrypted files")
                        .arg(read.problem)
                        .arg(read.entries.size()),
                    true);
    }
    entries_ = std::move(read.entries);
    journal_header_ = std::move(read.header);
    journal_records_ = static_cast<quint64>(entries_.size());
    sweepOrphans();
    return true;
}

bool Vault::recoverIndex(int* recovered, QString* error) {
    const auto fail = [error](const QString& message) {
        qCWarning(vault_log).noquote() << message;
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (lock_ == nullptr || key_.size() != key_size) {
        return fail(QStringLiteral("The recovery vault is not open"));
    }
    QList<VaultEntry> kept;
    if (QFile::exists(journalPath())) {
        kept = read_journal(journalPath(), key_).entries;
    }
    QHash<QString, VaultEntry> previous;
    const auto previous_path = journalPath() + QStringLiteral(".prev");
    if (QFile::exists(previous_path)) {
        for (const auto& entry : read_journal(previous_path, key_).entries) {
            previous.insert(entry.id, entry);
        }
    }
    QSet<QString> known;
    for (const auto& entry : kept) {
        known.insert(entry.id);
    }
    static const QRegularExpression payload_name{
        QStringLiteral("^(%1)\\.pmsv$").arg(QLatin1StringView{uuid_pattern})};
    const GcmKey key{key_};
    int count = 0;
    for (const auto& info : QDir{directory_}.entryInfoList({QStringLiteral("*.pmsv")}, QDir::Files, QDir::Name)) {
        const auto match = payload_name.match(info.fileName());
        if (!match.hasMatch() || known.contains(match.captured(1))) {
            continue;
        }
        const auto id = match.captured(1);
        QFile input{info.filePath()};
        VerificationSink sink{nullptr};
        QString problem;
        const auto size = input.open(QIODevice::ReadOnly) ? unseal_payload(key, input, id.toLatin1(), sink, problem)
                                                          : std::nullopt;
        if (!size || sink.written() != *size) {
            qCWarning(vault_log).noquote() << "not indexed," << info.fileName() << "does not authenticate:" << problem;
            continue;
        }
        VaultEntry entry;
        const auto old = previous.constFind(id);
        if (old != previous.cend() && old->bytes == *size && old->sha256 == sink.digest()) {
            entry = *old;
        } else {
            entry.id = id;
            entry.original_name = QStringLiteral("restored-") + id;
            entry.bytes = *size;
            entry.sha256 = sink.digest();
            entry.archived_ms = info.lastModified().toMSecsSinceEpoch();
        }
        kept.push_back(entry);
        known.insert(id);
        ++count;
    }
    QString message;
    if (!compact(kept, message)) {
        return fail(message);
    }
    entries_ = std::move(kept);
    needs_recovery_ = false;
    if (recovered != nullptr) {
        *recovered = count;
    }
    return true;
}

ArchiveResult Vault::sealPayload(const ArchiveRequest& request) const {
    const auto failed = [](const QString& message) {
        qCWarning(vault_log).noquote() << message;
        return ArchiveResult{std::nullopt, message};
    };
    if (request.source_key.size() > text_limit || request.source_id.size() > text_limit || request.name.size() > text_limit) {
        return failed(QStringLiteral("The item's name or id is too long for the recovery vault"));
    }
    QFile input{request.file};
    if (!QFileInfo{request.file}.isFile() || !input.open(QIODevice::ReadOnly)) {
        return failed(QStringLiteral("The file to keep in the recovery vault could not be read"));
    }
    VaultEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.source_key = request.source_key;
    entry.source_id = request.source_id;
    entry.original_name = request.name.isEmpty() ? QFileInfo{request.file}.fileName() : request.name;
    entry.bytes = static_cast<quint64>(input.size());
    entry.archived_ms = QDateTime::currentMSecsSinceEpoch();
    const auto path = payloadPath(entry.id);

    QString message;
    {
        const GcmKey key{key_};
        QSaveFile output{path};
        QCryptographicHash hash{QCryptographicHash::Sha256};
        if (!output.open(QIODevice::WriteOnly) || !restrict_open_file(output) ||
            !seal_payload(key, input, entry.bytes, output, entry.id.toLatin1(), &hash, message) || !output.commit()) {
            if (message.isEmpty()) {
                message = QStringLiteral("The encrypted copy could not be saved: %1").arg(output.errorString());
            }
            output.cancelWriting();
            return failed(message);
        }
        entry.sha256 = hash.result();
    }
    // Check what is on disk, not what was in memory: decrypt it into a hash sink. Nothing is written.
    if (!decryptPayload(entry, nullptr, message)) {
        QFile::remove(path);
        return failed(message);
    }
    return {entry, {}};
}

bool Vault::appendJournal(const QList<VaultEntry>& entries, QString& error) {
    const GcmKey key{key_};
    QByteArray records;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto record = seal_record(key, journal_header_, journal_records_ + static_cast<quint64>(index), entries[index]);
        if (!record) {
            error = QStringLiteral("An index record could not be sealed");
            return false;
        }
        records += *record;
    }
    QFile journal{journalPath()};
    if (!journal.open(QIODevice::ReadWrite)) {
        error = QStringLiteral("The recovery vault index could not be opened: %1").arg(journal.errorString());
        return false;
    }
    const auto end = journal.size();
    if (!journal.seek(end) || journal.write(records) != records.size() || !sync_file(journal)) {
        error = QStringLiteral("The recovery vault index could not be updated: %1").arg(journal.errorString());
        if (!journal.resize(end)) {  // leave no partial record behind
            qCWarning(vault_log) << "the index could not be trimmed back; the next open will offer recovery";
        }
        return false;
    }
    journal_records_ += static_cast<quint64>(entries.size());
    return true;
}

bool Vault::compact(const QList<VaultEntry>& entries, QString& error) {
    const auto journal = journalPath();
    if (QFile::exists(journal)) {
        const auto previous = journal + QStringLiteral(".prev");
        QFile::remove(previous);
        if (!QFile::copy(journal, previous)) {
            error = QStringLiteral("The previous recovery vault index could not be kept");
            return false;
        }
    }
    QByteArray header;
    if (!detail::write_journal(journal, key_, entries, error, &header)) {
        return false;
    }
    journal_header_ = header;
    journal_records_ = static_cast<quint64>(entries.size());
    return true;
}

std::optional<VaultEntry> Vault::archive(const QString& file, const QString& source_key, const QString& source_id,
                                         const QString& name, QString* error) {
    auto results = archiveBatch({ArchiveRequest{file, source_key, source_id, name}});
    if (!results.front().entry && error != nullptr) {
        *error = results.front().error;
    }
    return results.front().entry;
}

QList<ArchiveResult> Vault::archiveBatch(const QList<ArchiveRequest>& requests) {
    QList<ArchiveResult> results;
    results.reserve(requests.size());
    if (!ready()) {
        const auto message = needs_recovery_ ? QStringLiteral("The recovery vault's index has to be rebuilt first")
                                             : QStringLiteral("The recovery vault is not open");
        for (qsizetype index = 0; index < requests.size(); ++index) {
            results.push_back({std::nullopt, message});
        }
        return results;
    }
    QList<VaultEntry> sealed;
    for (const auto& request : requests) {
        results.push_back(sealPayload(request));
        if (results.back().entry) {
            sealed.push_back(*results.back().entry);
        }
    }
    if (sealed.isEmpty()) {
        return results;
    }
    // The payloads' names must be durable before the journal points at them.
    QString message;
    if (!sync_directory(directory_)) {
        message = QStringLiteral("The encrypted copies could not be made durable");
    }
    if (!message.isEmpty() || !appendJournal(sealed, message)) {
        for (auto& result : results) {
            if (result.entry) {
                QFile::remove(payloadPath(result.entry->id));
                result = {std::nullopt, message};
            }
        }
        return results;
    }
    entries_ += sealed;
    return results;
}

bool Vault::decryptPayload(const VaultEntry& entry, QIODevice* destination, QString& error) const {
    QFile input{payloadPath(entry.id)};
    if (!input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("The archived copy is missing from the recovery vault");
        return false;
    }
    const GcmKey key{key_};
    VerificationSink sink{destination};
    QString problem;
    const auto size = unseal_payload(key, input, entry.id.toLatin1(), sink, problem);
    if (!sink.destinationError().isEmpty()) {
        error = QStringLiteral("The restored file could not be written: %1").arg(sink.destinationError());
        return false;
    }
    if (!size) {
        error = QStringLiteral("The archived copy failed its integrity check: %1").arg(problem);
        return false;
    }
    if (*size != entry.bytes || sink.written() != entry.bytes || sink.digest() != entry.sha256) {
        error = QStringLiteral("The archived copy failed its integrity check: it does not match its index entry");
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
    if (!ready()) {
        return fail(needs_recovery_ ? QStringLiteral("The recovery vault's index has to be rebuilt first")
                                    : QStringLiteral("The recovery vault is not open"));
    }
    const auto entry = std::find_if(entries_.cbegin(), entries_.cend(),
                                    [&entry_id](const VaultEntry& candidate) { return candidate.id == entry_id; });
    if (entry == entries_.cend()) {
        return fail(QStringLiteral("This item is not in the recovery vault"));
    }
    if (destination_dir.isEmpty() || !QDir{}.mkpath(destination_dir)) {
        return fail(QStringLiteral("The restore folder could not be created"));
    }
    QString message;
    const auto target = reserve_target(
        QDir{destination_dir}, safe_file_name(entry->original_name, QStringLiteral("restored-") + entry->id), message);
    if (target.isEmpty()) {
        return fail(message);
    }
    // The reserved (empty) file is replaced by a QSaveFile that only commits once every check passed.
    QSaveFile output{target};
    if (!output.open(QIODevice::WriteOnly)) {
        QFile::remove(target);
        return fail(QStringLiteral("The restored file could not be created: %1").arg(output.errorString()));
    }
    if (!decryptPayload(*entry, &output, message)) {
        output.cancelWriting();
        QFile::remove(target);
        return fail(message);
    }
    if (!output.commit()) {
        const auto reason = output.errorString();
        QFile::remove(target);
        return fail(QStringLiteral("The restored file could not be written: %1").arg(reason));
    }
    if (restored_path != nullptr) {
        *restored_path = target;
    }
    return true;
}

void Vault::sweepOrphans() const {
    QSet<QString> indexed;
    for (const auto& entry : entries_) {
        indexed.insert(entry.id + QStringLiteral(".pmsv"));
    }
    static const QRegularExpression payload{QStringLiteral("^%1\\.pmsv$").arg(QLatin1StringView{uuid_pattern})};
    // QSaveFile's temporaries: "<name>.XXXXXX".
    static const QRegularExpression leftover{
        QStringLiteral("^(?:%1\\.pmsv|journal\\.pmsj)\\.[A-Za-z0-9]{6}$").arg(QLatin1StringView{uuid_pattern})};
    const auto cutoff = QDateTime::currentDateTimeUtc().addMSecs(-orphan_age_ms);
    for (const auto& info : QDir{directory_}.entryInfoList(QDir::Files | QDir::Hidden)) {
        const auto name = info.fileName();
        const auto orphan = (payload.match(name).hasMatch() && !indexed.contains(name)) || leftover.match(name).hasMatch();
        if (!orphan || info.lastModified().toUTC() >= cutoff) {
            continue;
        }
        if (QFile::remove(info.filePath())) {
            qCInfo(vault_log).noquote() << "removed" << name << "(unindexed for more than 24 hours)";
        } else {
            qCWarning(vault_log).noquote() << "could not remove the unindexed file" << name;
        }
    }
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
    if (!ready()) {
        return 0;
    }
    const auto cutoff = std::min(cutoff_ms, QDateTime::currentMSecsSinceEpoch());
    QList<VaultEntry> keep;
    QList<VaultEntry> drop;
    for (const auto& entry : entries_) {
        (entry.archived_ms < cutoff ? drop : keep).push_back(entry);
    }
    if (drop.isEmpty()) {
        return 0;
    }
    // Index first: a crash after this leaves unindexed encrypted files (swept a day later), never entries
    // without payloads.
    QString message;
    if (!compact(keep, message)) {
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
