#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

#include "platform/reduced_motion.hpp"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

class ReducedMotionTests final : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { qunsetenv("PMS_REDUCED_MOTION"); }

    void environmentForcesReducedMotion() {
        qputenv("PMS_REDUCED_MOTION", "1");
        const pms::desktop::Platform platform;
        QVERIFY(platform.reducedMotion());
    }

#ifdef Q_OS_WIN
    void followsWindowsClientAreaAnimation() {
        BOOL animations = TRUE;
        QVERIFY(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0));
        const pms::desktop::Platform platform;
        QCOMPARE(platform.reducedMotion(), animations == FALSE);
    }
#endif

    void rereadsWhenTheAppBecomesActive() {
        pms::desktop::Platform platform;
        if (platform.reducedMotion()) {
            QSKIP("The OS already asks for reduced motion");
        }
        QSignalSpy changed{&platform, &pms::desktop::Platform::reducedMotionChanged};
        qputenv("PMS_REDUCED_MOTION", "1");
        emit qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        QCOMPARE(changed.count(), 1);
        QVERIFY(platform.reducedMotion());
    }
};

QTEST_MAIN(ReducedMotionTests)
#include "reduced_motion_tests.moc"
