#include "device_connector.hpp"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <PortableDeviceTypes.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace pms::desktop {
namespace {

constexpr auto folder_prefix = "folder:";

[[nodiscard]] QString folder_path(const QString& device_id) {
    return QDir::fromNativeSeparators(device_id.mid(static_cast<qsizetype>(std::char_traits<char>::length(folder_prefix))));
}

[[nodiscard]] QString contained_folder_asset_path(const QString& device_id, const QString& asset_id) {
    if (asset_id.isEmpty() || QDir::isAbsolutePath(asset_id)) {
        return {};
    }
    const auto root = QFileInfo{folder_path(device_id)}.canonicalFilePath();
    const auto candidate = QFileInfo{QDir{root}.filePath(asset_id)}.canonicalFilePath();
    if (root.isEmpty() || candidate.isEmpty()) {
        return {};
    }
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    auto prefix = QDir::fromNativeSeparators(QDir::cleanPath(root));
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
    return QDir::fromNativeSeparators(candidate).startsWith(QDir::fromNativeSeparators(prefix), sensitivity)
               ? candidate
               : QString{};
}

[[nodiscard]] bool favorites_folder(const QString& relative_path) {
    const auto folders = QDir::fromNativeSeparators(QFileInfo{relative_path}.path()).split(QLatin1Char('/'));
    return std::ranges::any_of(folders, [](const QString& folder) {
        return folder.compare(QStringLiteral("favorites"), Qt::CaseInsensitive) == 0 ||
               folder.compare(QStringLiteral("favourites"), Qt::CaseInsensitive) == 0 ||
               folder.compare(QStringLiteral("favorite"), Qt::CaseInsensitive) == 0 ||
               folder.compare(QStringLiteral("favourite"), Qt::CaseInsensitive) == 0;
    });
}

[[nodiscard]] bool supported_media(const QString& mime) {
    return mime.startsWith(QStringLiteral("image/")) || mime.startsWith(QStringLiteral("video/"));
}

[[nodiscard]] QByteArray read_file_limited(const QString& path, const qsizetype limit) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return limit > 0 ? file.read(limit) : file.readAll();
}

class HashSink final : public QIODevice {
public:
    HashSink() { open(QIODevice::WriteOnly); }
    [[nodiscard]] QString digest() { return QString::fromLatin1(hash_.result().toHex()); }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, const qint64 length) override {
        hash_.addData(QByteArrayView{data, length});
        return length;
    }

private:
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};

[[nodiscard]] std::vector<DeviceMedia> folder_catalog(const QString& root) {
    std::vector<DeviceMedia> result;
    const QDir root_directory{root};
    QMimeDatabase mime_database;
    QDirIterator iterator{root, QDir::Files | QDir::Readable | QDir::NoSymLinks, QDirIterator::Subdirectories};
    while (iterator.hasNext()) {
        const auto path = iterator.next();
        const QFileInfo info{path};
        const auto mime = mime_database.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
        if (!supported_media(mime)) {
            continue;
        }
        const auto relative_path = root_directory.relativeFilePath(path);
        result.push_back({
            relative_path,
            info.fileName(),
            mime,
            static_cast<std::uint64_t>(std::max<qint64>(0, info.size())),
            static_cast<std::uint64_t>(std::max<qint64>(0, info.lastModified().toMSecsSinceEpoch())),
            0,
            0,
            0,
            favorites_folder(relative_path),
        });
    }
    std::ranges::sort(result, {}, &DeviceMedia::assetId);
    return result;
}

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_;
};

[[nodiscard]] QString hresult_text(const HRESULT result) {
    wchar_t* message{};
    const auto count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    const auto text = count == 0 ? QStringLiteral("Windows error 0x%1").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
                                 : QString::fromWCharArray(message, static_cast<qsizetype>(count)).trimmed();
    if (message != nullptr) {
        LocalFree(message);
    }
    return text;
}

