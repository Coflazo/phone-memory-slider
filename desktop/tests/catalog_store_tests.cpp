#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include <vector>

#include "catalog_store.hpp"

class CatalogStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void persistsPagesAndReplacesStaleRevisions() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        pms::desktop::CatalogStore store{directory.filePath(QStringLiteral("catalog.db"))};
        QVERIFY2(store.open(), qPrintable(store.errorString()));
        QVERIFY(store.beginSync(QStringLiteral("phone"), 7));

        std::vector<pms::desktop::CatalogRecord> firstPage{
            {.assetId = QStringLiteral("a"),
             .mimeType = QStringLiteral("image/jpeg"),
             .bytes = 100,
             .favorite = true,
             .embedding = pms::FixedEmbedding{{0.25F, 0.75F}},
             .perceptualHash = 42,
             .blurProblem = 0.2F,
             .keepScore = 0.9F,
             .label = QStringLiteral("favorite")},
            {.assetId = QStringLiteral("b"),
             .mimeType = QStringLiteral("video/mp4"),
             .bytes = 900,
             .durationMs = 4'000,
             .embedding = pms::FixedEmbedding{{0.8F, 0.1F}},
             .decision = QStringLiteral("delete")},
        };
        QVERIFY2(
            store.upsertPage(QStringLiteral("phone"), 7, firstPage, QStringLiteral("7:2"), false),
            qPrintable(store.errorString()));

        const auto state = store.syncState(QStringLiteral("phone"));
        QCOMPARE(state.revision, 7);
        QCOMPARE(state.cursor, QStringLiteral("7:2"));
        QVERIFY(!state.complete);
        const auto records = store.assets(QStringLiteral("phone"));
        QCOMPARE(records.size(), 2);
        QCOMPARE(records[0].assetId, QStringLiteral("a"));
        QVERIFY(std::abs(records[0].embedding[0] - 0.25F) < 0.002F);
        QCOMPARE(records[0].perceptualHash, 42);
        QVERIFY(std::abs(records[0].blurProblem - 0.2F) < 0.001F);
        QCOMPARE(records[1].decision, QStringLiteral("delete"));

        QVERIFY(store.beginSync(QStringLiteral("phone"), 7));
        QCOMPARE(store.assets(QStringLiteral("phone")).size(), 2);
        QVERIFY(store.beginSync(QStringLiteral("phone"), 8));
        QCOMPARE(store.assets(QStringLiteral("phone")).size(), 0);
    }

    void rejectsPagesFromTheWrongRevision() {
        QTemporaryDir directory;
        pms::desktop::CatalogStore store{directory.filePath(QStringLiteral("catalog.db"))};
        QVERIFY(store.open());
        QVERIFY(store.beginSync(QStringLiteral("phone"), 3));
        const std::vector<pms::desktop::CatalogRecord> page{{.assetId = QStringLiteral("a")}};
        QVERIFY(!store.upsertPage(QStringLiteral("phone"), 2, page, {}, true));
        QCOMPARE(store.assets(QStringLiteral("phone")).size(), 0);
    }

    void migratesThePreAnalysisSchemaWithoutDroppingRows() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("catalog.db"));
        const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            auto legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            legacy.setDatabaseName(path);
            QVERIFY(legacy.open());
            QSqlQuery query{legacy};
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE devices(device_id TEXT PRIMARY KEY, revision INTEGER NOT NULL, cursor TEXT NOT NULL DEFAULT '', complete INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE assets(device_id TEXT NOT NULL, asset_id TEXT NOT NULL, mime_type TEXT NOT NULL, bytes INTEGER NOT NULL, "
                "modified_ms INTEGER NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL, duration_ms INTEGER NOT NULL, "
                "favorite INTEGER NOT NULL, embedding BLOB NOT NULL, cleanup_score REAL NOT NULL, keep_score REAL NOT NULL, "
                "storage_score REAL NOT NULL, reasons TEXT NOT NULL, label TEXT NOT NULL, decision TEXT NOT NULL, "
                "PRIMARY KEY(device_id, asset_id))")));
            QVERIFY(query.exec(QStringLiteral(
                "INSERT INTO devices(device_id, revision, cursor, complete) VALUES('phone', 7, '', 1)")));
            QVERIFY(query.exec(QStringLiteral(
                "INSERT INTO assets VALUES('phone','kept','image/jpeg',12,0,1,1,0,0,X'0000000000000000000000000000000000000000000000000000000000000000',0,0.5,0,'','','')")));
            legacy.close();
        }
        QSqlDatabase::removeDatabase(connection);

        pms::desktop::CatalogStore store{path};
        QVERIFY2(store.open(), qPrintable(store.errorString()));
        const auto rows = store.assets(QStringLiteral("phone"));
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.front().assetId, QStringLiteral("kept"));
        QCOMPARE(rows.front().perceptualHash, 0);
    }
};

QTEST_MAIN(CatalogStoreTests)
#include "catalog_store_tests.moc"
