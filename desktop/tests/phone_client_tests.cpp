#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "phone_client.hpp"

class PhoneClientTests final : public QObject {
    Q_OBJECT

private slots:
    void buildsOfflinePairingPayload() {
        const auto payload = pms::desktop::PhoneClient::makePairingPayload(
            QStringLiteral("AA:BB:CC:DD:EE:FF"),
            QStringLiteral("0123456789012345678901234567890123456789012"),
            QStringLiteral("Studio PC"));

        QVERIFY(payload.startsWith(QStringLiteral("pms://pair?")));
        QVERIFY(payload.contains(QStringLiteral("address=AA:BB:CC:DD:EE:FF")) ||
                payload.contains(QStringLiteral("address=AA%3ABB%3ACC%3ADD%3AEE%3AFF")));
        QVERIFY(payload.contains(QStringLiteral("uuid=8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf")));
        QVERIFY(!payload.contains(QStringLiteral("http"), Qt::CaseInsensitive));
    }

    void parsesFavoriteFromCatalog() {
        const QJsonObject asset{
            {QStringLiteral("asset_id"), QStringLiteral("opaque-1")},
            {QStringLiteral("mime_type"), QStringLiteral("image/jpeg")},
            {QStringLiteral("bytes"), 1200},
            {QStringLiteral("modified_epoch_ms"), 42},
            {QStringLiteral("width"), 640},
            {QStringLiteral("height"), 480},
            {QStringLiteral("duration_ms"), 0},
            {QStringLiteral("favorite"), true},
        };
        const auto body = QJsonDocument{QJsonObject{
                                             {QStringLiteral("revision"), 7},
                                             {QStringLiteral("assets"), QJsonArray{asset}},
                                             {QStringLiteral("next_cursor"), QJsonValue::Null},
                                             {QStringLiteral("complete"), true},
                                         }}
                              .toJson(QJsonDocument::Compact);

        const auto page = pms::desktop::PhoneClient::parseCatalogPage(body);

        if (!page) {
            QFAIL(qPrintable(page.error()));
        }
        QCOMPARE(page->assets.size(), 1);
        QVERIFY(page->assets.front().favorite);
    }

    void requiresExactPairingProof() {
        const auto secret = QStringLiteral("0123456789012345678901234567890123456789012");

        QVERIFY(pms::desktop::PhoneClient::validPairingHandshake(
            QByteArrayLiteral("PMS/1 AUTH 0123456789012345678901234567890123456789012"), secret));
        QVERIFY(!pms::desktop::PhoneClient::validPairingHandshake(
            QByteArrayLiteral("PMS/1 AUTH 1123456789012345678901234567890123456789012"), secret));
        QVERIFY(!pms::desktop::PhoneClient::validPairingHandshake(
            QByteArrayLiteral("PMS/1 AUTH 0123456789012345678901234567890123456789012 "), secret));
    }
};

QTEST_MAIN(PhoneClientTests)
#include "phone_client_tests.moc"