[[nodiscard]] bool create_device(
    const QString& device_id,
    ComPtr<IPortableDevice>& device,
    ComPtr<IPortableDeviceContent>& content,
    QString& error) {
    ComPtr<IPortableDeviceValues> client;
    auto result = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client));
    if (FAILED(result)) {
        error = hresult_text(result);
        return false;
    }
    client->SetStringValue(WPD_CLIENT_NAME, L"Phone Memory Slider");
    client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
    client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
    client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);

    result = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_PortableDevice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device));
    }
    if (FAILED(result)) {
        error = hresult_text(result);
        return false;
    }
    result = device->Open(reinterpret_cast<LPCWSTR>(device_id.utf16()), client.Get());
    if (FAILED(result)) {
        error = result == E_ACCESSDENIED
                    ? QStringLiteral("Unlock the phone, approve this computer, then reconnect the cable")
                    : hresult_text(result);
        return false;
    }
    result = device->Content(&content);
    if (FAILED(result)) {
        error = hresult_text(result);
        return false;
    }
    return true;
}

[[nodiscard]] QString value_string(IPortableDeviceValues* values, const PROPERTYKEY& key) {
    PWSTR value{};
    if (FAILED(values->GetStringValue(key, &value)) || value == nullptr) {
        return {};
    }
    const auto result = QString::fromWCharArray(value);
    CoTaskMemFree(value);
    return result;
}

[[nodiscard]] QString mime_for_name(const QString& name, const GUID& type) {
    if (type == WPD_CONTENT_TYPE_IMAGE) {
        return QMimeDatabase{}.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
    }
    if (type == WPD_CONTENT_TYPE_VIDEO) {
        const auto detected = QMimeDatabase{}.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
        return detected.startsWith(QStringLiteral("video/")) ? detected : QStringLiteral("video/mp4");
    }
    return {};
}

[[nodiscard]] bool enumerate_children(
    IPortableDeviceContent* content,
    IPortableDeviceProperties* properties,
    IPortableDeviceKeyCollection* keys,
    const QString& parent_id,
    std::vector<DeviceMedia>& output,
    QString& error,
    const bool favorite_context = false,
    const int depth = 0) {
    constexpr int maximum_depth = 256;
    constexpr std::size_t maximum_items = 1'000'000;
    if (depth > maximum_depth || output.size() >= maximum_items) {
        error = QStringLiteral("The phone gallery exceeds the supported traversal limits");
        return false;
    }
    ComPtr<IEnumPortableDeviceObjectIDs> objects;
    auto result = content->EnumObjects(
        0,
        reinterpret_cast<LPCWSTR>(parent_id.utf16()),
        nullptr,
        &objects);
    if (FAILED(result)) {
        error = hresult_text(result);
        return false;
    }

    for (;;) {
        PWSTR object_id{};
        DWORD fetched{};
        result = objects->Next(1, &object_id, &fetched);
        if (result == S_FALSE || fetched == 0) {
            return true;
        }
        if (FAILED(result)) {
            error = hresult_text(result);
            return false;
        }
        const auto id = QString::fromWCharArray(object_id);
        CoTaskMemFree(object_id);

        ComPtr<IPortableDeviceValues> values;
        if (FAILED(properties->GetValues(reinterpret_cast<LPCWSTR>(id.utf16()), keys, &values))) {
            continue;
        }
        GUID content_type{};
        if (FAILED(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &content_type))) {
            continue;
        }
        if (content_type == WPD_CONTENT_TYPE_FOLDER || content_type == WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT) {
            const auto folder_name = value_string(values.Get(), WPD_OBJECT_NAME);
            const auto in_favorites = favorite_context || favorites_folder(folder_name + QStringLiteral("/item"));
            if (!enumerate_children(content, properties, keys, id, output, error, in_favorites, depth + 1)) {
                return false;
            }
            continue;
        }
        if (content_type != WPD_CONTENT_TYPE_IMAGE && content_type != WPD_CONTENT_TYPE_VIDEO) {
            continue;
        }
        if (output.size() >= maximum_items) {
            error = QStringLiteral("The phone gallery exceeds the supported item limit");
            return false;
        }
        auto name = value_string(values.Get(), WPD_OBJECT_ORIGINAL_FILE_NAME);
        if (name.isEmpty()) {
            name = value_string(values.Get(), WPD_OBJECT_NAME);
        }
        ULONGLONG bytes{};
        values->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &bytes);
        DeviceMedia media{id, name.isEmpty() ? QStringLiteral("Phone media") : name, mime_for_name(name, content_type), bytes};
        media.favorite = favorite_context;
        output.push_back(std::move(media));
    }
}

