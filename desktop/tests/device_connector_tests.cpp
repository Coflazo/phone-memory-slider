#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "device_connector.hpp"

class DeviceConnectorTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsFolderTraversalOutsideTheSelectedRoot() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto gallery = directory.filePath(QStringLiteral("Gallery"));
        QVERIFY(QDir{}.mkpath(gallery));
        const auto outside = directory.filePath(QStringLiteral("outside.jpg"));
        QFile outside_file{outside};
        QVERIFY(outside_file.open(QIODevice::WriteOnly));
        QVERIFY(outside_file.write("private") == 7);
        outside_file.close();

        pms::desktop::DeviceConnector connector;
        const auto device = pms::desktop::DeviceConnector::folderDeviceId(QUrl::fromLocalFile(gallery));
        QVERIFY(!device.isEmpty());
        QVERIFY(connector.preview(device, QStringLiteral("../outside.jpg"), 1024).isEmpty());
        QVERIFY(connector.sha256(device, QStringLiteral("../outside.jpg")).isEmpty());
        QVERIFY(!connector.copyTo(device, QStringLiteral("../outside.jpg"), directory.filePath(QStringLiteral("copy.jpg"))));
        QVERIFY(!connector.remove(device, QStringLiteral("../outside.jpg")));
        QVERIFY(QFileInfo::exists(outside));
    }
};

QTEST_MAIN(DeviceConnectorTests)
#include "device_connector_tests.moc"
