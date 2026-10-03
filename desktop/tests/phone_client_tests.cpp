#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>

#include "phone_client.hpp"

namespace {

class TlsPhoneFixture final : public QObject {
public:
    TlsPhoneFixture() {
        QFile certificate_file{QFINDTESTDATA("fixtures/test-certificate.der")};
        QFile key_file{QFINDTESTDATA("fixtures/test-key.der")};
        if (!certificate_file.open(QIODevice::ReadOnly) || !key_file.open(QIODevice::ReadOnly)) {
            qFatal("TLS fixture identity is missing");
        }
        certificate_ = QSslCertificate{&certificate_file, QSsl::Der};
        const QSslKey key{&key_file, QSsl::Rsa, QSsl::Der, QSsl::PrivateKey};
        if (certificate_.isNull() || key.isNull()) {
            qFatal("TLS fixture identity is invalid");
        }
        auto configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(certificate_);
        configuration.setPrivateKey(key);
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        server_.setSslConfiguration(configuration);
        connect(&server_, &QTcpServer::pendingConnectionAvailable, this, [this](auto) {
            while (server_.hasPendingConnections()) {
                auto* socket = qobject_cast<QSslSocket*>(server_.nextPendingConnection());
                if (socket == nullptr) {
                    continue;
                }
                connect(socket, &QIODevice::readyRead, socket, [this, socket] { receive(socket); });
                QTimer::singleShot(0, socket, [this, socket] { receive(socket); });
            }
        });
        if (!server_.listen(QHostAddress::LocalHost)) {
            qFatal("TLS fixture could not listen");
        }
    }

    [[nodiscard]] QUrl endpoint() const {
        return QUrl{QStringLiteral("https://127.0.0.1:%1").arg(server_.serverPort())};
    }

    [[nodiscard]] static QString code() { return QStringLiteral("481516"); }

private:
    void receive(QSslSocket* socket) {
        auto request = socket->property("request").toByteArray() + socket->readAll();
        socket->setProperty("request", request);
        const auto headers_end = request.indexOf("\r\n\r\n");
        if (headers_end < 0) {
            return;
        }
        const auto content_length_position = request.toLower().indexOf("content-length:");
        auto content_length = 0;
        if (content_length_position >= 0) {
            const auto line_end = request.indexOf("\r\n", content_length_position);
            content_length = request.mid(content_length_position + 15, line_end - content_length_position - 15)
                                 .trimmed()
                                 .toInt();
        }
        if (request.size() < headers_end + 4 + content_length) {
            return;
        }
        const auto first_line_end = request.indexOf("\r\n");
        const auto request_line = request.left(first_line_end);
        const auto authenticated = request.toLower().contains("authorization: bearer fixture-token");
        if (request_line.startsWith("POST /v1/pair ")) {
            if (request.contains(QByteArrayLiteral(R"("code":"481516")"))) {
                send(socket, 200, QByteArrayLiteral(R"({"token":"fixture-token-01234567890123456789","protocol_version":1})"));
            } else {
                send(socket, 403, QByteArrayLiteral(R"({"error":"pairing_rejected"})"));
            }
        } else if (!authenticated) {
            send(socket, 401, QByteArrayLiteral(R"({"error":"unauthorized"})"));
        } else if (request_line.startsWith("GET /v1/capabilities ")) {
            send(socket, 200, QByteArrayLiteral(R"({"protocol_version":1,"device_id":"fixture","device_name":"Fixture phone","permission_coverage":"full","supports_favorites":true,"supports_recoverable_trash":true})"));
        } else if (request_line.startsWith("GET /v1/catalog") && request_line.contains("cursor=stale")) {
            send(socket, 409, QByteArrayLiteral(R"({"error":"catalog_changed"})"));
        } else if (request_line.startsWith("GET /v1/catalog")) {
            send(socket, 200, QByteArrayLiteral(R"({"revision":1,"assets":[{"asset_id":"video-a","bytes":12,"mime_type":"video/mp4","modified_epoch_ms":1,"width":4,"height":3,"duration_ms":1000,"favorite":false}],"next_cursor":null,"complete":true})"));
        } else if (request_line.startsWith("GET /v1/content")) {
            send(socket, 206, QByteArrayLiteral("hello video!"), QByteArrayLiteral("video/mp4"));
        } else {
            send(socket, 404, QByteArrayLiteral(R"({"error":"not_found"})"));
        }
    }