[[nodiscard]] bool read_wpd_resource(
    const QString& device_id,
    const QString& asset_id,
    const PROPERTYKEY& resource_key,
    QByteArray* bytes,
    QIODevice* destination,
    const qsizetype limit,
    QString& error) {
    ComPtr<IPortableDevice> device;
    ComPtr<IPortableDeviceContent> content;
    if (!create_device(device_id, device, content, error)) {
        return false;
    }
    ComPtr<IPortableDeviceResources> resources;
    auto result = content->Transfer(&resources);
    if (FAILED(result)) {
        error = hresult_text(result);
        return false;
    }
    DWORD optimal{};
    ComPtr<IStream> stream;
    result = resources->GetStream(
        reinterpret_cast<LPCWSTR>(asset_id.utf16()), resource_key, STGM_READ, &optimal, &stream);
    if (FAILED(result)) {
        return false;
    }

    std::vector<char> chunk(std::clamp<DWORD>(optimal, 64U * 1024U, 4U * 1024U * 1024U));
    qsizetype total{};
    for (;;) {
        ULONG read{};
        result = stream->Read(chunk.data(), static_cast<ULONG>(chunk.size()), &read);
        if (FAILED(result)) {
            error = hresult_text(result);
            return false;
        }
        if (read == 0) {
            return true;
        }
        if (destination != nullptr && limit > 0 && static_cast<qsizetype>(read) > limit - total) {
            error = QStringLiteral("The media exceeds the local preview limit");
            return false;
        }
        const auto allowed = limit > 0 ? std::min<qsizetype>(read, limit - total) : static_cast<qsizetype>(read);
        if (allowed <= 0) {
            return true;
        }
        if (bytes != nullptr) {
            bytes->append(chunk.data(), allowed);
        }
        if (destination != nullptr && destination->write(chunk.data(), allowed) != allowed) {
            error = destination->errorString();
            return false;
        }
        total += allowed;
        if (limit > 0 && total >= limit && destination == nullptr) {
            return true;
        }
    }
}
#endif

}  // namespace

std::vector<DeviceSummary> DeviceConnector::devices() {
    error_.clear();
    std::vector<DeviceSummary> result;
#ifdef Q_OS_WIN
    ComApartment apartment;
    if (!apartment.ready()) {
        setError(QStringLiteral("Windows Portable Devices could not be initialized"));
        return result;
    }
    ComPtr<IPortableDeviceManager> manager;
    const auto created = CoCreateInstance(
        CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(created)) {
        setError(hresult_text(created));
        return result;
    }
    DWORD count{};
    if (FAILED(manager->GetDevices(nullptr, &count)) || count == 0) {
        return result;
    }
    constexpr DWORD identifier_capacity = 1024;
    std::vector<std::array<wchar_t, identifier_capacity>> storage(count);
    std::vector<PWSTR> identifiers(count);
    for (DWORD index = 0; index < count; ++index) {
        identifiers[index] = storage[index].data();
    }
    if (FAILED(manager->GetDevices(identifiers.data(), &count))) {
        setError(QStringLiteral("Windows could not enumerate the connected phones"));
        return result;
    }
    for (DWORD index = 0; index < count; ++index) {
        DWORD name_length{};
        manager->GetDeviceFriendlyName(identifiers[index], nullptr, &name_length);
        std::vector<wchar_t> name(std::max<DWORD>(name_length, 2));
        if (FAILED(manager->GetDeviceFriendlyName(identifiers[index], name.data(), &name_length))) {
            name.assign({L'P', L'h', L'o', L'n', L'e', L'\0'});
        }
        result.push_back({
            QString::fromWCharArray(identifiers[index]),
            QString::fromWCharArray(name.data()),
            QStringLiteral("USB media via Windows Portable Devices"),
            true,
        });
    }
#endif
    return result;
}

