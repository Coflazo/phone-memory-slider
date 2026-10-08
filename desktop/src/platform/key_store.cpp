#include "platform/key_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <atomic>

#include "platform/durable.hpp"

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#else
#include <openssl/rand.h>
#include <unistd.h>
#endif
#if defined(Q_OS_MACOS)
#include <Security/Security.h>

#include <memory>
#elif defined(Q_OS_LINUX)
#include <libsecret/secret.h>
#endif

namespace pms::desktop {
namespace {

Q_LOGGING_CATEGORY(key_log, "pms.keystore")

constexpr qsizetype key_bytes = 32;
std::atomic<KeyBackend> last_backend{KeyBackend::None};

void wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        volatile auto* data = bytes.data();
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            data[index] = 0;
        }
    }
    bytes.clear();
}

std::optional<QByteArray> failed(QString* error, const QString& message) {
    qCWarning(key_log).noquote() << message;
    if (error != nullptr) {
        *error = message;
    }
    return std::nullopt;
}

std::optional<QByteArray> found(QByteArray key, const KeyBackend backend) {
    last_backend = backend;
    return key;
}

std::optional<QByteArray> random_key() {
    QByteArray key(key_bytes, Qt::Uninitialized);
#ifdef Q_OS_WIN
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return std::nullopt;
    }
#else
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), static_cast<int>(key.size())) != 1) {
        return std::nullopt;
    }
#endif
    return key;
}

QString keys_directory() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) {
        root = QDir::home().filePath(QStringLiteral(".phone-memory-slider"));
    }
    return root + QStringLiteral("/keys");
}

QString key_path(const QString& name, const QString& suffix) {
    return QDir{keys_directory()}.filePath(name + suffix);
}

std::optional<QByteArray> read_file(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

// Atomic and never replaces `to`: false when `to` exists. QFile::rename is not enough here: it checks for the
// target first and, when the rename itself fails, falls back to a copy that truncates the target.
bool rename_without_replacing(const QString& from, const QString& to) {
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(from).utf16()),
                       reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(to).utf16()),
                       MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    const auto source = QFile::encodeName(from);
    if (::link(source.constData(), QFile::encodeName(to).constData()) != 0) {  // EEXIST: another process won
        return false;
    }
    ::unlink(source.constData());
    return true;
#endif
}

enum class Publish { Written, Exists, Failed };

// Puts `bytes` at `path` only if nothing is there: a temporary file (0600 before any byte is written), fsync, then
// a rename that never replaces. Exists means another process got there first; the caller reads its file instead.
Publish publish_new_file(const QString& path, const QByteArray& bytes) {
    const auto directory = QFileInfo{path}.absolutePath();
    if (!QDir{}.mkpath(directory)) {
        return Publish::Failed;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return Publish::Failed;
    }
#endif
    const auto temporary = path + QLatin1Char('.') + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".tmp");
    {
        QFile file{temporary};
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return Publish::Failed;
        }
#ifndef Q_OS_WIN
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            file.close();
            QFile::remove(temporary);
            return Publish::Failed;
        }
#endif
        const auto written = file.write(bytes) == bytes.size() && sync_file(file);
        file.close();
        if (!written) {
            QFile::remove(temporary);
            return Publish::Failed;
        }
    }
    if (rename_without_replacing(temporary, path)) {
        if (!sync_directory(directory)) {
            qCWarning(key_log) << "the keys folder could not be flushed to disk";
        }
        return Publish::Written;
    }
    QFile::remove(temporary);
    return QFile::exists(path) ? Publish::Exists : Publish::Failed;
}

#if defined(Q_OS_WIN)
std::optional<QByteArray> dpapi_read(const QString& path, QString* error) {
    auto sealed = read_file(path);
    if (!sealed) {
        return failed(error, QStringLiteral("The vault key could not be read"));
    }
    DATA_BLOB input{static_cast<DWORD>(sealed->size()), reinterpret_cast<BYTE*>(sealed->data())};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        return failed(error, QStringLiteral("Windows could not unlock the vault key"));
    }
    QByteArray key{reinterpret_cast<const char*>(plain.pbData), static_cast<qsizetype>(plain.cbData)};
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    if (key.size() != key_bytes) {
        wipe(key);
        return failed(error, QStringLiteral("The vault key is invalid"));
    }
    return found(std::move(key), KeyBackend::Dpapi);
}

