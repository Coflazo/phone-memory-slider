#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStyleHints>

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
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("reviewDeck"), &review_deck);
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &app_controller);
    const auto reduce_motion = app.styleHints()->property("reduceMotion").toBool();
    engine.rootContext()->setContextProperty(QStringLiteral("systemReducedMotion"), reduce_motion);
    engine.loadFromModule(QStringLiteral("PhoneMemorySlider"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}