std::vector<DeviceMedia> DeviceConnector::catalog(const QString& device_id) {
    error_.clear();
    if (isFolderDevice(device_id)) {
        const auto path = folder_path(device_id);
        if (!QFileInfo{path}.isDir()) {
            setError(QStringLiteral("The selected gallery folder is no longer available"));
            return {};
        }
        return folder_catalog(path);
    }
#ifdef Q_OS_WIN
    ComApartment apartment;
    ComPtr<IPortableDevice> device;
    ComPtr<IPortableDeviceContent> content;
    if (!apartment.ready() || !create_device(device_id, device, content, error_)) {
        return {};
    }
    ComPtr<IPortableDeviceProperties> properties;
    ComPtr<IPortableDeviceKeyCollection> keys;
    if (FAILED(content->Properties(&properties)) ||
        FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&keys)))) {
        setError(QStringLiteral("The phone media catalog could not be opened"));
        return {};
    }
    for (const auto& key : {WPD_OBJECT_CONTENT_TYPE, WPD_OBJECT_ORIGINAL_FILE_NAME, WPD_OBJECT_NAME, WPD_OBJECT_SIZE}) {
        keys->Add(key);
    }
    std::vector<DeviceMedia> result;
    if (!enumerate_children(content.Get(), properties.Get(), keys.Get(), QString::fromWCharArray(WPD_DEVICE_OBJECT_ID), result, error_)) {
        return {};
    }
    std::ranges::sort(result, {}, &DeviceMedia::assetId);
    return result;
#else
    setError(QStringLiteral("Use a mounted DCIM folder on this platform"));
    return {};
#endif
}

QByteArray DeviceConnector::preview(const QString& device_id, const QString& asset_id, const qsizetype byte_limit) {
    error_.clear();
    if (isFolderDevice(device_id)) {
        const auto path = contained_folder_asset_path(device_id, asset_id);
        if (path.isEmpty()) {
            setError(QStringLiteral("The media is no longer inside the selected folder"));
            return {};
        }
        return read_file_limited(path, byte_limit);
    }
#ifdef Q_OS_WIN
    ComApartment apartment;
    QByteArray bytes;
    if (apartment.ready() && read_wpd_resource(device_id, asset_id, WPD_RESOURCE_THUMBNAIL, &bytes, nullptr, byte_limit, error_)) {
        return bytes;
    }
    bytes.clear();
    error_.clear();
    if (apartment.ready() && read_wpd_resource(device_id, asset_id, WPD_RESOURCE_DEFAULT, &bytes, nullptr, byte_limit, error_)) {
        return bytes;
    }
#endif
    if (error_.isEmpty()) {
        setError(QStringLiteral("The phone did not provide a readable preview for this item"));
    }
    return {};
}

QString DeviceConnector::sha256(const QString& device_id, const QString& asset_id) {
    error_.clear();
    HashSink sink;
    if (isFolderDevice(device_id)) {
        const auto path = contained_folder_asset_path(device_id, asset_id);
        if (path.isEmpty()) {
            setError(QStringLiteral("The media is no longer inside the selected folder"));
            return {};
        }
        QFile input{path};
        if (!input.open(QIODevice::ReadOnly)) {
            setError(input.errorString());
            return {};
        }
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        while (!input.atEnd()) {
            const auto read = input.read(buffer.data(), buffer.size());
            if (read < 0 || (read > 0 && sink.write(buffer.data(), read) != read)) {
                setError(input.errorString());
                return {};
            }
        }
        return sink.digest();
    }
#ifdef Q_OS_WIN
    ComApartment apartment;
    if (apartment.ready() &&
        read_wpd_resource(device_id, asset_id, WPD_RESOURCE_DEFAULT, nullptr, &sink, 0, error_)) {
        return sink.digest();
    }
#endif
    if (error_.isEmpty()) {
        setError(QStringLiteral("The phone item could not be checksummed"));
    }
    return {};
}

