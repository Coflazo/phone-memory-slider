#include "phone_client.hpp"

#include <QHostAddress>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QSslCertificate>
#include <QSslError>
#include <QSslSocket>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pms::desktop {
namespace {

constexpr auto max_json_bytes = 2 * 1024 * 1024;
constexpr std::uint64_t max_content_chunk_bytes = 8ULL * 1024ULL * 1024ULL;

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

[[nodiscard]] bool expected_pin_error(const QSslError::SslError error) {
    return error == QSslError::SelfSignedCertificate || error == QSslError::SelfSignedCertificateInChain ||
           error == QSslError::UnableToGetLocalIssuerCertificate || error == QSslError::CertificateUntrusted ||
           error == QSslError::HostNameMismatch;
}

void enforce_reply_limit(QNetworkReply* reply, const qint64 limit) {
    reply->setReadBufferSize(limit + 1);
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply, limit] {
        if (reply->bytesAvailable() > limit) {
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit] {
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > limit) {
            reply->abort();
        }
    });
}

}  // namespace

PhoneClient::PhoneClient(QObject* parent) : QObject(parent), network_(this) {
    qRegisterMetaType<RemoteCatalogPage>();
    qRegisterMetaType<RemoteCapabilities>();
    qRegisterMetaType<RemoteTrashPrepared>();
    qRegisterMetaType<RemoteTrashResult>();
}

bool PhoneClient::isLocalEndpoint(const QUrl& endpoint) {
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("https") || endpoint.port() <= 0 ||
        !endpoint.userInfo().isEmpty() || !endpoint.query().isEmpty() || !endpoint.fragment().isEmpty() ||
        (!endpoint.path().isEmpty() && endpoint.path() != QStringLiteral("/"))) {
        return false;
    }
    QHostAddress address;
    if (!address.setAddress(endpoint.host())) {
        return false;
    }
    if (address.isLoopback() || address.isLinkLocal()) {
        return true;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const auto value = address.toIPv4Address();
        return (value & 0xFF000000U) == 0x0A000000U || (value & 0xFFF00000U) == 0xAC100000U ||
               (value & 0xFFFF0000U) == 0xC0A80000U;
    }
    const auto bytes = address.toIPv6Address();
    return (bytes[0] & 0xFEU) == 0xFCU;
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
            !json_uint64(object, "modified_epoch_ms", asset.modifiedEpochMs) ||
            !json_uint64(object, "width", width) || !json_uint64(object, "height", height) ||
            !json_uint64(object, "duration_ms", asset.durationMs) || width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max() || !object.value(QStringLiteral("favorite")).isBool()) {
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

void PhoneClient::pair(const QUrl& endpoint, const QString& code, const QString& desktop_name) {
    disconnectPhone();
    if (!isLocalEndpoint(endpoint) || code.size() != 6 ||
        !std::ranges::all_of(code, [](const QChar character) { return character.isDigit(); }) ||
        desktop_name.trimmed().isEmpty()) {
        fail(QStringLiteral("Check the local phone address and six-digit code"));
        return;
    }
    endpoint_ = endpoint;
    pending_code_ = code;
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/pair"));
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto payload = QJsonDocument{QJsonObject{{QStringLiteral("code"), code},
                                                   {QStringLiteral("desktop_name"), desktop_name.left(80)}}}
                             .toJson(QJsonDocument::Compact);
    auto* reply = network_.post(request, payload);
    enforce_reply_limit(reply, max_json_bytes);
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError>& errors) {
        const auto certificates = reply->sslConfiguration().peerCertificateChain();
        if (certificates.isEmpty() || !std::ranges::all_of(errors, [](const QSslError& error) {
                return expected_pin_error(error.error());
            })) {
            return;
        }
        const auto digest = certificates.front().digest(QCryptographicHash::Sha256);
        pinned_digest_ = digest;
        reply->ignoreSslErrors(errors);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation = session_generation_] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        reply->deleteLater();
        if (generation != session_generation_) {
            return;
        }
        if (error != QNetworkReply::NoError || status != 200 || body.size() > max_json_bytes) {
            pinned_digest_.clear();
            pending_code_.clear();
            fail(QStringLiteral("Pairing failed; verify the phone code and local connection"));
            return;
        }
        const auto document = QJsonDocument::fromJson(body);
        const auto token = document.object().value(QStringLiteral("token"));
        if (!document.isObject() || !token.isString() || token.toString().size() < 32 || pinned_digest_.isEmpty()) {
            fail(QStringLiteral("Phone returned an invalid pairing response"));
            return;
        }
        token_ = token.toString();
        pending_code_.clear();
        emit paired();
    });
}

