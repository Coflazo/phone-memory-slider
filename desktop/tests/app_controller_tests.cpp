#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "app_controller.hpp"

class AppControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void scansAFolderAndRunsPersonalizedLocalAnalysis() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto gallery = directory.filePath(QStringLiteral("DCIM"));
        QVERIFY(QDir{}.mkpath(gallery));
        QImage image{128, 96, QImage::Format_RGB32};
        image.fill(QColor{220, 84, 52});
        const auto photo = QDir{gallery}.filePath(QStringLiteral("IMG_0001.png"));
        QVERIFY(image.save(photo));

        pms::desktop::ReviewDeckModel deck;
        pms::desktop::AppController controller{
            &deck,
            directory.filePath(QStringLiteral("catalog.db")),
            directory.filePath(QStringLiteral("vault"))};
        QCOMPARE(controller.stage(), pms::desktop::AppController::Stage::Welcome);

        controller.scanFolder(QUrl::fromLocalFile(gallery));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.stage() == pms::desktop::AppController::Stage::Seeding ||
                controller.stage() == pms::desktop::AppController::Stage::Error,
            5'000);
        QVERIFY2(controller.stage() == pms::desktop::AppController::Stage::Seeding,
                 qPrintable(controller.errorText()));
        QCOMPARE(controller.deviceName(), QStringLiteral("DCIM"));

        QList<QUrl> seeds;
        for (auto index = 0; index < 20; ++index) {
            seeds.push_back(QUrl::fromLocalFile(photo));
        }
        controller.analyzeWithSeeds(seeds);
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Review, 5'000);
        QCOMPARE(controller.seedCount(), 20);
        QCOMPARE(deck.remaining(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            deck.data(deck.index(0), pms::desktop::ReviewDeckModel::PreviewUrlRole).toUrl().isLocalFile(),
            5'000);
        const auto preview = deck.data(deck.index(0), pms::desktop::ReviewDeckModel::PreviewUrlRole).toUrl().toLocalFile();
        QVERIFY(!QImage{preview}.isNull());

        deck.deleteCurrent();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Summary, 5'000);
        controller.confirmDelete();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Complete, 10'000);
        QVERIFY(!QFileInfo::exists(photo));
        const auto encrypted = QDir{directory.filePath(QStringLiteral("vault"))}
                                   .entryList({QStringLiteral("*.pmsvault")}, QDir::Files);
        QCOMPARE(encrypted.size(), 1);
    }

    void deckProtectsFavoritesAndReportsRecoverableBatch() {
        pms::desktop::ReviewDeckModel deck;
        std::vector<pms::ReviewItem> items{
            {{"ordinary", 1.0F, 0.1F, 1.0F, false, false}, pms::MediaKind::Photo, 100},
            {{"favorite", 1.0F, 1.0F, 1.0F, true, false}, pms::MediaKind::Photo, 200},
        };
        deck.loadItems(std::move(items));
        QCOMPARE(deck.currentAssetId(), QStringLiteral("ordinary"));
        deck.deleteCurrent();
        deck.deleteCurrent();
        const auto batch = deck.pendingTrashBatch();
        QCOMPARE(batch.asset_ids.size(), 1);
        QCOMPARE(batch.asset_ids.front(), std::string{"ordinary"});
        QCOMPARE(batch.total_bytes, 100);
    }

    void refusesToDeleteBytesThatChangedAfterReview() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto gallery = directory.filePath(QStringLiteral("DCIM"));
        QVERIFY(QDir{}.mkpath(gallery));
        const auto photo = QDir{gallery}.filePath(QStringLiteral("mutable.png"));
        QImage image{64, 64, QImage::Format_RGB32};
        image.fill(Qt::red);
        QVERIFY(image.save(photo));

        pms::desktop::ReviewDeckModel deck;
        pms::desktop::AppController controller{
            &deck,
            directory.filePath(QStringLiteral("catalog.db")),
            directory.filePath(QStringLiteral("vault"))};
        controller.scanFolder(QUrl::fromLocalFile(gallery));
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Seeding, 5'000);
        controller.analyzeWithoutSeeds();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Review, 5'000);
        deck.deleteCurrent();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Summary, 5'000);

        image.fill(Qt::blue);
        QVERIFY(image.save(photo));
        controller.confirmDelete();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Error, 10'000);
        QVERIFY(QFileInfo::exists(photo));
        QVERIFY(controller.errorText().contains(QStringLiteral("changed since review")));
        QCOMPARE(QDir{directory.filePath(QStringLiteral("vault"))}
                     .entryList({QStringLiteral("*.pmsvault")}, QDir::Files)
                     .size(),
                 0);
    }

    void learnsFromExportedFavoritesFoldersAutomatically() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto gallery = directory.filePath(QStringLiteral("Gallery"));
        const auto favorites = QDir{gallery}.filePath(QStringLiteral("Favorites"));
        QVERIFY(QDir{}.mkpath(favorites));
        QImage image{32, 32, QImage::Format_RGB32};
        for (auto index = 0; index < 20; ++index) {
            image.fill(QColor{80 + index, 120, 180});
            QVERIFY(image.save(QDir{favorites}.filePath(QStringLiteral("favorite-%1.png").arg(index))));
        }
        image.fill(Qt::gray);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("ordinary.png"))));

        pms::desktop::ReviewDeckModel deck;
        pms::desktop::AppController controller{
            &deck,
            directory.filePath(QStringLiteral("catalog.db")),
            directory.filePath(QStringLiteral("vault"))};
        controller.scanFolder(QUrl::fromLocalFile(gallery));
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Seeding, 5'000);
        QVERIFY(controller.statusText().contains(QStringLiteral("20 in Favorites")));
        controller.analyzeWithoutSeeds();
        QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Review, 10'000);
        QCOMPARE(controller.seedCount(), 0);
        QCOMPARE(deck.remaining(), 1);
        QVERIFY(controller.statusText().contains(QStringLiteral("resemblance")));
    }

    void persistsKeepSwipesAcrossLaterScans() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto gallery = directory.filePath(QStringLiteral("DCIM"));
        QVERIFY(QDir{}.mkpath(gallery));
        QImage image{48, 48, QImage::Format_RGB32};
        image.fill(QColor{32, 140, 210});
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("remember-me.png"))));
        const auto database = directory.filePath(QStringLiteral("catalog.db"));
        const auto vault = directory.filePath(QStringLiteral("vault"));

        {
            pms::desktop::ReviewDeckModel deck;
            pms::desktop::AppController controller{&deck, database, vault};
            controller.scanFolder(QUrl::fromLocalFile(gallery));
            QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Seeding, 5'000);
            controller.analyzeWithoutSeeds();
            QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Review, 5'000);
            QCOMPARE(deck.remaining(), 1);
            deck.keepCurrent();
            QTRY_COMPARE_WITH_TIMEOUT(controller.stage(), pms::desktop::AppController::Stage::Summary, 5'000);
        }

        QTest::qWait(2);
        pms::desktop::ReviewDeckModel later_deck;
        pms::desktop::AppController later_controller{&later_deck, database, vault};
        later_controller.scanFolder(QUrl::fromLocalFile(gallery));
        QTRY_COMPARE_WITH_TIMEOUT(later_controller.stage(), pms::desktop::AppController::Stage::Seeding, 5'000);
        later_controller.analyzeWithoutSeeds();
        QTRY_COMPARE_WITH_TIMEOUT(later_controller.stage(), pms::desktop::AppController::Stage::Summary, 5'000);
        QCOMPARE(later_deck.remaining(), 0);
    }
};

QTEST_MAIN(AppControllerTests)
#include "app_controller_tests.moc"