    static void send(QSslSocket* socket, const int status, const QByteArray& body, const QByteArray& type = "application/json") {
        const auto reason = status == 200 ? QByteArrayLiteral("OK")
                          : status == 206 ? QByteArrayLiteral("Partial Content")
                          : status == 401 ? QByteArrayLiteral("Unauthorized")
                          : status == 403 ? QByteArrayLiteral("Forbidden")
                          : status == 409 ? QByteArrayLiteral("Conflict")
                                          : QByteArrayLiteral("Not Found");
        const auto content_range = status == 206
                                       ? QByteArrayLiteral("Content-Range: bytes 0-") +
                                             QByteArray::number(body.size() - 1) + '/' + QByteArray::number(body.size()) +
                                             QByteArrayLiteral("\r\n")
                                       : QByteArray{};
        socket->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + reason +
                      QByteArrayLiteral("\r\nContent-Type: ") + type +
                      QByteArrayLiteral("\r\nContent-Length: ") + QByteArray::number(body.size()) +
                      QByteArrayLiteral("\r\n") + content_range + QByteArrayLiteral("Connection: close\r\n\r\n") + body);
        socket->disconnectFromHost();
    }

    QSslServer server_;
    QSslCertificate certificate_;
};

}  // namespace

class PhoneClientTests final : public QObject {
    Q_OBJECT

private slots:
    void acceptsOnlyLocalIpEndpoints() {
        QVERIFY(pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("https://127.0.0.1:41820")}));
        QVERIFY(pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("https://192.168.1.4:41820")}));
        QVERIFY(pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("https://[fd12:3456::1]:41820")}));
        QVERIFY(!pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("https://8.8.8.8:41820")}));
        QVERIFY(!pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("http://192.168.1.4:41820")}));
        QVERIFY(!pms::desktop::PhoneClient::isLocalEndpoint(QUrl{QStringLiteral("https://example.com:41820")}));
    }

    void parsesBoundedCatalogPayloadAndRejectsMalformedTypes() {
        const auto valid = QByteArrayLiteral(
            R"({"revision":7,"assets":[{"asset_id":"opaque-a","bytes":123,"mime_type":"image/jpeg","modified_epoch_ms":4,"width":40,"height":30,"duration_ms":0,"favorite":true}],"next_cursor":"7:1","complete":false})");
        const auto page = pms::desktop::PhoneClient::parseCatalogPage(valid);
        if (!page.has_value()) {
            QFAIL(qPrintable(page.error()));
        }
        QCOMPARE(page->revision, 7);
        QCOMPARE(page->assets.size(), 1);
        QCOMPARE(page->assets.front().assetId, QStringLiteral("opaque-a"));
        QVERIFY(page->assets.front().favorite);
        QCOMPARE(page->nextCursor, QStringLiteral("7:1"));

        const auto malformed = pms::desktop::PhoneClient::parseCatalogPage(
            QByteArrayLiteral(R"({"revision":"7","assets":[],"complete":true})"));
        QVERIFY(!malformed.has_value());
    }

    void pairsPinsAuthenticatesAndDownloadsRangedContentOverTls() {
        TlsPhoneFixture fixture;
        pms::desktop::PhoneClient client;
        QSignalSpy paired{&client, &pms::desktop::PhoneClient::paired};
        client.pair(fixture.endpoint(), fixture.code(), QStringLiteral("Test desktop"));
        QVERIFY2(paired.wait(), "The pinned TLS fixture did not pair");

        QSignalSpy capabilities{&client, &pms::desktop::PhoneClient::capabilitiesReceived};
        client.fetchCapabilities();
        QVERIFY(capabilities.wait());

        QSignalSpy catalog{&client, &pms::desktop::PhoneClient::catalogPageReceived};
        client.fetchCatalog();
        QVERIFY(catalog.wait());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSignalSpy content{&client, &pms::desktop::PhoneClient::contentReady};
        QSignalSpy transfer_failed{&client, &pms::desktop::PhoneClient::requestFailed};
        const auto output = directory.filePath(QStringLiteral("video.mp4"));
        client.fetchContent(QStringLiteral("video-a"), 12, output);
        if (!content.wait()) {
            const auto message = transfer_failed.isEmpty()
                                     ? QStringLiteral("Video transfer timed out without an error")
                                     : transfer_failed.front().front().toString();
            QFAIL(qPrintable(message));
        }
        QFile file{output};
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArrayLiteral("hello video!"));

        QSignalSpy invalidated{&client, &pms::desktop::PhoneClient::catalogInvalidated};
        client.fetchCatalog(QStringLiteral("stale"));
        QVERIFY(invalidated.wait());
    }

    void rejectsTheWrongPairingCode() {
        TlsPhoneFixture fixture;
        pms::desktop::PhoneClient client;
        QSignalSpy failed{&client, &pms::desktop::PhoneClient::requestFailed};
        client.pair(fixture.endpoint(), QStringLiteral("000000"), QStringLiteral("Test desktop"));
        QVERIFY(failed.wait());
    }
};

QTEST_MAIN(PhoneClientTests)
#include "phone_client_tests.moc"
