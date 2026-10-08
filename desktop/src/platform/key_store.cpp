#include "platform/key_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#else
#include <openssl/rand.h>
#endif
#if defined(Q_OS_MACOS)
#include <Security/Security.h>

#include <memory>
#endif

namespace pms::desktop {
namespace {

Q_LOGGING_CATEGORY(key_log, "pms.keystore")

constexpr qsizetype key_bytes = 32;

std::optional<QByteArray> failed(QString* error, const QString& message) {
    qCWarning(key_log).noquote() << message;
    if (error != nullptr) {
        *error = message;
    }
    return std::nullopt;
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

bool write_private_file(const QString& path, const QByteArray& bytes) {
    if (!QDir{}.mkpath(QFileInfo{path}.absolutePath())) {
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(QFileInfo{path}.absolutePath(),
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return false;
    }
#endif
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return false;
    }
#ifndef Q_OS_WIN
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#else
    return true;
#endif
}

std::optional<QByteArray> read_file(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

#if defined(Q_OS_WIN)
std::optional<QByteArray> dpapi_load_or_create(const QString& name, QString* error) {
    const auto path = QDir{keys_directory()}.filePath(name + QStringLiteral(".dpapi"));
    if (QFile::exists(path)) {
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
            return failed(error, QStringLiteral("The vault key is invalid"));
        }
        return key;
    }
    auto key = random_key();
    if (!key) {
        return failed(error, QStringLiteral("Windows could not generate a vault key"));
    }
    DATA_BLOB input{static_cast<DWORD>(key->size()), reinterpret_cast<BYTE*>(key->data())};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&input, L"Phone Memory Slider vault key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &sealed)) {
        return failed(error, QStringLiteral("Windows could not protect the vault key"));
    }
    const QByteArray sealed_bytes{reinterpret_cast<const char*>(sealed.pbData), static_cast<qsizetype>(sealed.cbData)};
    LocalFree(sealed.pbData);
    if (!write_private_file(path, sealed_bytes)) {
        return failed(error, QStringLiteral("The vault key could not be saved"));
    }
    return key;
}
#else
QString file_key_path(const QString& name) {
    return QDir{keys_directory()}.filePath(name + QStringLiteral(".key"));
}

// ponytail: Linux has no libsecret backend yet (CI has no Secret Service to test it against); the key is a
// 0600 file in a 0700 folder, which is what v1 shipped. Add libsecret behind this function when a Linux user asks.
std::optional<QByteArray> file_load_or_create(const QString& name, QString* error) {
    const auto path = file_key_path(name);
    qCWarning(key_log).noquote() << "No system key store is in use; the vault key is" << path
                                 << "and only this user can read it";
    if (QFile::exists(path)) {
        const auto key = read_file(path);
        if (!key || key->size() != key_bytes) {
            return failed(error, QStringLiteral("The vault key is unreadable or invalid"));
        }
        return key;
    }
    auto key = random_key();
    if (!key) {
        return failed(error, QStringLiteral("A vault key could not be generated"));
    }
    if (!write_private_file(path, *key)) {
        return failed(error, QStringLiteral("The vault key could not be saved"));
    }
    return key;
}
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

// No keychain at all (a headless account): the caller falls back to the file key.
bool keychain_absent(const OSStatus status) {
    return status == errSecNotAvailable || status == errSecNoSuchKeychain;
}

std::optional<QByteArray> keychain_load_or_create(const QString& name, QString* error, bool& absent) {
    absent = false;
    const CfHandle service{(QStandardPaths::isTestModeEnabled() ? QStringLiteral("Phone Memory Slider (tests)")
                                                                : QStringLiteral("Phone Memory Slider"))
                               .toCFString()};
    const CfHandle account{name.toCFString()};
    {
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
        const void* values[] = {kSecClassGenericPassword, service.get(), account.get(), kCFBooleanTrue, kSecMatchLimitOne};
        const CfHandle query{CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks)};
        CFTypeRef found = nullptr;
        const auto status = SecItemCopyMatching(static_cast<CFDictionaryRef>(query.get()), &found);
        const CfHandle owned{found};
        if (status == errSecSuccess) {
            auto key = QByteArray::fromCFData(static_cast<CFDataRef>(found));
            if (key.size() != key_bytes) {
                return failed(error, QStringLiteral("The vault key in the Keychain is invalid"));
            }
            return key;
        }
        if (keychain_absent(status)) {
            absent = true;
            return std::nullopt;
        }
        if (status != errSecItemNotFound) {  // e.g. a locked keychain: never replace a key we cannot read
            return failed(error, QStringLiteral("The Keychain could not be read (error %1)").arg(status));
        }
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
    const auto status = SecItemAdd(static_cast<CFDictionaryRef>(attributes.get()), nullptr);
    if (status == errSecSuccess) {
        return key;
    }
    if (keychain_absent(status)) {
        absent = true;
        return std::nullopt;
    }
    return failed(error, QStringLiteral("The Keychain could not store the vault key (error %1)").arg(status));
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
#elif defined(Q_OS_MACOS)
    // A file key exists only if an earlier run found no keychain; keep using it so the vault stays readable.
    if (QFile::exists(file_key_path(name))) {
        return file_load_or_create(name, error);
    }
    bool absent = false;
    auto key = keychain_load_or_create(name, error, absent);
    if (key || !absent) {
        return key;
    }
    return file_load_or_create(name, error);
#else
    return file_load_or_create(name, error);
#endif
}

}  // namespace pms::desktop
