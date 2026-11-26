#include "link/pin_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtGlobal>

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(PMS_HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace pms::desktop {

namespace {

constexpr qint64 max_blob = 4 * 1024 * 1024;  // far above any real pin list; a bigger file is not ours

[[noreturn]] void unreadable(const QString& why) {
    throw std::runtime_error(why.toStdString());
}

[[nodiscard]] std::optional<QByteArray> read_file(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        unreadable(QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
    }
    if (file.size() > max_blob) {
        unreadable(QStringLiteral("%1 is too large").arg(path));
    }
    return file.readAll();
}

// Replaces the file in one step (QSaveFile), so a crash mid-write leaves the old pins.
void write_file(const QString& path, const QByteArray& bytes, const bool ownerOnly) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        throw std::runtime_error(QStringLiteral("cannot create the folder of %1").arg(path).toStdString());
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        (ownerOnly && !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        throw std::runtime_error(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()).toStdString());
    }
}

class OwnerOnlyFileVault final : public SecretVault {
public:
    explicit OwnerOnlyFileVault(QString path) : path_(std::move(path)) {}
    [[nodiscard]] std::optional<QByteArray> read() override { return read_file(path_); }
    void write(const QByteArray& blob) override { write_file(path_, blob, true); }

private:
    QString path_;
};

#ifdef Q_OS_WIN
// Not a secret: it only keeps other programs' DPAPI calls for this user from opening the blob by accident.
constexpr char dpapi_entropy[] = "Phone Memory Slider pinned phones v1";

DATA_BLOB blob_of(const QByteArray& bytes) {
    return {static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()))};
}

class DpapiFileVault final : public SecretVault {
public:
    explicit DpapiFileVault(QString path) : path_(std::move(path)) {}

    [[nodiscard]] std::optional<QByteArray> read() override {
        const auto sealed = read_file(path_);
        if (!sealed) {
            return std::nullopt;
        }
        DATA_BLOB in = blob_of(*sealed);
        DATA_BLOB entropy = blob_of(QByteArray::fromRawData(dpapi_entropy, sizeof dpapi_entropy - 1));
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            unreadable(QStringLiteral("%1 does not decrypt for this Windows user (error %2)")
                           .arg(path_)
                           .arg(GetLastError()));
        }
        QByteArray blob(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return blob;
    }

    void write(const QByteArray& blob) override {
        DATA_BLOB in = blob_of(blob);
        DATA_BLOB entropy = blob_of(QByteArray::fromRawData(dpapi_entropy, sizeof dpapi_entropy - 1));
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"Phone Memory Slider pinned phones", &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            throw std::runtime_error("DPAPI refused to protect the pinned phones (error " +
                                     std::to_string(GetLastError()) + ")");
        }
        const QByteArray sealed(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
        LocalFree(out.pbData);
        write_file(path_, sealed, false);
    }

private:
    QString path_;
};
#elif defined(Q_OS_MACOS)
// One generic password item in the login Keychain.
class KeychainVault final : public SecretVault {
public:
    [[nodiscard]] std::optional<QByteArray> read() override {
        CFMutableDictionaryRef query = item_query();
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);
        if (status == errSecItemNotFound) {
            return std::nullopt;
        }
        if (status != errSecSuccess || result == nullptr) {
            unreadable(QStringLiteral("the Keychain refused to read the pinned phones (OSStatus %1)").arg(status));
        }
        const auto data = static_cast<CFDataRef>(result);
        QByteArray blob(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<qsizetype>(CFDataGetLength(data)));
        CFRelease(result);
        return blob;
    }

    void write(const QByteArray& blob) override {
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(blob.constData()), blob.size());
        CFMutableDictionaryRef query = item_query();
        CFMutableDictionaryRef update =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(update, kSecValueData, data);
        OSStatus status = SecItemUpdate(query, update);
        if (status == errSecItemNotFound) {
            CFDictionarySetValue(query, kSecValueData, data);
            status = SecItemAdd(query, nullptr);
        }
        CFRelease(update);
        CFRelease(query);
        CFRelease(data);
        if (status != errSecSuccess) {
            throw std::runtime_error("the Keychain refused to store the pinned phones (OSStatus " +
                                     std::to_string(status) + ")");
        }
    }

private:
    static CFMutableDictionaryRef item_query() {
        CFMutableDictionaryRef query =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, CFSTR("Phone Memory Slider"));
        CFDictionarySetValue(query, kSecAttrAccount, CFSTR("pinned phones"));
        return query;
    }
};
#elif defined(PMS_HAVE_LIBSECRET)
// One item in the Secret Service's default collection (GNOME Keyring, KWallet), stored as Base64 text.
const SecretSchema* pin_schema() {
    static const SecretSchema schema = [] {
        SecretSchema s{};
        s.name = "org.phonememoryslider.PinnedPhones";
        s.flags = SECRET_SCHEMA_NONE;
        s.attributes[0].name = "store";
        s.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
        return s;
    }();
    return &schema;
}

