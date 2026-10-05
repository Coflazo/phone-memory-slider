#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "recovery_vault.hpp"

class RecoveryVaultTests final : public QObject {
    Q_OBJECT

private slots:
    void encryptsVerifiesAndRestoresAcrossChunks() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath(QStringLiteral("source.bin"));
        const QByteArray contents(5 * 1024 * 1024 + 37, '\x5a');
        QFile source_file{source};
        QVERIFY(source_file.open(QIODevice::WriteOnly));
        QCOMPARE(source_file.write(contents), contents.size());
        source_file.close();

        pms::desktop::RecoveryVault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY2(vault.open(), qPrintable(vault.errorString()));
        pms::desktop::VaultEntry entry;
        QVERIFY2(vault.archive(source, QStringLiteral("asset-7"), QStringLiteral("IMG_0007.JPG"), entry),
                 qPrintable(vault.errorString()));
        QVERIFY(QFileInfo::exists(entry.encryptedPath));
        QCOMPARE(QDir{directory.filePath(QStringLiteral("vault"))}
                     .entryList({QStringLiteral("verify-*")}, QDir::Files)
                     .size(),
                 0);
        QVERIFY(QFile::remove(source));

        const auto restored = directory.filePath(QStringLiteral("restored.bin"));
        QVERIFY2(vault.restore(entry, restored), qPrintable(vault.errorString()));
        QFile restored_file{restored};
        QVERIFY(restored_file.open(QIODevice::ReadOnly));
        QCOMPARE(restored_file.readAll(), contents);
    }

    void rejectsTamperedCiphertext() {
        QTemporaryDir directory;
        const auto source = directory.filePath(QStringLiteral("source.bin"));
        QFile source_file{source};
        QVERIFY(source_file.open(QIODevice::WriteOnly));
        QVERIFY(source_file.write(QByteArray(4096, '\x2a')) == 4096);
        source_file.close();

        pms::desktop::RecoveryVault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open());
        pms::desktop::VaultEntry entry;
        QVERIFY(vault.archive(source, QStringLiteral("asset-8"), QStringLiteral("IMG_0008.JPG"), entry));
        QFile encrypted{entry.encryptedPath};
        QVERIFY(encrypted.open(QIODevice::ReadWrite));
        QVERIFY(encrypted.seek(encrypted.size() - 1));
        const auto byte = encrypted.read(1);
        QVERIFY(encrypted.seek(encrypted.size() - 1));
        QVERIFY(encrypted.write(QByteArray{1, static_cast<char>(byte.front() ^ 0x01)}) == 1);
        encrypted.close();

        QVERIFY(!vault.restore(entry, directory.filePath(QStringLiteral("must-not-exist.bin"))));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("must-not-exist.bin"))));
    }

    void repeatedAssetIdsKeepIndependentRecoveryCopies() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto source = directory.filePath(QStringLiteral("source.bin"));
        pms::desktop::RecoveryVault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open());

        pms::desktop::VaultEntry first;
        pms::desktop::VaultEntry second;
        for (const auto& item : std::initializer_list<std::pair<QByteArray, pms::desktop::VaultEntry*>>{
                 {QByteArray{"first-version"}, &first},
                 {QByteArray{"second-version"}, &second}}) {
            QFile file{source};
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(item.first), item.first.size());
            file.close();
            QVERIFY2(vault.archive(source, QStringLiteral("same-asset-id"), QStringLiteral("photo.jpg"), *item.second),
                     qPrintable(vault.errorString()));
        }

        QVERIFY(first.id != second.id);
        QVERIFY(first.encryptedPath != second.encryptedPath);
        const auto first_restore = directory.filePath(QStringLiteral("first.bin"));
        const auto second_restore = directory.filePath(QStringLiteral("second.bin"));
        QVERIFY(vault.restore(first, first_restore));
        QVERIFY(vault.restore(second, second_restore));
        QFile first_file{first_restore};
        QFile second_file{second_restore};
        QVERIFY(first_file.open(QIODevice::ReadOnly));
        QVERIFY(second_file.open(QIODevice::ReadOnly));
        QCOMPARE(first_file.readAll(), QByteArray{"first-version"});
        QCOMPARE(second_file.readAll(), QByteArray{"second-version"});
    }
};

QTEST_MAIN(RecoveryVaultTests)
#include "recovery_vault_tests.moc"
