#include "phone_client.hpp"

#include <QBluetoothHostInfo>
#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QStorageInfo>
#include <QSysInfo>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

#include "qrcodegen.hpp"

namespace pms::desktop {
namespace {

constexpr auto max_json_bytes = 2 * 1024 * 1024;
constexpr std::uint64_t max_content_chunk_bytes = 1024ULL * 1024ULL;
constexpr auto service_uuid_text = "8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf";
constexpr auto handshake_prefix = "PMS/1 AUTH ";

[[nodiscard]] bool json_uint64(const QJsonObject& object, const char* key, std::uint64_t& result) {
    const auto value = object.value(QLatin1StringView{key});
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<qint64>::max()) ||
        number != std::floor(number)) {
        return false;
    }
    result = static_cast<std::uint64_t>(number);
    return true;
}

[[nodiscard]] bool bounded_string(const QJsonObject& object, const char* key, QString& result, const qsizetype limit) {
    const auto value = object.value(QLatin1StringView{key});
    if (!value.isString()) {
        return false;
    }
    result = value.toString();
    return !result.isEmpty() && result.size() <= limit;
}

[[nodiscard]] QString target_with_query(const QString& path, const QUrlQuery& query) {
    const auto encoded = query.toString(QUrl::FullyEncoded);
    return encoded.isEmpty() ? path : path + QLatin1Char('?') + encoded;
}

[[nodiscard]] QByteArray random_secret() {
    QByteArray bytes(32, Qt::Uninitialized);
    auto* generator = QRandomGenerator::system();
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(generator->generate() & 0xffU);
    }
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

}  // namespace

PhoneClient::PhoneClient(QObject* parent) : QObject(parent) {
    qRegisterMetaType<RemoteCatalogPage>();
    qRegisterMetaType<RemoteCapabilities>();
    qRegisterMetaType<RemoteTrashPrepared>();
    qRegisterMetaType<RemoteTrashResult>();
}

QString PhoneClient::qrPayload() const { return qr_payload_; }
QVariantList PhoneClient::qrModules() const { return qr_modules_; }
int PhoneClient::qrSize() const noexcept { return qr_size_; }
QString PhoneClient::bluetoothAddress() const { return bluetooth_address_; }
bool PhoneClient::listening() const noexcept { return server_ && server_->isListening(); }

QString PhoneClient::makePairingPayload(
    const QString& address,
    const QString& secret,
    const QString& computer_name) {
    QUrl url{QStringLiteral("pms://pair")};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("address"), address);
    query.addQueryItem(QStringLiteral("uuid"), QString::fromLatin1(service_uuid_text));
    query.addQueryItem(QStringLiteral("secret"), secret);
    query.addQueryItem(QStringLiteral("name"), computer_name.left(80));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

bool PhoneClient::validPairingHandshake(const QByteArray& line, const QString& secret) noexcept {
    const auto expected = QByteArray{handshake_prefix} + secret.toLatin1();
    if (line.size() != expected.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype index = 0; index < line.size(); ++index) {
        difference |= static_cast<unsigned char>(line[index]) ^ static_cast<unsigned char>(expected[index]);
    }
    return difference == 0;
}

