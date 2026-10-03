#pragma once

#include <QImage>

namespace pms::desktop {

class FacePresenceDetector final {
public:
    [[nodiscard]] static bool detect(const QImage& image);
};

}  // namespace pms::desktop