void PhoneClient::fetchCatalog(const QString& cursor, const int limit) {
    if (token_.isEmpty() || pinned_digest_.isEmpty() || limit < 1 || limit > 1000 || cursor.size() > 512) {
        fail(QStringLiteral("No authenticated phone session is available"));
        return;
    }
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/catalog"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    if (!cursor.isEmpty()) {
        query.addQueryItem(QStringLiteral("cursor"), cursor);
    }
    url.setQuery(query);
    auto request = authenticatedRequest(url);
    auto* reply = network_.get(request);
    enforce_reply_limit(reply, max_json_bytes);
    pinReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation = session_generation_] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        reply->deleteLater();
        if (generation != session_generation_) {
            return;
        }
        if (status == 409) {
            emit catalogInvalidated();
            return;
        }
        if (error != QNetworkReply::NoError || status != 200) {
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

void PhoneClient::fetchContent(
    const QString& asset_id,
    const std::uint64_t total_bytes,
    const QString& output_path) {
    cancelContentDownload();
    const QStorageInfo storage{QFileInfo{output_path}.absolutePath()};
    if (token_.isEmpty() || pinned_digest_.isEmpty() || asset_id.isEmpty() || asset_id.size() > 512 ||
        total_bytes == 0 || total_bytes > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
        output_path.isEmpty() || !storage.isValid() || storage.isReadOnly() ||
        static_cast<std::uint64_t>(storage.bytesAvailable()) < total_bytes + 64ULL * 1024ULL * 1024ULL) {
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

void PhoneClient::cancelContent() {
    cancelContentDownload();
}

void PhoneClient::fetchNextContentChunk() {
    if (!content_file_ || content_offset_ >= content_total_bytes_) {
        return;
    }
    const auto remaining = content_total_bytes_ - content_offset_;
    const auto length = std::min(remaining, max_content_chunk_bytes);
    const auto end = content_offset_ + length - 1;
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/content"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("asset_id"), content_asset_id_);
    url.setQuery(query);
    auto request = authenticatedRequest(url);
    request.setRawHeader("Range", QStringLiteral("bytes=%1-%2").arg(content_offset_).arg(end).toLatin1());
    auto* reply = network_.get(request);
    content_reply_ = reply;
    enforce_reply_limit(reply, static_cast<qint64>(length));
    pinReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this,
                                                    reply,
                                                    expected_asset_id = content_asset_id_,
                                                    expected_offset = content_offset_,
                                                    length] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        if (content_reply_ == reply) {
            content_reply_.clear();
        }
        reply->deleteLater();
        if (!content_file_ || expected_asset_id != content_asset_id_ || expected_offset != content_offset_) {
            return;
        }
        if (error != QNetworkReply::NoError || (status != 200 && status != 206) ||
            body.size() != static_cast<qsizetype>(length) || content_file_->write(body) != body.size()) {
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

void PhoneClient::fetchCapabilities() {
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/capabilities"));
    getJson(url, [this](const QJsonObject& object) {
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
        capabilities.supportsRecoverableTrash =
            object.value(QStringLiteral("supports_recoverable_trash")).toBool();
        emit capabilitiesReceived(capabilities);
    });
}

void PhoneClient::fetchThumbnail(const QString& asset_id, const int max_edge) {
    if (token_.isEmpty() || asset_id.isEmpty() || asset_id.size() > 512 || max_edge < 32 || max_edge > 2048) {
        fail(QStringLiteral("Invalid thumbnail request"));
        return;
    }
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/thumbnail"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("asset_id"), asset_id);
    query.addQueryItem(QStringLiteral("max_edge"), QString::number(max_edge));
    url.setQuery(query);
    auto* reply = network_.get(authenticatedRequest(url));
    enforce_reply_limit(reply, 5 * 1024 * 1024);
    pinReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, asset_id, generation = session_generation_] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        reply->deleteLater();
        if (generation != session_generation_) {
            return;
        }
        if (error != QNetworkReply::NoError || status != 200 || body.isEmpty() || body.size() > 5 * 1024 * 1024) {
            fail(QStringLiteral("A media preview could not be read"));
            return;
        }
        emit thumbnailReceived(asset_id, body);
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
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/trash/prepare"));
    postJson(url, QJsonObject{{QStringLiteral("asset_ids"), ids}}, [this](const QJsonObject& object) {
        RemoteTrashPrepared prepared;
        if (!bounded_string(object, "token", prepared.token, 256) ||
            !json_uint64(object, "total_bytes", prepared.totalBytes) ||
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
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/trash/commit"));
    postJson(url, QJsonObject{{QStringLiteral("token"), preparation_token}}, [this](const QJsonObject& object) {
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
    auto url = endpoint_;
    url.setPath(QStringLiteral("/v1/trash/result"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), preparation_token);
    url.setQuery(query);
    getJson(url, [this](const QJsonObject& object) {
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
        for (const auto key : {QStringLiteral("trashed_ids"), QStringLiteral("failed_ids")}) {
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

QNetworkRequest PhoneClient::authenticatedRequest(const QUrl& url) const {
    QNetworkRequest request{url};
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return request;
}

void PhoneClient::pinReply(QNetworkReply* reply) {
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError>& errors) {
        const auto certificates = reply->sslConfiguration().peerCertificateChain();
        if (!certificates.isEmpty() && certificates.front().digest(QCryptographicHash::Sha256) == pinned_digest_ &&
            std::ranges::all_of(errors, [](const QSslError& error) { return expected_pin_error(error.error()); })) {
            reply->ignoreSslErrors(errors);
        }
    });
}

void PhoneClient::getJson(const QUrl& url, const std::function<void(const QJsonObject&)>& on_success) {
    if (token_.isEmpty() || pinned_digest_.isEmpty()) {
        fail(QStringLiteral("No authenticated phone session is available"));
        return;
    }
    auto* reply = network_.get(authenticatedRequest(url));
    enforce_reply_limit(reply, max_json_bytes);
    pinReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, on_success, generation = session_generation_] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        reply->deleteLater();
        if (generation != session_generation_) {
            return;
        }
        const auto document = QJsonDocument::fromJson(body);
        if (error != QNetworkReply::NoError || status != 200 || body.size() > max_json_bytes ||
            !document.isObject()) {
            fail(QStringLiteral("The phone returned an invalid response"));
            return;
        }
        on_success(document.object());
    });
}

void PhoneClient::postJson(
    const QUrl& url,
    const QJsonObject& payload,
    const std::function<void(const QJsonObject&)>& on_success) {
    if (token_.isEmpty() || pinned_digest_.isEmpty()) {
        fail(QStringLiteral("No authenticated phone session is available"));
        return;
    }
    auto request = authenticatedRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto* reply = network_.post(request, QJsonDocument{payload}.toJson(QJsonDocument::Compact));
    enforce_reply_limit(reply, max_json_bytes);
    pinReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, on_success, generation = session_generation_] {
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        reply->deleteLater();
        if (generation != session_generation_) {
            return;
        }
        const auto document = QJsonDocument::fromJson(body);
        if (error != QNetworkReply::NoError || (status != 200 && status != 202) || body.size() > max_json_bytes ||
            !document.isObject()) {
            fail(QStringLiteral("The phone rejected the request"));
            return;
        }
        on_success(document.object());
    });
}

void PhoneClient::disconnectPhone() {
    ++session_generation_;
    for (auto* reply : network_.findChildren<QNetworkReply*>()) {
        reply->abort();
    }
    cancelContentDownload();
    network_.clearAccessCache();
    endpoint_.clear();
    pinned_digest_.clear();
    pending_code_.clear();
    token_.clear();
}

void PhoneClient::cancelContentDownload() {
    const auto reply = content_reply_;
    content_reply_.clear();
    if (content_file_) {
        content_file_->cancelWriting();
    }
    content_file_.reset();
    content_asset_id_.clear();
    content_output_path_.clear();
    content_total_bytes_ = 0;
    content_offset_ = 0;
    if (reply) {
        reply->abort();
    }
}

void PhoneClient::fail(const QString& safe_message) {
    emit requestFailed(safe_message);
}

}  // namespace pms::desktop