bool DeviceConnector::copyTo(
    const QString& device_id,
    const QString& asset_id,
    const QString& destination,
    const std::uint64_t byte_limit) {
    error_.clear();
    QSaveFile output{destination};
    if (!output.open(QIODevice::WriteOnly)) {
        setError(output.errorString());
        return false;
    }
    if (isFolderDevice(device_id)) {
        const auto path = contained_folder_asset_path(device_id, asset_id);
        if (path.isEmpty()) {
            setError(QStringLiteral("The media is no longer inside the selected folder"));
            return false;
        }
        QFile input{path};
        if (!input.open(QIODevice::ReadOnly)) {
            setError(input.errorString());
            return false;
        }
        if (byte_limit > 0 && static_cast<std::uint64_t>(input.size()) > byte_limit) {
            setError(QStringLiteral("The media exceeds the local preview limit"));
            return false;
        }
        std::uint64_t total{};
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        while (!input.atEnd()) {
            const auto read = input.read(buffer.data(), buffer.size());
            if (read <= 0) {
                setError(input.errorString());
                return false;
            }
            if (byte_limit > 0 && static_cast<std::uint64_t>(read) > byte_limit - total) {
                setError(QStringLiteral("The media exceeds the local preview limit"));
                return false;
            }
            if (output.write(buffer.data(), read) != read) {
                setError(output.errorString());
                return false;
            }
            total += static_cast<std::uint64_t>(read);
        }
    } else {
#ifdef Q_OS_WIN
        ComApartment apartment;
        if (!apartment.ready() || !read_wpd_resource(
                                      device_id,
                                      asset_id,
                                      WPD_RESOURCE_DEFAULT,
                                      nullptr,
                                      &output,
                                      static_cast<qsizetype>(byte_limit),
                                      error_)) {
            return false;
        }
#else
        setError(QStringLiteral("This device source cannot be copied on this platform"));
        return false;
#endif
    }
    if (!output.commit()) {
        setError(output.errorString());
        return false;
    }
    return true;
}

bool DeviceConnector::remove(const QString& device_id, const QString& asset_id) {
    error_.clear();
    if (isFolderDevice(device_id)) {
        const auto path = contained_folder_asset_path(device_id, asset_id);
        if (path.isEmpty()) {
            setError(QStringLiteral("The media is no longer inside the selected folder"));
            return false;
        }
        QFile file{path};
        if (file.remove()) {
            return true;
        }
        setError(file.errorString());
        return false;
    }
#ifdef Q_OS_WIN
    ComApartment apartment;
    ComPtr<IPortableDevice> device;
    ComPtr<IPortableDeviceContent> content;
    if (!apartment.ready() || !create_device(device_id, device, content, error_)) {
        return false;
    }
    ComPtr<IPortableDevicePropVariantCollection> objects;
    auto result = CoCreateInstance(
        CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&objects));
    if (FAILED(result)) {
        setError(hresult_text(result));
        return false;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(asset_id.utf16()));
    result = objects->Add(&value);
    if (SUCCEEDED(result)) {
        result = content->Delete(PORTABLE_DEVICE_DELETE_NO_RECURSION, objects.Get(), nullptr);
    }
    if (FAILED(result)) {
        setError(hresult_text(result));
        return false;
    }
    return true;
#else
    setError(QStringLiteral("This device source cannot delete media on this platform"));
    return false;
#endif
}

QString DeviceConnector::errorString() const {
    return error_;
}

QString DeviceConnector::folderDeviceId(const QUrl& folder) {
    if (!folder.isLocalFile()) {
        return {};
    }
    const QFileInfo info{folder.toLocalFile()};
    const auto path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    return QString::fromLatin1(folder_prefix) + QDir::fromNativeSeparators(path);
}

bool DeviceConnector::isFolderDevice(const QString& device_id) {
    return device_id.startsWith(QString::fromLatin1(folder_prefix), Qt::CaseSensitive);
}

void DeviceConnector::setError(QString error) {
    error_ = std::move(error);
}

}  // namespace pms::desktop
