#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <limits>

#include "platform/key_store.hpp"
#include "vault/vault.hpp"

using pms::desktop::Vault;
using pms::desktop::VaultEntry;

namespace {

constexpr qint64 payload_header = 48;  // magic 8 | version 4 | size 8 | nonce 12 | tag 16

QByteArray random_bytes(const qsizetype size) {
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator generator{0x5eed};
    for (auto& byte : bytes) {
        byte = static_cast<char>(generator.bounded(256));
    }
    return bytes;
}

QString write_file(const QString& path, const QByteArray& contents) {
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return {};
    }
    return path;
}

QByteArray read_file(const QString& path) {
    QFile file{path};
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

void flip_byte(const QString& path, const qint64 offset) {
    QFile file{path};
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.seek(offset));
    const auto byte = file.read(1);
    QCOMPARE(byte.size(), 1);
    QVERIFY(file.seek(offset));
    QCOMPARE(file.write(QByteArray(1, static_cast<char>(byte[0] ^ 0x01))), qint64{1});
}

QString payload(const QTemporaryDir& directory, const VaultEntry& entry) {
    return directory.filePath(QStringLiteral("vault/") + entry.id + QStringLiteral(".pmsv"));
}

}  // namespace

class VaultTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void keyStoreKeepsOneKeyPerName() {
        QString error;
        const auto first = pms::desktop::KeyStore::loadOrCreate(QStringLiteral("vault-test-key"), &error);
        QVERIFY2(first.has_value(), qPrintable(error));
        QCOMPARE(first->size(), 32);
        const auto second = pms::desktop::KeyStore::loadOrCreate(QStringLiteral("vault-test-key"), &error);
        QVERIFY(second.has_value());
        QCOMPARE(*second, *first);
        QVERIFY(!pms::desktop::KeyStore::loadOrCreate(QStringLiteral("../escape"), &error).has_value());
        QVERIFY(!pms::desktop::KeyStore::loadOrCreate(QString{}, &error).has_value());
    }

    // NIST GCM spec test case 16 (AES-256, 60-byte plaintext, 20-byte AAD), fed 16 bytes at a time so the
    // chained-call path and the AAD placement are checked against the standard, not just against each other.
    void gcmStreamMatchesTheNistVector() {
        const auto key = QByteArray::fromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        const auto nonce = QByteArray::fromHex("cafebabefacedbaddecaf888");
        const auto plain = QByteArray::fromHex(
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        const auto aad = QByteArray::fromHex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        const auto cipher = QByteArray::fromHex(
            "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
            "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
        const auto expected_tag = QByteArray::fromHex("76fc6ece0f4e1768cddf8853bb2d551b");

        for (const qint64 chunk : {qint64{16}, pms::desktop::detail::gcm_chunk_bytes}) {
            QBuffer input;
            input.setData(plain);
            QVERIFY(input.open(QIODevice::ReadOnly));
            QBuffer output;
            QVERIFY(output.open(QIODevice::WriteOnly));
            QByteArray tag(16, '\0');
            QCryptographicHash hash{QCryptographicHash::Sha256};
            QVERIFY(pms::desktop::detail::aes_gcm_stream(input, output, key, nonce, aad, tag, true, chunk, &hash));
            QCOMPARE(output.data().toHex(), cipher.toHex());
            QCOMPARE(tag.toHex(), expected_tag.toHex());
            QCOMPARE(hash.result(), QCryptographicHash::hash(plain, QCryptographicHash::Sha256));

            QBuffer back_in;
            back_in.setData(cipher);
            QVERIFY(back_in.open(QIODevice::ReadOnly));
            QBuffer back_out;
            QVERIFY(back_out.open(QIODevice::WriteOnly));
            QVERIFY(pms::desktop::detail::aes_gcm_stream(back_in, back_out, key, nonce, aad, tag, false, chunk));
            QCOMPARE(back_out.data(), plain);

            QBuffer forged_in;
            forged_in.setData(cipher);
            QVERIFY(forged_in.open(QIODevice::ReadOnly));
            QBuffer forged_out;
            QVERIFY(forged_out.open(QIODevice::WriteOnly));
            auto other_aad = aad;
            other_aad[0] = static_cast<char>(other_aad[0] ^ 0x01);
            QVERIFY(!pms::desktop::detail::aes_gcm_stream(forged_in, forged_out, key, nonce, other_aad, tag, false, chunk));
        }
    }

    void roundTripsAcrossChunksWithoutOverwriting() {
        QTemporaryDir directory;
        const auto contents = random_bytes(2 * pms::desktop::detail::gcm_chunk_bytes + 37);
        const auto source = write_file(directory.filePath(QStringLiteral("IMG_0007.JPG")), contents);
        QVERIFY(!source.isEmpty());

        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QString error;
        QVERIFY2(vault.open(&error), qPrintable(error));
        const auto entry = vault.archive(source, QStringLiteral("usb:pixel"), QStringLiteral("obj-7"),
                                         QStringLiteral("IMG_0007.JPG"), &error);
        QVERIFY2(entry.has_value(), qPrintable(error));
        QCOMPARE(entry->bytes, quint64(contents.size()));
        QCOMPARE(entry->sha256, QCryptographicHash::hash(contents, QCryptographicHash::Sha256));
        QCOMPARE(entry->source_key, QStringLiteral("usb:pixel"));
        QCOMPARE(entry->source_id, QStringLiteral("obj-7"));
        QVERIFY(entry->archived_ms > 0);
        QCOMPARE(vault.bytesUsed(), quint64(contents.size() + payload_header));
        QVERIFY(QFile::remove(source));

        Vault reopened{directory.filePath(QStringLiteral("vault"))};
        QVERIFY2(reopened.open(&error), qPrintable(error));
        QCOMPARE(reopened.entries().size(), 1);
        QCOMPARE(reopened.entries().front().sha256, entry->sha256);

        const auto restore_dir = directory.filePath(QStringLiteral("restored"));
        QVERIFY(QDir{}.mkpath(restore_dir));
        QString restored;
        QVERIFY2(reopened.restore(entry->id, restore_dir, &restored, &error), qPrintable(error));
        QCOMPARE(QFileInfo{restored}.fileName(), QStringLiteral("IMG_0007.JPG"));
        QCOMPARE(read_file(restored), contents);

        QString second;
        QVERIFY(reopened.restore(entry->id, restore_dir, &second, &error));
        QVERIFY(second != restored);  // an existing file is never overwritten
        QCOMPARE(QFileInfo{second}.fileName(), QStringLiteral("IMG_0007 2.JPG"));
        QCOMPARE(read_file(second), contents);
        QCOMPARE(read_file(restored), contents);
    }

    void roundTripsAnEmptyFile() {
        QTemporaryDir directory;
        const auto source = write_file(directory.filePath(QStringLiteral("empty.jpg")), QByteArray{});
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open(nullptr));
        QString error;
        const auto entry = vault.archive(source, QStringLiteral("s"), QStringLiteral("e"), QStringLiteral("empty.jpg"), &error);
        QVERIFY2(entry.has_value(), qPrintable(error));
        QString restored;
        QVERIFY2(vault.restore(entry->id, directory.filePath(QStringLiteral("out")), &restored, &error), qPrintable(error));
        QCOMPARE(QFileInfo{restored}.size(), qint64{0});
    }

    void anyChangedPayloadByteFailsRestore() {
        QTemporaryDir directory;
        const auto contents = random_bytes(4096);
        const auto source = write_file(directory.filePath(QStringLiteral("a.jpg")), contents);
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open(nullptr));
        const auto entry = vault.archive(source, QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("a.jpg"), nullptr);
        QVERIFY(entry.has_value());
        const auto path = payload(directory, *entry);
        const auto out = directory.filePath(QStringLiteral("out"));
        QVERIFY(QDir{}.mkpath(out));

        // Every header byte (magic, version, size, nonce), every tag byte, and ciphertext at both ends and the middle.
        QList<qint64> offsets;
        for (qint64 offset = 0; offset < payload_header; ++offset) {
            offsets.push_back(offset);
        }
        offsets << payload_header << payload_header + 2048 << payload_header + 4095;
        for (const auto offset : offsets) {
            flip_byte(path, offset);
            QString restored;
            QString error;
            QVERIFY2(!vault.restore(entry->id, out, &restored, &error), qPrintable(QString::number(offset)));
            QVERIFY(!error.isEmpty());
            QVERIFY2(QDir{out}.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                     "a failed restore left a file behind");
            flip_byte(path, offset);  // put it back
        }
        QString restored;
        QVERIFY(vault.restore(entry->id, out, &restored, nullptr));
        QCOMPARE(read_file(restored), contents);
    }

    void swappedPayloadsFailRestore() {
        QTemporaryDir directory;
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open(nullptr));
        // Same size, so only the id bound into the AAD tells them apart.
        const auto a = vault.archive(write_file(directory.filePath(QStringLiteral("a")), QByteArray(512, 'a')),
                                     QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("a.jpg"), nullptr);
        const auto b = vault.archive(write_file(directory.filePath(QStringLiteral("b")), QByteArray(512, 'b')),
                                     QStringLiteral("s"), QStringLiteral("b"), QStringLiteral("b.jpg"), nullptr);
        QVERIFY(a && b);
        const auto path_a = payload(directory, *a);
        const auto path_b = payload(directory, *b);
        const auto swap = directory.filePath(QStringLiteral("swap"));
        QVERIFY(QFile::rename(path_a, swap) && QFile::rename(path_b, path_a) && QFile::rename(swap, path_b));

        const auto out = directory.filePath(QStringLiteral("out"));
        QString restored;
        QVERIFY(!vault.restore(a->id, out, &restored, nullptr));
        QVERIFY(!vault.restore(b->id, out, &restored, nullptr));
        QVERIFY(QDir{out}.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot));
    }

    void editedManifestFailsOpen() {
        QTemporaryDir directory;
        const auto vault_dir = directory.filePath(QStringLiteral("vault"));
        {
            Vault vault{vault_dir};
            QVERIFY(vault.open(nullptr));
            QVERIFY(vault.archive(write_file(directory.filePath(QStringLiteral("a")), "abc"), QStringLiteral("s"),
                                  QStringLiteral("a"), QStringLiteral("a.jpg"), nullptr));
        }
        const auto manifest = QDir{vault_dir}.filePath(QStringLiteral("manifest.pmsm"));
        const auto original = read_file(manifest);
        QVERIFY(original.size() > payload_header);
        for (const qint64 offset : {qint64{0}, qint64{10}, qint64{25}, qint64{40}, original.size() - 1}) {
            flip_byte(manifest, offset);
            Vault vault{vault_dir};
            QString error;
            QVERIFY2(!vault.open(&error), qPrintable(QString::number(offset)));
            QVERIFY(!error.isEmpty());
            QVERIFY(!vault.archive(write_file(directory.filePath(QStringLiteral("b")), "b"), QStringLiteral("s"),
                                   QStringLiteral("b"), QStringLiteral("b"), nullptr));
            flip_byte(manifest, offset);
        }
        QVERIFY(write_file(manifest, original.left(original.size() - 1)).size() > 0);  // truncated
        Vault truncated{vault_dir};
        QVERIFY(!truncated.open(nullptr));
        QVERIFY(write_file(manifest, original).size() > 0);
        Vault restored{vault_dir};
        QVERIFY(restored.open(nullptr));
        QCOMPARE(restored.entries().size(), 1);
    }

    void purgeRemovesOnlyOldEntries() {
        QTemporaryDir directory;
        const auto vault_dir = directory.filePath(QStringLiteral("vault"));
        Vault vault{vault_dir};
        QVERIFY(vault.open(nullptr));
        QList<VaultEntry> archived;
        for (int index = 0; index < 3; ++index) {
            const auto name = QStringLiteral("f%1").arg(index);
            const auto entry = vault.archive(write_file(directory.filePath(name), name.toUtf8()), QStringLiteral("s"),
                                             name, name + QStringLiteral(".jpg"), nullptr);
            QVERIFY(entry.has_value());
            archived.push_back(*entry);
            QThread::msleep(5);  // distinct archive times
        }
        QVERIFY(archived[0].archived_ms < archived[1].archived_ms && archived[1].archived_ms < archived[2].archived_ms);

        QCOMPARE(vault.purgeOlderThan(archived[0].archived_ms), 0);
        QCOMPARE(vault.purgeOlderThan(archived[2].archived_ms), 2);
        QCOMPARE(vault.entries().size(), 1);
        QCOMPARE(vault.entries().front().id, archived[2].id);
        QVERIFY(!QFile::exists(payload(directory, archived[0])));
        QVERIFY(!QFile::exists(payload(directory, archived[1])));
        QVERIFY(QFile::exists(payload(directory, archived[2])));

        Vault reopened{vault_dir};
        QVERIFY(reopened.open(nullptr));
        QCOMPARE(reopened.entries().size(), 1);
        QString restored;
        QVERIFY(reopened.restore(archived[2].id, directory.filePath(QStringLiteral("out")), &restored, nullptr));
        QCOMPARE(read_file(restored), QByteArray{"f2"});
        QVERIFY(!reopened.restore(archived[0].id, directory.filePath(QStringLiteral("out")), &restored, nullptr));
    }

    void hasRoomForKeepsTenPercentHeadroom() {
        using pms::desktop::has_room;
        QVERIFY(has_room(110, 100));
        QVERIFY(!has_room(109, 100));
        QVERIFY(has_room(11, 10));
        QVERIFY(!has_room(11, 11));
        QVERIFY(!has_room(10, 10));
        QVERIFY(has_room(0, 0));
        QVERIFY(has_room(1, 1) == false);
        constexpr auto max = std::numeric_limits<quint64>::max();
        QVERIFY(!has_room(max, max));
        QVERIFY(has_room(max, max / 11 * 10));
        QVERIFY(!has_room(max / 2, max));

        QTemporaryDir directory;
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open(nullptr));
        QVERIFY(vault.hasRoomFor(1));
        QVERIFY(!vault.hasRoomFor(max / 2));
    }

    void neverLeavesPlaintextInTheVault() {
        QTemporaryDir directory;
        const auto marker = QByteArray("PLAINTEXT-MARKER-") + random_bytes(64).toHex();
        const auto contents = marker + QByteArray(100000, 'x') + marker;
        const auto source = write_file(directory.filePath(QStringLiteral("secret.jpg")), contents);
        const auto vault_dir = directory.filePath(QStringLiteral("vault"));
        Vault vault{vault_dir};
        QVERIFY(vault.open(nullptr));
        const auto entry = vault.archive(source, QStringLiteral("s"), QStringLiteral("x"), QStringLiteral("secret.jpg"), nullptr);
        QVERIFY(entry.has_value());

        const auto files = QDir{vault_dir}.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        QCOMPARE(files, (QStringList{entry->id + QStringLiteral(".pmsv"), QStringLiteral("manifest.pmsm")}));
        QVERIFY(QDir{vault_dir}.entryList({QStringLiteral("verify-*")}, QDir::Files).isEmpty());
        QVERIFY(!read_file(payload(directory, *entry)).contains(marker));
        QVERIFY(!read_file(QDir{vault_dir}.filePath(QStringLiteral("manifest.pmsm"))).contains("secret.jpg"));
    }

    void restoreStaysInsideTheDestination() {
        QTemporaryDir directory;
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QVERIFY(vault.open(nullptr));
        const auto out = directory.filePath(QStringLiteral("out"));
        for (const auto& name : {QStringLiteral("../../evil.jpg"), QStringLiteral("..\\..\\evil.jpg"),
                                 QStringLiteral("a/b:c*?.jpg"), QStringLiteral(".."), QStringLiteral("NUL.jpg")}) {
            const auto entry = vault.archive(write_file(directory.filePath(QStringLiteral("src")), "data"), QStringLiteral("s"),
                                             QStringLiteral("id"), name, nullptr);
            QVERIFY(entry.has_value());
            QString restored;
            QString error;
            QVERIFY2(vault.restore(entry->id, out, &restored, &error), qPrintable(name + QStringLiteral(": ") + error));
            QCOMPARE(QFileInfo{restored}.absolutePath(), QFileInfo{out}.absoluteFilePath());
            QCOMPARE(read_file(restored), QByteArray{"data"});
        }
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("evil.jpg"))));
    }

    void refusesWorkBeforeOpenAndMissingFiles() {
        QTemporaryDir directory;
        Vault vault{directory.filePath(QStringLiteral("vault"))};
        QString error;
        QVERIFY(!vault.archive(write_file(directory.filePath(QStringLiteral("a")), "a"), QStringLiteral("s"),
                               QStringLiteral("a"), QStringLiteral("a"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(vault.open(nullptr));
        QVERIFY(!vault.archive(directory.filePath(QStringLiteral("missing")), QStringLiteral("s"), QStringLiteral("m"),
                               QStringLiteral("m"), &error));
        QString restored;
        QVERIFY(!vault.restore(QStringLiteral("not-an-entry"), directory.path(), &restored, &error));
        QVERIFY(vault.entries().isEmpty());
    }
};

QTEST_MAIN(VaultTests)
#include "vault_tests.moc"