class LibsecretVault final : public SecretVault {
public:
    [[nodiscard]] std::optional<QByteArray> read() override {
        GError* error = nullptr;
        gchar* text = secret_password_lookup_sync(pin_schema(), nullptr, &error, "store", "pins",
                                                  static_cast<const char*>(nullptr));
        if (error != nullptr) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            unreadable(QStringLiteral("the secret service refused to read the pinned phones: %1").arg(why));
        }
        if (text == nullptr) {
            return std::nullopt;
        }
        const auto decoded = QByteArray::fromBase64Encoding(QByteArray(text), QByteArray::AbortOnBase64DecodingErrors);
        secret_password_free(text);
        if (!decoded) {
            unreadable(QStringLiteral("the secret service item is not Base64"));
        }
        return *decoded;
    }

    void write(const QByteArray& blob) override {
        GError* error = nullptr;
        const QByteArray text = blob.toBase64();
        secret_password_store_sync(pin_schema(), SECRET_COLLECTION_DEFAULT, "Phone Memory Slider pinned phones",
                                   text.constData(), nullptr, &error, "store", "pins",
                                   static_cast<const char*>(nullptr));
        if (error != nullptr) {
            const std::string why = error->message;
            g_error_free(error);
            throw std::runtime_error("the secret service refused to store the pinned phones: " + why);
        }
    }
};
#endif

[[nodiscard]] QString hex(const link::Key32& key) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(key.data()), static_cast<qsizetype>(key.size())).toHex());
}

// {"version":1,"pins":[{"key":"<64 hex>","model":"Pixel 9","paired":1700000000}, ...]}
[[nodiscard]] QByteArray serialize(const std::map<link::Key32, link::Pinned>& pins) {
    QJsonArray list;
    for (const auto& [key, p] : pins) {
        list.append(QJsonObject{{QStringLiteral("key"), hex(key)},
                                {QStringLiteral("model"), QString::fromStdString(p.model)},
                                {QStringLiteral("paired"), static_cast<qint64>(p.paired_unix_s)}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("pins"), list}})
        .toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::map<link::Key32, link::Pinned> parse(const QByteArray& blob) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(blob, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        unreadable(QStringLiteral("not a JSON object"));
    }
    const auto root = doc.object();
    const auto version = root.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0 || !root.value(QStringLiteral("pins")).isArray()) {
        unreadable(QStringLiteral("unknown version or no pin list"));
    }
    std::map<link::Key32, link::Pinned> pins;
    for (const auto& entry : root.value(QStringLiteral("pins")).toArray()) {
        const auto o = entry.toObject();
        const auto key_hex = o.value(QStringLiteral("key")).toString().toLatin1();
        const auto key = QByteArray::fromHex(key_hex);
        const auto model = o.value(QStringLiteral("model"));
        const auto paired = o.value(QStringLiteral("paired"));
        if (key_hex.size() != 64 || key.size() != 32 || key.toHex() != key_hex.toLower() || !model.isString() ||
            !paired.isDouble() || static_cast<double>(paired.toInteger()) != paired.toDouble()) {
            unreadable(QStringLiteral("a malformed pin"));
        }
        link::Pinned p;
        std::memcpy(p.phone_key.data(), key.constData(), p.phone_key.size());
        p.model = model.toString().toStdString();
        p.paired_unix_s = paired.toInteger();
        pins[p.phone_key] = std::move(p);
    }
    return pins;
}

}  // namespace

std::unique_ptr<SecretVault> owner_only_file_vault(const QString& path) {
    return std::make_unique<OwnerOnlyFileVault>(path);
}

#ifdef Q_OS_WIN
std::unique_ptr<SecretVault> dpapi_file_vault(const QString& path) {
    return std::make_unique<DpapiFileVault>(path);
}
#endif

std::unique_ptr<SecretVault> platform_vault(const QString& directory) {
#ifdef Q_OS_WIN
    return dpapi_file_vault(QDir(directory).filePath(QStringLiteral("pinned-phones.dpapi")));
#elif defined(Q_OS_MACOS)
    Q_UNUSED(directory);
    return std::make_unique<KeychainVault>();
#else
    const auto fallback = QDir(directory).filePath(QStringLiteral("pinned-phones.json"));
#ifdef PMS_HAVE_LIBSECRET
    auto vault = std::make_unique<LibsecretVault>();
    try {
        (void)vault->read();  // is a secret service there at all?
        return vault;
    } catch (const std::exception& e) {
        qWarning("no secret service (%s); pinned phones are kept in %s, readable by this user only", e.what(),
                 qPrintable(fallback));
    }
#else
    qWarning("built without libsecret; pinned phones are kept in %s, readable by this user only", qPrintable(fallback));
#endif
    return owner_only_file_vault(fallback);
#endif
}

VaultPinStore::VaultPinStore(std::unique_ptr<SecretVault> vault) : vault_(std::move(vault)) {
    try {
        if (const auto blob = vault_->read()) {
            pins_ = parse(*blob);
        }
    } catch (const std::exception& e) {
        qWarning("pinned phones unreadable, starting empty: %s", e.what());
        pins_.clear();
    }
}

std::optional<link::Pinned> VaultPinStore::find(const link::Key32& phone_key) const {
    const auto it = pins_.find(phone_key);
    return it == pins_.end() ? std::nullopt : std::optional{it->second};
}

void VaultPinStore::pin(const link::Pinned& p) {
    auto next = pins_;
    next[p.phone_key] = p;  // pairing the same phone again replaces its pin
    save(next);
    pins_ = std::move(next);
}

void VaultPinStore::unpin(const link::Key32& phone_key) {
    auto next = pins_;
    if (next.erase(phone_key) == 0) {
        return;
    }
    save(next);
    pins_ = std::move(next);
}

void VaultPinStore::save(const std::map<link::Key32, link::Pinned>& pins) {
    vault_->write(serialize(pins));
}

}  // namespace pms::desktop
