#include <QPainter>
#include <QtTest>

#include <cmath>

#include "visual_encoder.hpp"

class VisualEncoderTests final : public QObject {
    Q_OBJECT

private slots:
    void emitsFiniteFixedEmbedding() {
        QImage image(80, 60, QImage::Format_RGB32);
        for (auto y = 0; y < image.height(); ++y) {
            for (auto x = 0; x < image.width(); ++x) {
                image.setPixelColor(x, y, QColor{20 + x * 2, 80, 180});
            }
        }
        const auto encoded = pms::desktop::VisualEncoder::encode(image, false, 4'000'000, 0);
        for (const auto value : encoded.embedding.values()) {
            QVERIFY(std::isfinite(value));
            QVERIFY(value >= 0.0F && value <= 1.0F);
        }
        QVERIFY(encoded.perceptualHash != 0);
        QVERIFY(!encoded.facePresent);
    }

    void detectsFacePresenceWithoutIdentity() {
        QImage image(180, 180, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setBrush(QColor{214, 154, 112});
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRect{45, 20, 90, 125});
        painter.setBrush(Qt::black);
        painter.drawEllipse(QRect{70, 65, 10, 7});
        painter.drawEllipse(QRect{100, 65, 10, 7});
        painter.end();

        const auto encoded = pms::desktop::VisualEncoder::encode(image, false, 200'000, 0);
        QVERIFY(encoded.facePresent);
        QVERIFY(encoded.embedding[14] == 1.0F);
    }
};

QTEST_MAIN(VisualEncoderTests)
#include "visual_encoder_tests.moc"
