#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStyleHints>
#include <QTimer>
#include <qqml.h>

#ifdef PMS_ENABLE_DEMO_AUTOMATION
#include <QDir>
#include <QDirIterator>
#include <QQuickWindow>

#include <functional>
#include <memory>
#endif

#include "review_deck_model.hpp"
#include "app_controller.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Phone Memory Slider"));
    app.setOrganizationName(QStringLiteral("Phone Memory Slider"));
    for (const auto* path : {
             ":/qt/qml/PhoneMemorySlider/resources/fonts/Geist-Regular.ttf",
             ":/qt/qml/PhoneMemorySlider/resources/fonts/Geist-SemiBold.ttf",
             ":/qt/qml/PhoneMemorySlider/resources/fonts/Geist-Bold.ttf",
             ":/qt/qml/PhoneMemorySlider/resources/fonts/GeistMono-Regular.ttf",
         }) {
        QFontDatabase::addApplicationFont(QString::fromLatin1(path));
    }
    app.setFont(QFont{QStringLiteral("Geist")});

    pms::desktop::ReviewDeckModel review_deck;
    pms::desktop::AppController app_controller{&review_deck};
    qmlRegisterUncreatableType<pms::desktop::AppController>(
        "PhoneMemorySlider", 1, 0, "AppController", QStringLiteral("Provided by the desktop runtime"));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("reviewDeck"), &review_deck);
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &app_controller);
    const auto reduce_motion = app.styleHints()->property("reduceMotion").toBool();
    engine.rootContext()->setContextProperty(QStringLiteral("systemReducedMotion"), reduce_motion);
    engine.loadFromModule(QStringLiteral("PhoneMemorySlider"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
#ifdef PMS_ENABLE_DEMO_AUTOMATION
    const auto demo_gallery = qEnvironmentVariable("PMS_DEMO_GALLERY");
    const auto capture_directory = qEnvironmentVariable("PMS_DEMO_CAPTURE_DIR");
    if (!demo_gallery.isEmpty()) {
        auto* root = engine.rootObjects().front();
        auto capture_frame = std::make_shared<std::function<void()>>([] {});
        if (!capture_directory.isEmpty()) {
            QDir{}.mkpath(capture_directory);
            auto frame = std::make_shared<int>(0);
            auto* window = qobject_cast<QQuickWindow*>(root);
            *capture_frame = [window, capture_directory, frame] {
                if (window == nullptr) {
                    return;
                }
                const auto image = window->grabWindow();
                if (!image.isNull()) {
                    image.save(QDir{capture_directory}.filePath(
                        QStringLiteral("frame-%1.png").arg((*frame)++, 5, 10, QLatin1Char('0'))));
                }
            };
        }
        auto capture_later = [&app, capture_frame](int delay_ms) {
            QTimer::singleShot(delay_ms, &app, [capture_frame] { (*capture_frame)(); });
        };
        capture_later(250);
        capture_later(800);

        auto review_started = std::make_shared<bool>(false);
        auto seeds_started = std::make_shared<bool>(false);
        auto analysis_bucket = std::make_shared<int>(-1);
        QObject::connect(&app_controller, &pms::desktop::AppController::stateChanged, &app,
                         [&, root, review_started, seeds_started, analysis_bucket, capture_frame, capture_later] {
            if (app_controller.stage() == pms::desktop::AppController::Stage::Syncing) {
                capture_later(120);
                capture_later(500);
            }
            if (app_controller.stage() == pms::desktop::AppController::Stage::Seeding && !*seeds_started) {
                *seeds_started = true;
                capture_later(250);
                capture_later(650);
                QList<QUrl> seeds;
                QDirIterator iterator{demo_gallery, QDir::Files, QDirIterator::Subdirectories};
                while (iterator.hasNext() && seeds.size() < 20) {
                    seeds.push_back(QUrl::fromLocalFile(iterator.next()));
                }
                while (!seeds.isEmpty() && seeds.size() < 20) {
                    seeds.push_back(seeds.front());
                }
                QTimer::singleShot(900, &app, [&, seeds] { app_controller.analyzeWithSeeds(seeds); });
            }
            if (app_controller.stage() == pms::desktop::AppController::Stage::Analyzing) {
                const auto bucket = static_cast<int>(app_controller.progress() * 4.0);
                if (bucket > *analysis_bucket) {
                    *analysis_bucket = bucket;
                    capture_later(80);
                }
            }
            if (app_controller.stage() == pms::desktop::AppController::Stage::Review && !*review_started) {
                *review_started = true;
                capture_later(300);
                capture_later(700);
                auto swipe_count = std::make_shared<int>(0);
                auto swipe = std::make_shared<std::function<void()>>();
                *swipe = [&, root, swipe_count, swipe, capture_frame, capture_later] {
                    if (app_controller.stage() != pms::desktop::AppController::Stage::Review) {
                        return;
                    }
                    (*capture_frame)();
                    QMetaObject::invokeMethod(
                        root,
                        "demoSwipe",
                        Q_ARG(QVariant, QVariant{(*swipe_count % 3) == 0}));
                    capture_later(110);
                    capture_later(340);
                    ++*swipe_count;
                    QTimer::singleShot(850, &app, *swipe);
                };
                QTimer::singleShot(1000, &app, *swipe);
            }
            if (app_controller.stage() == pms::desktop::AppController::Stage::Summary) {
                capture_later(250);
                capture_later(900);
                QTimer::singleShot(2200, &app, &QCoreApplication::quit);
            }
        });
        QTimer::singleShot(1200, &app, [&app_controller, demo_gallery] {
            app_controller.scanFolder(QUrl::fromLocalFile(demo_gallery));
        });
        QTimer::singleShot(capture_directory.isEmpty() ? 30'000 : 60'000, &app, &QCoreApplication::quit);
    }
#endif
    if (qEnvironmentVariableIsSet("PMS_SMOKE_TEST")) {
        QTimer::singleShot(750, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