std::optional<QByteArray> dpapi_load_or_create(const QString& name, QString* error) {
    const auto path = key_path(name, QStringLiteral(".dpapi"));
    if (QFile::exists(path)) {
        return dpapi_read(path, error);
    }
    auto key = random_key();
    if (!key) {
        return failed(error, QStringLiteral("Windows could not generate a vault key"));
    }
    DATA_BLOB input{static_cast<DWORD>(key->size()), reinterpret_cast<BYTE*>(key->data())};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&input, L"Phone Memory Slider vault key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &sealed)) {
        wipe(*key);
        return failed(error, QStringLiteral("Windows could not protect the vault key"));
    }
    const QByteArray sealed_bytes{reinterpret_cast<const char*>(sealed.pbData), static_cast<qsizetype>(sealed.cbData)};
    LocalFree(sealed.pbData);
    switch (publish_new_file(path, sealed_bytes)) {
    case Publish::Written:
        return found(std::move(*key), KeyBackend::Dpapi);
    case Publish::Exists:  // another first launch won: use its key
        wipe(*key);
        return dpapi_read(path, error);
    case Publish::Failed:
        break;
    }
    wipe(*key);
    return failed(error, QStringLiteral("The vault key could not be saved"));
}
#else
std::optional<QByteArray> file_read(const QString& path, QString* error) {
    auto key = read_file(path);
    if (!key || key->size() != key_bytes) {
        if (key) {
            wipe(*key);
        }
        return failed(error, QStringLiteral("The vault key is unreadable or invalid"));
    }
    return found(std::move(*key), KeyBackend::File);
}

std::optional<QByteArray> file_load_or_create(const QString& name, QString* error) {
    const auto path = key_path(name, QStringLiteral(".key"));
    qCWarning(key_log).noquote() << "No system key store is reachable; the vault key is" << path
                                 << "and only this user can read it";
    if (QFile::exists(path)) {
        return file_read(path, error);
    }
    auto key = random_key();
    if (!key) {
        return failed(error, QStringLiteral("A vault key could not be generated"));
    }
    switch (publish_new_file(path, *key)) {
    case Publish::Written:
        return found(std::move(*key), KeyBackend::File);
    case Publish::Exists:
        wipe(*key);
        return file_read(path, error);
    case Publish::Failed:
        break;
    }
    wipe(*key);
    return failed(error, QStringLiteral("The vault key could not be saved"));
}

constexpr KeyBackend system_backend =
#if defined(Q_OS_MACOS)
    KeyBackend::Keychain;
#else
    KeyBackend::SecretService;
#endif
#endif

#if defined(Q_OS_MACOS)
struct CfRelease {
    void operator()(const void* ref) const noexcept {
        if (ref != nullptr) {
            CFRelease(ref);
        }
    }
};
using CfHandle = std::unique_ptr<const void, CfRelease>;

// No keychain at all (a headless account): the caller may fall back to the file key.
bool keychain_absent(const OSStatus status) {
    return status == errSecNotAvailable || status == errSecNoSuchKeychain;
}

std::optional<QByteArray> system_load_or_create(const QString& name, QString* error, bool& absent) {
    absent = false;
    const CfHandle service{(QStandardPaths::isTestModeEnabled() ? QStringLiteral("Phone Memory Slider (tests)")
                                                                : QStringLiteral("Phone Memory Slider"))
                               .toCFString()};
    const CfHandle account{name.toCFString()};
    const auto lookup = [&service, &account](OSStatus& status) -> std::optional<QByteArray> {
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
        const void* values[] = {kSecClassGenericPassword, service.get(), account.get(), kCFBooleanTrue, kSecMatchLimitOne};
        const CfHandle query{CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks)};
        CFTypeRef result = nullptr;
        status = SecItemCopyMatching(static_cast<CFDictionaryRef>(query.get()), &result);
        const CfHandle owned{result};
        if (status != errSecSuccess) {
            return std::nullopt;
        }
        return QByteArray::fromCFData(static_cast<CFDataRef>(result));
    };
    const auto checked = [error](std::optional<QByteArray> key) -> std::optional<QByteArray> {
        if (key->size() != key_bytes) {
            wipe(*key);
            return failed(error, QStringLiteral("The vault key in the Keychain is invalid"));
        }
        return key;
    };
    OSStatus status{};
    if (auto key = lookup(status)) {
        return checked(std::move(key));
    }
    if (keychain_absent(status)) {
        absent = true;
        return std::nullopt;
    }
    if (status != errSecItemNotFound) {  // e.g. a locked keychain: never replace a key we cannot read
        return failed(error, QStringLiteral("The Keychain could not be read (error %1)").arg(status));
    }
    auto key = random_key();
    if (!key) {
        return failed(error, QStringLiteral("A vault key could not be generated"));
    }
    const CfHandle data{key->toCFData()};
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* values[] = {kSecClassGenericPassword, service.get(), account.get(), data.get()};
    const CfHandle attributes{CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks)};
    status = SecItemAdd(static_cast<CFDictionaryRef>(attributes.get()), nullptr);
    if (status == errSecSuccess) {
        return key;
    }
    wipe(*key);
    if (status == errSecDuplicateItem) {  // another first launch won: use its key
        if (auto winner = lookup(status)) {
            return checked(std::move(winner));
        }
    }
    if (keychain_absent(status)) {
        absent = true;
        return std::nullopt;
    }
    return failed(error, QStringLiteral("The Keychain could not store the vault key (error %1)").arg(status));
}
#elif defined(Q_OS_LINUX)
const SecretSchema* key_schema() {
    static const SecretSchema schema = [] {
        SecretSchema value{};
        value.name = "app.pms.PhoneMemorySlider.Key";
        value.flags = SECRET_SCHEMA_NONE;
        value.attributes[0] = {"name", SECRET_SCHEMA_ATTRIBUTE_STRING};
        value.attributes[1] = {"scope", SECRET_SCHEMA_ATTRIBUTE_STRING};
        return value;
    }();
    return &schema;
}

