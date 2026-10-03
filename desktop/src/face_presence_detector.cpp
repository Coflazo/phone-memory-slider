#include "face_presence_detector.hpp"

#include <QColor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pms::desktop {
namespace {

[[nodiscard]] bool skin_pixel(const QRgb pixel) noexcept {
    const auto red = qRed(pixel);
    const auto green = qGreen(pixel);
    const auto blue = qBlue(pixel);
    const auto maximum = std::max({red, green, blue});
    const auto minimum = std::min({red, green, blue});
    return red > 95 && green > 40 && blue > 20 && maximum - minimum > 15 &&
           std::abs(red - green) > 15 && red > green && red > blue;
}

}  // namespace

bool FacePresenceDetector::detect(const QImage& image) {
    if (image.isNull() || image.width() < 24 || image.height() < 24) {
        return false;
    }
    const auto sample = image.convertToFormat(QImage::Format_RGB32)
                            .scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const auto width = sample.width();
    const auto height = sample.height();
    const auto pixel_count = static_cast<std::size_t>(width * height);
    std::vector<unsigned char> skin(pixel_count);
    std::vector<unsigned char> visited(pixel_count);
    for (auto y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        for (auto x = 0; x < width; ++x) {
            skin[static_cast<std::size_t>(y * width + x)] = skin_pixel(row[x]) ? 1U : 0U;
        }
    }

    std::vector<int> stack;
    stack.reserve(pixel_count / 4);
    for (auto start = 0; start < width * height; ++start) {
        const auto start_index = static_cast<std::size_t>(start);
        if (visited[start_index] != 0U || skin[start_index] == 0U) {
            continue;
        }
        stack.clear();
        stack.push_back(start);
        visited[start_index] = 1U;
        auto count = 0;
        auto min_x = width;
        auto max_x = 0;
        auto min_y = height;
        auto max_y = 0;
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            const auto x = current % width;
            const auto y = current / width;
            ++count;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            constexpr std::array<std::array<int, 2>, 4> directions{{{{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
            for (const auto& direction : directions) {
                const auto next_x = x + direction[0];
                const auto next_y = y + direction[1];
                if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height) {
                    continue;
                }
                const auto next = static_cast<std::size_t>(next_y * width + next_x);
                if (skin[next] != 0U && visited[next] == 0U) {
                    visited[next] = 1U;
                    stack.push_back(static_cast<int>(next));
                }
            }
        }
        const auto fraction = static_cast<double>(count) / static_cast<double>(pixel_count);
        const auto component_width = max_x - min_x + 1;
        const auto component_height = max_y - min_y + 1;
        const auto aspect = static_cast<double>(component_width) / static_cast<double>(component_height);
        if (fraction >= 0.02 && fraction <= 0.45 && aspect >= 0.50 && aspect <= 1.45 && min_y < height * 3 / 4) {
            return true;
        }
    }
    return false;
}

}  // namespace pms::desktop