std::expected<RemoteCatalogPage, QString> PhoneClient::parseCatalogPage(const QByteArray& body) {
    if (body.size() > max_json_bytes) {
        return std::unexpected(QStringLiteral("Catalog response is too large"));
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("Phone returned invalid catalog JSON"));
    }
    const auto root = document.object();
    RemoteCatalogPage page;
    if (!json_uint64(root, "revision", page.revision) || !root.value(QStringLiteral("assets")).isArray() ||
        !root.value(QStringLiteral("complete")).isBool()) {
        return std::unexpected(QStringLiteral("Phone returned an invalid catalog page"));
    }
    page.complete = root.value(QStringLiteral("complete")).toBool();
    const auto cursor = root.value(QStringLiteral("next_cursor"));
    if (!cursor.isUndefined() && !cursor.isNull() && (!cursor.isString() || cursor.toString().size() > 512)) {
        return std::unexpected(QStringLiteral("Phone returned an invalid catalog cursor"));
    }
    page.nextCursor = cursor.toString();
    const auto assets = root.value(QStringLiteral("assets")).toArray();
    if (assets.size() > 1000) {
        return std::unexpected(QStringLiteral("Phone returned too many catalog items"));
    }
    page.assets.reserve(static_cast<std::size_t>(assets.size()));
    for (const auto& value : assets) {
        if (!value.isObject()) {
            return std::unexpected(QStringLiteral("Phone returned an invalid catalog item"));
        }
        const auto object = value.toObject();
        RemoteCatalogAsset asset;
        std::uint64_t width{};
        std::uint64_t height{};
        if (!bounded_string(object, "asset_id", asset.assetId, 512) ||
            !bounded_string(object, "mime_type", asset.mimeType, 128) || !json_uint64(object, "bytes", asset.bytes) ||
            !json_uint64(object, "modified_epoch_ms", asset.modifiedEpochMs) || !json_uint64(object, "width", width) ||
            !json_uint64(object, "height", height) || !json_uint64(object, "duration_ms", asset.durationMs) ||
            width > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
            height > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
            !object.value(QStringLiteral("favorite")).isBool()) {
            return std::unexpected(QStringLiteral("Phone returned an invalid catalog item"));
        }
        asset.width = static_cast<int>(width);
        asset.height = static_cast<int>(height);
        asset.favorite = object.value(QStringLiteral("favorite")).toBool();
        page.assets.push_back(std::move(asset));
    }
    if (!page.complete && page.nextCursor.isEmpty()) {
        return std::unexpected(QStringLiteral("Phone omitted the next catalog cursor"));
    }
    return page;
}

