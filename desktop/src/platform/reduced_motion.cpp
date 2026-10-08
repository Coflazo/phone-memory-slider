#include "platform/reduced_motion.hpp"

#include <QGuiApplication>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#endif

namespace pms::desktop {

bool system_reduced_motion() {
    if (qEnvironmentVariable("PMS_REDUCED_MOTION") == QLatin1StringView{"1"}) {
        return true;
    }
#if defined(Q_OS_WIN)
    BOOL animations = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) != FALSE && animations == FALSE;
#elif defined(Q_OS_MACOS)
    Boolean valid = false;
    const auto reduce = CFPreferencesGetAppBooleanValue(
        CFSTR("reduceMotion"), CFSTR("com.apple.universalaccess"), &valid);
    return valid && reduce;
#else
    const auto kdeglobals = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    if (!kdeglobals.isEmpty()) {
        const QSettings kde{kdeglobals, QSettings::IniFormat};
        bool parsed = false;
        const auto factor = kde.value(QStringLiteral("KDE/AnimationDurationFactor")).toDouble(&parsed);
        if (parsed && qFuzzyIsNull(factor)) {
            return true;
        }
    }
    // ponytail: one short gsettings call when the value is read; an XDG portal listener if it shows up in a profile.
    QProcess gsettings;
    gsettings.start(QStringLiteral("gsettings"),
                    {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("enable-animations")});
    if (!gsettings.waitForFinished(1000) || gsettings.exitStatus() != QProcess::NormalExit || gsettings.exitCode() != 0) {
        gsettings.kill();
        return false;
    }
    return gsettings.readAllStandardOutput().trimmed() == "false";
#endif
}

Platform::Platform(QObject* parent) : QObject(parent), reduced_motion_(system_reduced_motion()) {
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](const Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                refresh();
            }
        });
    }
}

void Platform::refresh() {
    const auto reduced = system_reduced_motion();
    if (reduced != reduced_motion_) {
        reduced_motion_ = reduced;
        emit reducedMotionChanged();
    }
}

}  // namespace pms::desktop
