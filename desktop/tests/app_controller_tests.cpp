#include <QTemporaryDir>
#include <QtTest>

#include "app_controller.hpp"

class AppControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsNonLocalPairingBeforeNetworkAccess() {
        QTemporaryDir directory;
        pms::desktop::ReviewDeckModel deck;
        pms::desktop::AppController controller{&deck, directory.filePath(QStringLiteral("catalog.db"))};
        QCOMPARE(controller.stage(), pms::desktop::AppController::Stage::Welcome);

        controller.startPairing(QStringLiteral("https://8.8.8.8:41820"), QStringLiteral("123456"));

        QCOMPARE(controller.stage(), pms::desktop::AppController::Stage::Error);
        QVERIFY(!controller.errorText().isEmpty());
        controller.cancel();
        QCOMPARE(controller.stage(), pms::desktop::AppController::Stage::Welcome);
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
};

QTEST_MAIN(AppControllerTests)
#include "app_controller_tests.moc"
