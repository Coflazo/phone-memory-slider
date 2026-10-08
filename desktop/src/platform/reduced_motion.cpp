#include "platform/reduced_motion.hpp"

#include <QGuiApplication>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif defined(Q_OS_MACOS)
namespace pms::desktop::detail {
[[nodiscard]] bool mac_reduced_motion() noexcept;  // reduced_motion_mac.mm
}
#else
#include <QFuture>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#endif

namespace pms::desktop {
namespace {

bool forced_by_environment() {
    return qEnvironmentVariable("PMS_REDUCED_MOTION") == QLatin1StringView{"1"};
}

}  // namespace

bool system_reduced_motion() {
    if (forced_by_environment()) {
        return true;
    }
#if defined(Q_OS_WIN)
    BOOL animations = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) != FALSE && animations == FALSE;
#elif defined(Q_OS_MACOS)
    return detail::mac_reduced_motion();
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
    QProcess gsettings;
    gsettings.start(QStringLiteral("gsettings"),
                    {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("enable-animations")});
    if (!gsettings.waitForFinished(1000) || gsettings.exitStatus() != QProcess::NormalExit || gsettings.exitCode() != 0) {
        gsettings.kill();
        gsettings.waitForFinished(100);
        return false;
    }
    return gsettings.readAllStandardOutput().trimmed() == "false";
#endif
}

Platform::Platform(QObject* parent) : QObject(parent), reduced_motion_(forced_by_environment()) {
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](const Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                refresh();
            }
        });
    }
    refresh();
}

void Platform::refresh() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    apply(system_reduced_motion());  // one system call, cheap enough for the GUI thread
#else
    if (forced_by_environment()) {
        apply(true);
        return;
    }
    // KDE's config file and the gsettings process are read on a worker thread; the answer comes back queued to
    // this object's thread, and is dropped if this object is gone by then.
    static_cast<void>(QtConcurrent::run(&system_reduced_motion).then(this, [this](const bool reduced) { apply(reduced); }));
#endif
}

void Platform::apply(const bool reduced) {
    if (reduced != reduced_motion_) {
        reduced_motion_ = reduced;
        emit reducedMotionChanged();
    }
}

}  // namespace pms::desktop