void PhoneClient::startPairing() {
    disconnectPhone();
    const auto adapters = QBluetoothLocalDevice::allDevices();
    if (adapters.isEmpty() || adapters.front().address().isNull()) {
        fail(QStringLiteral("No Bluetooth adapter is available on this computer"));
        return;
    }
    bluetooth_address_ = adapters.front().address().toString();
    server_ = std::make_unique<QBluetoothServer>(QBluetoothServiceInfo::RfcommProtocol, this);
    connect(server_.get(), &QBluetoothServer::newConnection, this, &PhoneClient::acceptConnection);
    connect(server_.get(), &QBluetoothServer::errorOccurred, this, [this](const QBluetoothServer::Error error) {
        if (error != QBluetoothServer::NoError) {
            fail(QStringLiteral("The local Bluetooth service could not start"));
        }
    });
    service_info_ = server_->listen(
        QBluetoothUuid{QUuid{QString::fromLatin1(service_uuid_text)}},
        QStringLiteral("Phone Memory Slider"));
    if (!service_info_.isValid()) {
        server_.reset();
        fail(QStringLiteral("The local Bluetooth service could not be advertised"));
        return;
    }

    secret_ = QString::fromLatin1(random_secret());
    auto computer_name = QSysInfo::machineHostName().trimmed();
    if (computer_name.isEmpty()) {
        computer_name = QCoreApplication::applicationName();
    }
    qr_payload_ = makePairingPayload(bluetooth_address_, secret_, computer_name);
    try {
        const auto code = qrcodegen::QrCode::encodeText(qr_payload_.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        qr_size_ = code.getSize();
        qr_modules_.clear();
        qr_modules_.reserve(qr_size_ * qr_size_);
        for (int y = 0; y < qr_size_; ++y) {
            for (int x = 0; x < qr_size_; ++x) {
                qr_modules_.push_back(code.getModule(x, y));
            }
        }
    } catch (const std::exception&) {
        disconnectPhone();
        fail(QStringLiteral("The local pairing code could not be generated"));
        return;
    }
    emit pairingReady();
}

void PhoneClient::acceptConnection() {
    if (!server_) {
        return;
    }
    auto* incoming = server_->nextPendingConnection();
    if (!incoming) {
        return;
    }
    if (socket_) {
        incoming->disconnectFromService();
        incoming->deleteLater();
        return;
    }
    socket_ = incoming;
    authenticated_ = false;
    handshake_buffer_.clear();
    connect(socket_, &QBluetoothSocket::readyRead, this, &PhoneClient::readSocket);
    connect(socket_, &QBluetoothSocket::disconnected, this, [this, incoming] {
        if (socket_ == incoming) {
            socket_->deleteLater();
            socket_ = nullptr;
            authenticated_ = false;
            handshake_buffer_.clear();
        }
        requests_.clear();
        active_request_.reset();
        resetResponseState();
    });
    connect(socket_, &QBluetoothSocket::errorOccurred, this, [this](QBluetoothSocket::SocketError) {
        fail(QStringLiteral("The private Bluetooth session was interrupted"));
    });
}

void PhoneClient::readSocket() {
    if (!socket_) {
        return;
    }
    if (authenticated_) {
        readResponse();
        return;
    }
    handshake_buffer_ += socket_->readAll();
    if (handshake_buffer_.size() > 256) {
        fail(QStringLiteral("A nearby device sent an invalid pairing proof"));
        socket_->disconnectFromService();
        return;
    }
    const auto line_end = handshake_buffer_.indexOf("\r\n");
    if (line_end < 0) {
        return;
    }
    if (line_end + 2 != handshake_buffer_.size() ||
        !validPairingHandshake(handshake_buffer_.left(line_end), secret_)) {
        fail(QStringLiteral("A nearby device could not prove it scanned this pairing code"));
        socket_->disconnectFromService();
        return;
    }
    authenticated_ = true;
    handshake_buffer_.clear();
    if (service_info_.isValid()) {
        service_info_.unregisterService();
        service_info_ = {};
    }
    if (server_) {
        server_->close();
        server_.reset();
    }
    emit paired();
}

void PhoneClient::enqueue(
    const QByteArray& method,
    const QString& target,
    const QByteArray& body,
    const QHash<QByteArray, QByteArray>& headers,
    const qsizetype max_body_bytes,
    std::function<void(int, const QHash<QByteArray, QByteArray>&, const QByteArray&)> handler) {
    if (!socket_ || !authenticated_ || socket_->state() != QBluetoothSocket::SocketState::ConnectedState ||
        secret_.isEmpty() ||
        target.isEmpty() || target.size() > 2048 || max_body_bytes < 0) {
        fail(QStringLiteral("No authenticated Bluetooth session is available"));
        return;
    }
    QByteArray request = method + ' ' + target.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: local\r\nAuthorization: Bearer " + secret_.toLatin1() + "\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n";
    for (auto iterator = headers.cbegin(); iterator != headers.cend(); ++iterator) {
        if (iterator.key().contains('\r') || iterator.key().contains('\n') || iterator.value().contains('\r') ||
            iterator.value().contains('\n')) {
            fail(QStringLiteral("A local Bluetooth request was invalid"));
            return;
        }
        request += iterator.key() + ": " + iterator.value() + "\r\n";
    }
    request += "\r\n";
    request += body;
    requests_.push_back({std::move(request), max_body_bytes, std::move(handler)});
    sendNext();
}

void PhoneClient::sendNext() {
    if (active_request_ || requests_.empty() || !socket_) {
        return;
    }
    active_request_ = std::move(requests_.front());
    requests_.pop_front();
    resetResponseState();
    const auto written = socket_->write(active_request_->bytes);
    if (written != active_request_->bytes.size()) {
        active_request_.reset();
        fail(QStringLiteral("The private Bluetooth request could not be sent"));
    }
}

void PhoneClient::readResponse() {
    if (!socket_ || !active_request_) {
        fail(QStringLiteral("The phone sent an unexpected Bluetooth response"));
        disconnectPhone();
        return;
    }
    response_buffer_ += socket_->readAll();
    if (response_buffer_.size() > active_request_->maxBodyBytes + 32 * 1024) {
        fail(QStringLiteral("The phone response exceeded its safe local limit"));
        disconnectPhone();
        return;
    }
    if (response_body_size_ < 0) {
        const auto header_end = response_buffer_.indexOf("\r\n\r\n");
        if (header_end < 0) {
            return;
        }
        const auto header_lines = response_buffer_.left(header_end).split('\n');
        if (header_lines.isEmpty()) {
            fail(QStringLiteral("The phone returned malformed response headers"));
            disconnectPhone();
            return;
        }
        const auto status_parts = header_lines.front().trimmed().split(' ');
        bool status_ok = false;
        response_status_ = status_parts.size() >= 2 ? status_parts[1].toInt(&status_ok) : 0;
        if (!status_ok || response_status_ < 100 || response_status_ > 599) {
            fail(QStringLiteral("The phone returned an invalid response status"));
            disconnectPhone();
            return;
        }
        response_headers_.clear();
        for (qsizetype index = 1; index < header_lines.size(); ++index) {
            const auto line = header_lines[index].trimmed();
            const auto separator = line.indexOf(':');
            if (separator <= 0) {
                fail(QStringLiteral("The phone returned malformed response headers"));
                disconnectPhone();
                return;
            }
            const auto name = line.left(separator).toLower();
            if (response_headers_.contains(name)) {
                fail(QStringLiteral("The phone returned ambiguous response headers"));
                disconnectPhone();
                return;
            }
            response_headers_.insert(name, line.mid(separator + 1).trimmed());
        }
        bool length_ok = false;
        response_body_size_ = response_headers_.value("content-length").toLongLong(&length_ok);
        if (!length_ok || response_body_size_ < 0 || response_body_size_ > active_request_->maxBodyBytes) {
            fail(QStringLiteral("The phone returned an invalid response length"));
            disconnectPhone();
            return;
        }
        response_buffer_.remove(0, header_end + 4);
    }
    if (response_buffer_.size() < response_body_size_) {
        return;
    }
    if (response_buffer_.size() != response_body_size_) {
        fail(QStringLiteral("The phone returned an ambiguous Bluetooth response"));
        disconnectPhone();
        return;
    }
    const auto body = response_buffer_;
    const auto status = response_status_;
    const auto headers = response_headers_;
    auto finished = std::move(*active_request_);
    active_request_.reset();
    resetResponseState();
    finished.handler(status, headers, body);
    sendNext();
}

void PhoneClient::fetchCapabilities() {
    getJson(QStringLiteral("/v1/capabilities"), [this](const QJsonObject& object) {
        RemoteCapabilities capabilities;
        const auto version = object.value(QStringLiteral("protocol_version"));
        if (!version.isDouble() || version.toInt() != 1 ||
            !bounded_string(object, "device_id", capabilities.deviceId, 256) ||
            !bounded_string(object, "device_name", capabilities.deviceName, 256) ||
            !bounded_string(object, "permission_coverage", capabilities.permissionCoverage, 32) ||
            !object.value(QStringLiteral("supports_favorites")).isBool() ||
            !object.value(QStringLiteral("supports_recoverable_trash")).isBool()) {
            fail(QStringLiteral("Phone returned incompatible capabilities"));
            return;
        }
        capabilities.supportsFavorites = object.value(QStringLiteral("supports_favorites")).toBool();
        capabilities.supportsRecoverableTrash = object.value(QStringLiteral("supports_recoverable_trash")).toBool();
        emit capabilitiesReceived(capabilities);
    });
}

void PhoneClient::fetchCatalog(const QString& cursor, const int limit) {
    if (limit < 1 || limit > 1000 || cursor.size() > 512) {
        fail(QStringLiteral("The catalog request was outside safe bounds"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    if (!cursor.isEmpty()) {
        query.addQueryItem(QStringLiteral("cursor"), cursor);
    }
    enqueue(
        QByteArrayLiteral("GET"),
        target_with_query(QStringLiteral("/v1/catalog"), query),
        {},
        {},
        max_json_bytes,
        [this](const int status, const QHash<QByteArray, QByteArray>&, const QByteArray& body) {
            if (status == 409) {
                emit catalogInvalidated();
                return;
            }
            if (status != 200) {
                fail(QStringLiteral("Catalog transfer failed; reconnect and resume"));
                return;
            }
            auto page = parseCatalogPage(body);
            if (!page) {
                fail(page.error());
                return;
            }
            emit catalogPageReceived(*page);
        });
}

void PhoneClient::fetchThumbnail(const QString& asset_id, const int max_edge) {
    if (asset_id.isEmpty() || asset_id.size() > 512 || max_edge < 32 || max_edge > 2048) {
        fail(QStringLiteral("Invalid thumbnail request"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("asset_id"), asset_id);
    query.addQueryItem(QStringLiteral("max_edge"), QString::number(max_edge));
    enqueue(
        QByteArrayLiteral("GET"),
        target_with_query(QStringLiteral("/v1/thumbnail"), query),
        {},
        {},
        5 * 1024 * 1024,
        [this, asset_id](const int status, const QHash<QByteArray, QByteArray>&, const QByteArray& body) {
            if (status != 200 || body.isEmpty()) {
                fail(QStringLiteral("A media preview could not be read"));
                return;
            }
            emit thumbnailReceived(asset_id, body);
        });
}

void PhoneClient::fetchContent(
    const QString& asset_id,
    const std::uint64_t total_bytes,
    const QString& output_path) {
    cancelContentDownload();
    const QStorageInfo storage{QFileInfo{output_path}.absolutePath()};
    if (asset_id.isEmpty() || asset_id.size() > 512 || total_bytes == 0 ||
        total_bytes > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) || output_path.isEmpty() ||
        (storage.isValid() && storage.bytesAvailable() >= 0 &&
         static_cast<std::uint64_t>(storage.bytesAvailable()) < total_bytes + 64ULL * 1024ULL * 1024ULL)) {
        fail(QStringLiteral("This video cannot be cached safely on the available local disk"));
        return;
    }
    content_file_ = std::make_unique<QSaveFile>(output_path);
    if (!content_file_->open(QIODevice::WriteOnly)) {
        cancelContentDownload();
        fail(QStringLiteral("The temporary video cache could not be opened"));
        return;
    }
    content_asset_id_ = asset_id;
    content_output_path_ = output_path;
    content_total_bytes_ = total_bytes;
    content_offset_ = 0;
    fetchNextContentChunk();
}

void PhoneClient::cancelContent() { cancelContentDownload(); }

void PhoneClient::fetchNextContentChunk() {
    if (!content_file_ || content_offset_ >= content_total_bytes_) {
        return;
    }
    const auto generation = content_generation_;
    const auto remaining = content_total_bytes_ - content_offset_;
    const auto length = std::min(remaining, max_content_chunk_bytes);
    const auto expected_offset = content_offset_;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("asset_id"), content_asset_id_);
    enqueue(
        QByteArrayLiteral("GET"),
        target_with_query(QStringLiteral("/v1/content"), query),
        {},
        {{QByteArrayLiteral("Range"),
          QStringLiteral("bytes=%1-%2").arg(content_offset_).arg(content_offset_ + length - 1).toLatin1()}},
        static_cast<qsizetype>(length),
        [this, generation, expected_offset, length](
            const int status,
            const QHash<QByteArray, QByteArray>&,
            const QByteArray& body) {
            if (generation != content_generation_ || !content_file_ || expected_offset != content_offset_) {
                return;
            }
            if ((status != 200 && status != 206) || body.size() != static_cast<qsizetype>(length) ||
                content_file_->write(body) != body.size()) {
                cancelContentDownload();
                fail(QStringLiteral("Video transfer stopped before playback was ready"));
                return;
            }
            content_offset_ += length;
            emit contentProgress(
                content_asset_id_,
                static_cast<double>(content_offset_) / static_cast<double>(content_total_bytes_));
            if (content_offset_ < content_total_bytes_) {
                fetchNextContentChunk();
                return;
            }
            const auto asset_id = content_asset_id_;
            const auto output_path = content_output_path_;
            if (!content_file_->commit()) {
                cancelContentDownload();
                fail(QStringLiteral("The temporary video cache could not be finalized"));
                return;
            }
            content_file_.reset();
            content_asset_id_.clear();
            content_output_path_.clear();
            content_total_bytes_ = 0;
            content_offset_ = 0;
            emit contentReady(asset_id, QUrl::fromLocalFile(output_path));
        });
}

void PhoneClient::prepareTrash(const std::vector<QString>& asset_ids) {
    if (asset_ids.empty() || asset_ids.size() > 500) {
        fail(QStringLiteral("Trash batches must contain 1 to 500 items"));
        return;
    }
    QJsonArray ids;
    for (const auto& id : asset_ids) {
        if (id.isEmpty() || id.size() > 512) {
            fail(QStringLiteral("Trash batch contains an invalid item"));
            return;
        }
        ids.push_back(id);
    }
    postJson(QStringLiteral("/v1/trash/prepare"), QJsonObject{{QStringLiteral("asset_ids"), ids}}, [this](const QJsonObject& object) {
        RemoteTrashPrepared prepared;
        if (!bounded_string(object, "token", prepared.token, 256) || !json_uint64(object, "total_bytes", prepared.totalBytes) ||
            !object.value(QStringLiteral("asset_ids")).isArray()) {
            fail(QStringLiteral("Phone returned an invalid trash preparation"));
            return;
        }
        for (const auto& value : object.value(QStringLiteral("asset_ids")).toArray()) {
            if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 512) {
                fail(QStringLiteral("Phone returned an invalid trash preparation"));
                return;
            }
            prepared.assetIds.push_back(value.toString());
        }
        emit trashPrepared(prepared);
    });
}

void PhoneClient::commitTrash(const QString& preparation_token) {
    if (preparation_token.isEmpty() || preparation_token.size() > 256) {
        fail(QStringLiteral("Trash preparation expired"));
        return;
    }
    postJson(QStringLiteral("/v1/trash/commit"), QJsonObject{{QStringLiteral("token"), preparation_token}}, [this](const QJsonObject& object) {
        const auto token = object.value(QStringLiteral("token"));
        if (!token.isString()) {
            fail(QStringLiteral("Phone rejected the trash confirmation"));
            return;
        }
        pollTrash(token.toString());
    });
}

void PhoneClient::pollTrash(const QString& preparation_token) {
    if (preparation_token.isEmpty() || preparation_token.size() > 256) {
        fail(QStringLiteral("Trash result token is invalid"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), preparation_token);
    getJson(target_with_query(QStringLiteral("/v1/trash/result"), query), [this](const QJsonObject& object) {
        RemoteTrashResult result;
        const auto token = object.value(QStringLiteral("token"));
        const auto status = object.value(QStringLiteral("status"));
        if (!token.isString() || !status.isString()) {
            fail(QStringLiteral("Phone returned an invalid trash result"));
            return;
        }
        result.token = token.toString();
        result.pending = status.toString() == QStringLiteral("pending");
        result.userCancelled = object.value(QStringLiteral("user_cancelled")).toBool(false);
        for (const auto& key : {QStringLiteral("trashed_ids"), QStringLiteral("failed_ids")}) {
            const auto values = object.value(key);
            if (!values.isUndefined() && !values.isArray()) {
                fail(QStringLiteral("Phone returned an invalid trash result"));
                return;
            }
            auto& target = key == QStringLiteral("trashed_ids") ? result.trashedIds : result.failedIds;
            for (const auto& value : values.toArray()) {
                if (!value.isString() || value.toString().size() > 512) {
                    fail(QStringLiteral("Phone returned an invalid trash result"));
                    return;
                }
                target.push_back(value.toString());
            }
        }
        emit trashResultReceived(result);
    });
}

void PhoneClient::getJson(const QString& target, const std::function<void(const QJsonObject&)>& on_success) {
    enqueue(
        QByteArrayLiteral("GET"),
        target,
        {},
        {},
        max_json_bytes,
        [this, on_success](const int status, const QHash<QByteArray, QByteArray>&, const QByteArray& body) {
            const auto document = QJsonDocument::fromJson(body);
            if (status != 200 || !document.isObject()) {
                fail(QStringLiteral("The phone returned an invalid response"));
                return;
            }
            on_success(document.object());
        });
}

void PhoneClient::postJson(
    const QString& target,
    const QJsonObject& payload,
    const std::function<void(const QJsonObject&)>& on_success) {
    enqueue(
        QByteArrayLiteral("POST"),
        target,
        QJsonDocument{payload}.toJson(QJsonDocument::Compact),
        {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
        max_json_bytes,
        [this, on_success](const int status, const QHash<QByteArray, QByteArray>&, const QByteArray& body) {
            const auto document = QJsonDocument::fromJson(body);
            if ((status != 200 && status != 202) || !document.isObject()) {
                fail(QStringLiteral("The phone rejected the request"));
                return;
            }
            on_success(document.object());
        });
}

void PhoneClient::disconnectPhone() {
    cancelContentDownload();
    requests_.clear();
    active_request_.reset();
    resetResponseState();
    if (socket_) {
        disconnect(socket_, nullptr, this, nullptr);
        socket_->disconnectFromService();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (service_info_.isValid()) {
        service_info_.unregisterService();
        service_info_ = {};
    }
    if (server_) {
        server_->close();
        server_.reset();
    }
    bluetooth_address_.clear();
    secret_.clear();
    authenticated_ = false;
    handshake_buffer_.clear();
    qr_payload_.clear();
    qr_modules_.clear();
    qr_size_ = 0;
}

void PhoneClient::cancelContentDownload() {
    ++content_generation_;
    if (content_file_) {
        content_file_->cancelWriting();
    }
    content_file_.reset();
    content_asset_id_.clear();
    content_output_path_.clear();
    content_total_bytes_ = 0;
    content_offset_ = 0;
}

void PhoneClient::resetResponseState() {
    response_buffer_.clear();
    response_body_size_ = -1;
    response_status_ = 0;
    response_headers_.clear();
}

void PhoneClient::fail(const QString& safe_message) { emit requestFailed(safe_message); }

}  // namespace pms::desktop
