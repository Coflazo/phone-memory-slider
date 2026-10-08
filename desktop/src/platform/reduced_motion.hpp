#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace pms::desktop {

// The OS "reduce motion" setting. Windows: client-area animations off (SPI_GETCLIENTAREAANIMATION);
// macOS: NSWorkspace accessibilityDisplayShouldReduceMotion; Linux: KDE AnimationDurationFactor 0 or GNOME
// enable-animations false (blocks up to 1 s on gsettings, so Platform calls it off the GUI thread there).
// PMS_REDUCED_MOTION=1 forces true (tests, demos).
[[nodiscard]] bool system_reduced_motion();

// QML singleton "Platform". main.cpp registers the instance until Plan C Task 7 moves the C++ singletons
// into a QML module. The value is read again whenever the app becomes active, so a change made in the
// OS settings applies on return. On Linux the read is asynchronous: the property starts false (true when
// PMS_REDUCED_MOTION=1) and reducedMotionChanged follows once the answer arrives.
class Platform final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY reducedMotionChanged)

public:
    explicit Platform(QObject* parent = nullptr);

    [[nodiscard]] bool reducedMotion() const noexcept { return reduced_motion_; }

signals:
    void reducedMotionChanged();

private:
    void refresh();
    void apply(bool reduced);

    bool reduced_motion_{};
};

}  // namespace pms::desktop