QString take_error(GError*& error) {
    const auto message = QString::fromUtf8(error != nullptr ? error->message : "unknown error");
    g_clear_error(&error);
    return message;
}

std::optional<QByteArray> system_load_or_create(const QString& name, QString* error, bool& absent) {
    absent = false;
    const auto account = name.toUtf8();
    const char* scope = QStandardPaths::isTestModeEnabled() ? "tests" : "app";
    const auto lookup = [&account, scope](GError** problem) {
        return secret_password_lookup_sync(key_schema(), nullptr, problem, "name", account.constData(), "scope", scope,
                                           static_cast<const char*>(nullptr));
    };
    GError* problem = nullptr;
    gchar* stored = lookup(&problem);
    if (problem != nullptr) {  // no session bus or no Secret Service: a headless session
        qCWarning(key_log).noquote() << "The Secret Service is not reachable:" << take_error(problem);
        absent = true;
        return std::nullopt;
    }
    if (stored == nullptr) {
        auto key = random_key();
        if (!key) {
            return failed(error, QStringLiteral("A vault key could not be generated"));
        }
        auto encoded = key->toBase64();
        wipe(*key);
        const auto saved = secret_password_store_sync(key_schema(), SECRET_COLLECTION_DEFAULT,
                                                      "Phone Memory Slider vault key", encoded.constData(), nullptr,
                                                      &problem, "name", account.constData(), "scope", scope,
                                                      static_cast<const char*>(nullptr));
        wipe(encoded);
        if (saved == FALSE) {
            return failed(error, QStringLiteral("The Secret Service could not store the vault key: %1").arg(take_error(problem)));
        }
        // Read back what is stored, so two first launches end up with the same key.
        stored = lookup(&problem);
        if (problem != nullptr || stored == nullptr) {
            return failed(error, QStringLiteral("The Secret Service lost the vault key it just stored: %1")
                                     .arg(take_error(problem)));
        }
    }
    QByteArray encoded{stored};
    secret_password_free(stored);  // clears, then frees
    auto key = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    wipe(encoded);
    if (key.size() != key_bytes) {
        wipe(key);
        return failed(error, QStringLiteral("The vault key in the Secret Service is invalid"));
    }
    return key;
}
#endif

}  // namespace

std::optional<QByteArray> KeyStore::loadOrCreate(const QString& name, QString* error) {
    static const QRegularExpression valid{QStringLiteral("^[A-Za-z0-9_-]{1,64}$")};
    if (!valid.match(name).hasMatch()) {
        return failed(error, QStringLiteral("Invalid key name"));
    }
#if defined(Q_OS_WIN)
    return dpapi_load_or_create(name, error);
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    // A file key exists only if an earlier run found no system store; keep using it so the vault stays readable.
    if (QFile::exists(key_path(name, QStringLiteral(".key")))) {
        return file_load_or_create(name, error);
    }
    const auto marker = key_path(name, QStringLiteral(".system-store"));
    bool absent = false;
    auto key = system_load_or_create(name, error, absent);
    if (key) {
        if (!QFile::exists(marker) && publish_new_file(marker, QByteArrayLiteral("system key store\n")) == Publish::Failed) {
            qCWarning(key_log) << "the system key store marker could not be written";
        }
        return found(std::move(*key), system_backend);
    }
    if (!absent) {
        return std::nullopt;
    }
    if (QFile::exists(marker)) {
        return failed(error, QStringLiteral("The system key store that holds the vault key is not reachable; the vault "
                                            "stays closed rather than start over with a new key"));
    }
    return file_load_or_create(name, error);
#else
    return file_load_or_create(name, error);
#endif
}

KeyBackend KeyStore::backend() noexcept {
    return last_backend;
}

}  // namespace pms::desktop
