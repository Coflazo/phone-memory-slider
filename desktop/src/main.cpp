#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStyleHints>

#include "review_deck_model.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Phone Memory Slider"));
    app.setOrganizationName(QStringLiteral("Phone Memory Slider"));

    pms::desktop::ReviewDeckModel review_deck;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("reviewDeck"), &review_deck);
    const auto reduce_motion = app.styleHints()->property("reduceMotion").toBool();
    engine.rootContext()->setContextProperty(QStringLiteral("systemReducedMotion"), reduce_motion);
    engine.loadFromModule(QStringLiteral("PhoneMemorySlider"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}

