#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "platform/key_store.hpp"
#include "vault/vault.hpp"

using pms::desktop::ArchiveRequest;
using pms::desktop::Vault;
using pms::desktop::VaultEntry;

namespace {

constexpr qint64 payload_header = 48;          // magic 8 | version 4 | size 8 | nonce 12 | tag 16
constexpr qint64 journal_header = 28;          // magic 8 | version 4 | journal id 16
constexpr qint64 record_fixed = 4 + 1 + 36 + 12 + 16;  // length | id length | id | nonce | tag

QByteArray random_bytes(const qsizetype size, const quint32 seed = 0x5eed) {
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator generator{seed};
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

QByteArray vault_key() {
    QString error;
    return pms::desktop::KeyStore::loadOrCreate(QStringLiteral("vault"), &error).value_or(QByteArray{});
}

// [begin, end) of every record in a journal file.
QList<std::pair<qint64, qint64>> record_spans(const QByteArray& journal) {
    QList<std::pair<qint64, qint64>> spans;
    qint64 offset = journal_header;
    while (offset + 4 <= journal.size()) {
        const auto length = qFromLittleEndian<quint32>(journal.constData() + offset);
        const auto end = offset + record_fixed + static_cast<qint64>(length);
        spans.push_back({offset, end});
        offset = end;
    }
    return spans;
}

bool empty_dir(const QString& path) {
    return QDir{path}.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

}  // namespace

class VaultTests final : public QObject {
    Q_OBJECT

    QTemporaryDir directory_;
    QString vault_dir_;
    QString sources_;

    // Archives `count` small files (one journal append each) and closes the vault.
    QList<VaultEntry> archive_files(const int count) {
        QList<VaultEntry> entries;
        Vault vault{vault_dir_};
        if (!vault.open(nullptr)) {
            return entries;
        }
        for (int index = 0; index < count; ++index) {
            const auto name = QStringLiteral("IMG_%1.JPG").arg(index);
            const auto entry = vault.archive(write_file(QDir{sources_}.filePath(name), random_bytes(300 + index, index)),
                                             QStringLiteral("usb:pixel"), QStringLiteral("obj-%1").arg(index), name, nullptr);
            if (entry) {
                entries.push_back(*entry);
            }
        }
        return entries;
    }

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QVERIFY(directory_.isValid());
        vault_dir_ = directory_.filePath(QUuid::createUuid().toString(QUuid::Id128));
        sources_ = directory_.filePath(QUuid::createUuid().toString(QUuid::Id128));
        QVERIFY(QDir{}.mkpath(sources_));
    }

    void keyStoreKeepsOneKeyPerName() {
        QString error;
        const auto first = pms::desktop::KeyStore::loadOrCreate(QStringLiteral("vault-test-key"), &error);
        QVERIFY2(first.has_value(), qPrintable(error));
        QCOMPARE(first->size(), 32);
        QVERIFY(pms::desktop::KeyStore::backend() != pms::desktop::KeyBackend::None);
#ifdef Q_OS_WIN
        QCOMPARE(pms::desktop::KeyStore::backend(), pms::desktop::KeyBackend::Dpapi);
#endif
        const auto second = pms::desktop::KeyStore::loadOrCreate(QStringLiteral("vault-test-key"), &error);
        QVERIFY(second.has_value());
        QCOMPARE(*second, *first);
        QVERIFY(!pms::desktop::KeyStore::loadOrCreate(QStringLiteral("../escape"), &error).has_value());
        QVERIFY(!pms::desktop::KeyStore::loadOrCreate(QString{}, &error).has_value());
    }

    // R1: first launches racing to create a key all end up with the same one.
    void racingFirstLaunchesShareOneKey() {
        const auto name = QStringLiteral("race-") + QUuid::createUuid().toString(QUuid::Id128);
        constexpr int racers = 8;
        std::vector<QByteArray> keys(racers);
        std::vector<std::thread> threads;
        for (int index = 0; index < racers; ++index) {
            threads.emplace_back([&keys, &name, index] {
                keys[static_cast<std::size_t>(index)] =
                    pms::desktop::KeyStore::loadOrCreate(name, nullptr).value_or(QByteArray{});
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        QCOMPARE(keys.front().size(), 32);
        for (const auto& key : keys) {
            QCOMPARE(key, keys.front());
        }
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
        const auto contents = random_bytes(2 * pms::desktop::detail::gcm_chunk_bytes + 37);
        const auto source = write_file(QDir{sources_}.filePath(QStringLiteral("IMG_0007.JPG")), contents);
        QVERIFY(!source.isEmpty());

        QString error;
        std::optional<VaultEntry> entry;
        {
            Vault vault{vault_dir_};
            QVERIFY2(vault.open(&error), qPrintable(error));
            entry = vault.archive(source, QStringLiteral("usb:pixel"), QStringLiteral("obj-7"), QStringLiteral("IMG_0007.JPG"),
                                  &error);
            QVERIFY2(entry.has_value(), qPrintable(error));
            QCOMPARE(entry->bytes, quint64(contents.size()));
            QCOMPARE(entry->sha256, QCryptographicHash::hash(contents, QCryptographicHash::Sha256));
            QCOMPARE(entry->source_key, QStringLiteral("usb:pixel"));
            QCOMPARE(entry->source_id, QStringLiteral("obj-7"));
            QVERIFY(entry->archived_ms > 0);
            QCOMPARE(vault.bytesUsed(), quint64(contents.size() + payload_header));

            // R1: one vault folder, one open instance.
            Vault second{vault_dir_};
            QVERIFY(!second.open(&error));
            QVERIFY2(error.contains(QStringLiteral("another window")), qPrintable(error));
            QVERIFY(!second.needsRecovery());
        }
        QVERIFY(QFile::remove(source));

        Vault reopened{vault_dir_};
        QVERIFY2(reopened.open(&error), qPrintable(error));
        QCOMPARE(reopened.entries().size(), 1);
        QCOMPARE(reopened.entries().front().sha256, entry->sha256);

        const auto restore_dir = directory_.filePath(QStringLiteral("restored-") + QUuid::createUuid().toString(QUuid::Id128));
        QString restored;
        QVERIFY2(reopened.restore(entry->id, restore_dir, &restored, &error), qPrintable(error));
        QCOMPARE(QFileInfo{restored}.fileName(), QStringLiteral("IMG_0007.JPG"));
        QCOMPARE(read_file(restored), contents);

        QString second;
        QVERIFY(reopened.restore(entry->id, restore_dir, &second, &error));
        QCOMPARE(QFileInfo{second}.fileName(), QStringLiteral("IMG_0007 2.JPG"));  // never overwrites
        QCOMPARE(read_file(second), contents);
        QCOMPARE(read_file(restored), contents);
    }

    void roundTripsAnEmptyFile() {
        const auto source = write_file(QDir{sources_}.filePath(QStringLiteral("empty.jpg")), QByteArray{});
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        QString error;
        const auto entry = vault.archive(source, QStringLiteral("s"), QStringLiteral("e"), QStringLiteral("empty.jpg"), &error);
        QVERIFY2(entry.has_value(), qPrintable(error));
        QString restored;
        QVERIFY2(vault.restore(entry->id, directory_.filePath(QStringLiteral("empty-out")), &restored, &error), qPrintable(error));
        QCOMPARE(QFileInfo{restored}.size(), qint64{0});
    }

    void batchArchiveReportsEachItem() {
        QList<VaultEntry> archived;
        {
            Vault vault{vault_dir_};
            QVERIFY(vault.open(nullptr));
            const auto results = vault.archiveBatch({
                {write_file(QDir{sources_}.filePath(QStringLiteral("a")), "a"), QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("a.jpg")},
                {QDir{sources_}.filePath(QStringLiteral("missing")), QStringLiteral("s"), QStringLiteral("m"), QStringLiteral("m.jpg")},
                {write_file(QDir{sources_}.filePath(QStringLiteral("c")), "c"), QStringLiteral("s"), QStringLiteral("c"), QStringLiteral("c.jpg")},
            });
            QCOMPARE(results.size(), 3);
            QVERIFY(results[0].entry && results[0].error.isEmpty());
            QVERIFY(!results[1].entry && !results[1].error.isEmpty());
            QVERIFY(results[2].entry && results[2].error.isEmpty());
            QCOMPARE(vault.entries().size(), 2);
            archived = vault.entries();
        }
        QCOMPARE(record_spans(read_file(QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj")))).size(), 2);
        Vault reopened{vault_dir_};
        QVERIFY(reopened.open(nullptr));
        QCOMPARE(reopened.entries().size(), 2);
        QCOMPARE(reopened.entries()[1].id, archived[1].id);
    }

    void anyChangedPayloadByteFailsRestore() {
        const auto contents = random_bytes(4096);
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        const auto entry = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("a.jpg")), contents),
                                         QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("a.jpg"), nullptr);
        QVERIFY(entry.has_value());
        const auto path = QDir{vault_dir_}.filePath(entry->id + QStringLiteral(".pmsv"));
        const auto out = directory_.filePath(QStringLiteral("tamper-out"));
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
            QVERIFY2(error.contains(QStringLiteral("integrity")), qPrintable(error));
            QVERIFY2(empty_dir(out), "a failed restore left a file behind");
            flip_byte(path, offset);  // put it back
        }
        QString restored;
        QVERIFY(vault.restore(entry->id, out, &restored, nullptr));
        QCOMPARE(read_file(restored), contents);
    }

    // M1: identical contents, so size and SHA-256 match either way; only the id bound into the AAD tells them apart.
    void swappedIdenticalPayloadsFailRestore() {
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        const QByteArray same(512, 's');
        const auto a = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("a")), same), QStringLiteral("s"),
                                     QStringLiteral("a"), QStringLiteral("a.jpg"), nullptr);
        const auto b = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("b")), same), QStringLiteral("s"),
                                     QStringLiteral("b"), QStringLiteral("b.jpg"), nullptr);
        QVERIFY(a && b);
        QCOMPARE(a->sha256, b->sha256);
        const auto path_a = QDir{vault_dir_}.filePath(a->id + QStringLiteral(".pmsv"));
        const auto path_b = QDir{vault_dir_}.filePath(b->id + QStringLiteral(".pmsv"));
        const auto swap = QDir{vault_dir_}.filePath(QStringLiteral("swap"));
        QVERIFY(QFile::rename(path_a, swap) && QFile::rename(path_b, path_a) && QFile::rename(swap, path_b));

        const auto out = directory_.filePath(QStringLiteral("swap-out"));
        QString restored;
        QVERIFY(!vault.restore(a->id, out, &restored, nullptr));
        QVERIFY(!vault.restore(b->id, out, &restored, nullptr));
        QVERIFY(empty_dir(out));
        QVERIFY(QFile::rename(path_a, swap) && QFile::rename(path_b, path_a) && QFile::rename(swap, path_b));
        QVERIFY(vault.restore(a->id, out, &restored, nullptr));
    }

    // M1: the payload's 32 header bytes are part of the AAD, not only checked field by field.
    void payloadHeaderIsInTheAad() {
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        const auto contents = random_bytes(1000);
        const auto entry = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("h")), contents),
                                         QStringLiteral("s"), QStringLiteral("h"), QStringLiteral("h.jpg"), nullptr);
        QVERIFY(entry.has_value());
        const auto file = read_file(QDir{vault_dir_}.filePath(entry->id + QStringLiteral(".pmsv")));
        const auto header = file.left(32);
        const auto nonce = file.mid(20, 12);
        const auto key = vault_key();
        QCOMPARE(key.size(), 32);
        const auto decrypt = [&](const QByteArray& aad, QByteArray* plain) {
            QBuffer input;
            input.setData(file.mid(payload_header));
            input.open(QIODevice::ReadOnly);
            QBuffer output;
            output.open(QIODevice::WriteOnly);
            auto tag = file.mid(32, 16);
            const auto ok = pms::desktop::detail::aes_gcm_stream(input, output, key, nonce, aad, tag, false);
            *plain = output.data();
            return ok;
        };
        QByteArray plain;
        QVERIFY(decrypt(header + entry->id.toLatin1(), &plain));
        QCOMPARE(plain, contents);
        QVERIFY(!decrypt(entry->id.toLatin1(), &plain));  // without the header
        QVERIFY(!decrypt(header, &plain));                // without the id
    }

    void journalOpensTenThousandEntriesFast() {
        QList<VaultEntry> entries;
        for (int index = 0; index < 10000; ++index) {
            VaultEntry entry;
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry.source_key = QStringLiteral("usb:pixel");
            entry.source_id = QStringLiteral("object-%1").arg(index);
            entry.original_name = QStringLiteral("IMG_%1.JPG").arg(index);
            entry.bytes = 4'000'000;
            entry.sha256 = QByteArray(32, static_cast<char>(index));
            entry.archived_ms = 1'700'000'000'000 + index;
            entries.push_back(entry);
        }
        QVERIFY(QDir{}.mkpath(vault_dir_));
        QString error;
        QVERIFY2(pms::desktop::detail::write_journal(QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj")), vault_key(),
                                                     entries, error),
                 qPrintable(error));
        QElapsedTimer timer;
        timer.start();
        Vault vault{vault_dir_};
        QVERIFY2(vault.open(&error), qPrintable(error));
        const auto elapsed = timer.elapsed();
        qInfo() << "opened a 10000-entry journal in" << elapsed << "ms";
        QCOMPARE(vault.entries().size(), 10000);
        QCOMPARE(vault.entries().back().original_name, QStringLiteral("IMG_9999.JPG"));
        QVERIFY2(elapsed < 5000, qPrintable(QString::number(elapsed)));
    }

    void tamperedRecordKeepsThePrefixAndRecoversTheRest() {
        const auto archived = archive_files(4);
        QCOMPARE(archived.size(), 4);
        const auto journal = QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj"));
        const auto spans = record_spans(read_file(journal));
        QCOMPARE(spans.size(), 4);
        flip_byte(journal, spans[2].second - 1);  // last ciphertext byte of record 2

        Vault vault{vault_dir_};
        QString error;
        QVERIFY(!vault.open(&error));
        QVERIFY(vault.needsRecovery());
        QVERIFY2(error.contains(QStringLiteral("record 2")), qPrintable(error));
        QVERIFY(!vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("x")), "x"), QStringLiteral("s"),
                               QStringLiteral("x"), QStringLiteral("x"), nullptr));
        int recovered = 0;
        QVERIFY2(vault.recoverIndex(&recovered, &error), qPrintable(error));
        QCOMPARE(recovered, 2);
        const auto entries = vault.entries();
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries[0].original_name, archived[0].original_name);
        QCOMPARE(entries[1].original_name, archived[1].original_name);
        for (int index = 2; index < 4; ++index) {
            QVERIFY(entries[index].original_name.startsWith(QStringLiteral("restored-")));
        }
        QString restored;
        QVERIFY(vault.restore(archived[3].id, directory_.filePath(QStringLiteral("recovered-out")), &restored, &error));
        QCOMPARE(read_file(restored), random_bytes(303, 3));
    }

    void reorderedRecordsAreDetected() {
        QCOMPARE(archive_files(3).size(), 3);
        const auto journal = QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj"));
        const auto bytes = read_file(journal);
        const auto spans = record_spans(bytes);
        const auto record = [&bytes](const std::pair<qint64, qint64>& span) {
            return bytes.mid(span.first, span.second - span.first);
        };
        QVERIFY(!write_file(journal, bytes.left(journal_header) + record(spans[0]) + record(spans[2]) + record(spans[1]))
                     .isEmpty());
        Vault vault{vault_dir_};
        QString error;
        QVERIFY(!vault.open(&error));
        QVERIFY(vault.needsRecovery());
        QVERIFY2(error.contains(QStringLiteral("record 1")), qPrintable(error));
        int recovered = 0;
        QVERIFY(vault.recoverIndex(&recovered, nullptr));
        QCOMPARE(recovered, 2);
        QCOMPARE(vault.entries().size(), 3);
    }

    void truncatedJournal() {
        QCOMPARE(archive_files(4).size(), 4);
        const auto journal = QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj"));
        const auto bytes = read_file(journal);
        const auto spans = record_spans(bytes);

        // Cut inside the last record: detected.
        QVERIFY(!write_file(journal, bytes.left(spans[3].first + 10)).isEmpty());
        {
            Vault vault{vault_dir_};
            QString error;
            QVERIFY(!vault.open(&error));
            QVERIFY(vault.needsRecovery());
            QVERIFY2(error.contains(QStringLiteral("cut off")), qPrintable(error));
        }
        // Cut at a record boundary: the journal is valid with 3 entries (rollback is out of scope); the fourth
        // payload is unindexed but younger than a day, so it is kept for recovery.
        QVERIFY(!write_file(journal, bytes.left(spans[3].first)).isEmpty());
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        QCOMPARE(vault.entries().size(), 3);
        QCOMPARE(QDir{vault_dir_}.entryList({QStringLiteral("*.pmsv")}, QDir::Files).size(), 4);
    }

    void missingJournalWithPayloadsIsRecoverable() {
        const auto archived = archive_files(2);
        QCOMPARE(archived.size(), 2);
        QVERIFY(QFile::remove(QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj"))));
        {
            Vault vault{vault_dir_};
            QString error;
            QVERIFY(!vault.open(&error));
            QVERIFY(vault.needsRecovery());
            QVERIFY2(error.contains(QStringLiteral("2 encrypted files")), qPrintable(error));
            QVERIFY(vault.entries().isEmpty());
            QString restored;
            QVERIFY(!vault.restore(archived[0].id, directory_.filePath(QStringLiteral("x")), &restored, &error));
            int recovered = 0;
            QVERIFY2(vault.recoverIndex(&recovered, &error), qPrintable(error));
            QCOMPARE(recovered, 2);
            for (const auto& entry : vault.entries()) {
                QCOMPARE(entry.original_name, QStringLiteral("restored-") + entry.id);
            }
            QVERIFY(vault.restore(archived[1].id, directory_.filePath(QStringLiteral("missing-out")), &restored, &error));
            QCOMPARE(read_file(restored), random_bytes(301, 1));
        }
        Vault reopened{vault_dir_};
        QVERIFY(reopened.open(nullptr));
        QCOMPARE(reopened.entries().size(), 2);
    }

    void purgeCompactsAndKeepsThePreviousJournal() {
        QList<VaultEntry> archived;
        {
            Vault vault{vault_dir_};
            QVERIFY(vault.open(nullptr));
            for (int index = 0; index < 3; ++index) {
                const auto name = QStringLiteral("f%1").arg(index);
                const auto entry = vault.archive(write_file(QDir{sources_}.filePath(name), name.toUtf8()), QStringLiteral("s"),
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
        }
        const QDir dir{vault_dir_};
        QVERIFY(!QFile::exists(dir.filePath(archived[0].id + QStringLiteral(".pmsv"))));
        QVERIFY(!QFile::exists(dir.filePath(archived[1].id + QStringLiteral(".pmsv"))));
        QVERIFY(QFile::exists(dir.filePath(archived[2].id + QStringLiteral(".pmsv"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("journal.pmsj.prev"))));
        {
            Vault reopened{vault_dir_};
            QVERIFY(reopened.open(nullptr));
            QCOMPARE(reopened.entries().size(), 1);
            QString restored;
            QVERIFY(reopened.restore(archived[2].id, directory_.filePath(QStringLiteral("purge-out")), &restored, nullptr));
            QCOMPARE(read_file(restored), QByteArray{"f2"});
            QVERIFY(!reopened.restore(archived[0].id, directory_.filePath(QStringLiteral("purge-out")), &restored, nullptr));
        }
        // Losing the compacted journal: recovery takes the original name from journal.pmsj.prev.
        QVERIFY(QFile::remove(dir.filePath(QStringLiteral("journal.pmsj"))));
        Vault recovering{vault_dir_};
        QVERIFY(!recovering.open(nullptr));
        int recovered = 0;
        QVERIFY(recovering.recoverIndex(&recovered, nullptr));
        QCOMPARE(recovered, 1);
        QCOMPARE(recovering.entries().front().original_name, QStringLiteral("f2.jpg"));
        QCOMPARE(recovering.entries().front().source_id, QStringLiteral("f2"));
    }

    // M10: a cutoff in the future is clamped to now, so entries stamped later (a clock that jumped) stay.
    void purgeNeverReachesIntoTheFuture() {
        VaultEntry future;
        future.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        future.original_name = QStringLiteral("future.jpg");
        future.bytes = 1;
        future.sha256 = QByteArray(32, 'f');
        future.archived_ms = QDateTime::currentMSecsSinceEpoch() + 24LL * 60 * 60 * 1000;
        QVERIFY(QDir{}.mkpath(vault_dir_));
        QString error;
        QVERIFY(pms::desktop::detail::write_journal(QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj")), vault_key(),
                                                    {future}, error));
        Vault vault{vault_dir_};
        QVERIFY2(vault.open(&error), qPrintable(error));
        QCOMPARE(vault.purgeOlderThan(std::numeric_limits<qint64>::max()), 0);
        QCOMPARE(vault.entries().size(), 1);
    }

    // M3: unindexed payloads and temporaries go after a day, and only when the journal authenticated.
    void sweepsOnlyOldOrphansOfAnIntactJournal() {
        const auto archived = archive_files(1);
        QCOMPARE(archived.size(), 1);
        const QDir dir{vault_dir_};
        const auto indexed = dir.filePath(archived[0].id + QStringLiteral(".pmsv"));
        const auto old_orphan = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".pmsv"));
        const auto new_orphan = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".pmsv"));
        const auto old_temporary = dir.filePath(QStringLiteral("journal.pmsj.Ab12Cd"));
        for (const auto& path : {old_orphan, new_orphan, old_temporary}) {
            QVERIFY(QFile::copy(indexed, path));
        }
        const auto age = [](const QString& path) {
            QFile file{path};
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-2), QFileDevice::FileModificationTime));
        };
        age(old_orphan);
        age(old_temporary);
        age(indexed);

        const auto journal = dir.filePath(QStringLiteral("journal.pmsj"));
        flip_byte(journal, read_file(journal).size() - 1);
        {
            Vault damaged{vault_dir_};
            QVERIFY(!damaged.open(nullptr));
            QVERIFY(QFile::exists(old_orphan));  // nothing is swept from a vault whose index failed
            QVERIFY(QFile::exists(old_temporary));
        }
        flip_byte(journal, read_file(journal).size() - 1);
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        QVERIFY(!QFile::exists(old_orphan));
        QVERIFY(!QFile::exists(old_temporary));
        QVERIFY(QFile::exists(new_orphan));
        QVERIFY(QFile::exists(indexed));
        QCOMPARE(vault.entries().size(), 1);
    }

    void hasRoomForKeepsTenPercentHeadroom() {
        using pms::desktop::has_room;
        QVERIFY(has_room(110, 100));
        QVERIFY(!has_room(109, 100));
        QVERIFY(has_room(11, 10));
        QVERIFY(!has_room(11, 11));
        QVERIFY(!has_room(10, 10));
        QVERIFY(has_room(0, 0));
        QVERIFY(!has_room(1, 1));
        constexpr auto max = std::numeric_limits<quint64>::max();
        QVERIFY(!has_room(max, max));
        QVERIFY(has_room(max, max / 11 * 10));
        QVERIFY(!has_room(max / 2, max));

        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        QVERIFY(vault.hasRoomFor(1));
        QVERIFY(!vault.hasRoomFor(max / 2));
    }

    void neverLeavesPlaintextInTheVault() {
        const auto marker = QByteArray("PLAINTEXT-MARKER-") + random_bytes(64).toHex();
        const auto contents = marker + QByteArray(100000, 'x') + marker;
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        const auto entry = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("secret.jpg")), contents),
                                         QStringLiteral("s"), QStringLiteral("x"), QStringLiteral("secret.jpg"), nullptr);
        QVERIFY(entry.has_value());

        const auto files = QDir{vault_dir_}.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        QCOMPARE(files, (QStringList{entry->id + QStringLiteral(".pmsv"), QStringLiteral("journal.pmsj"),
                                     QStringLiteral("vault.lock")}));
        QVERIFY(!read_file(QDir{vault_dir_}.filePath(entry->id + QStringLiteral(".pmsv"))).contains(marker));
        QVERIFY(!read_file(QDir{vault_dir_}.filePath(QStringLiteral("journal.pmsj"))).contains("secret.jpg"));
    }

    void restoreStaysInsideTheDestination() {
        Vault vault{vault_dir_};
        QVERIFY(vault.open(nullptr));
        const auto out = directory_.filePath(QStringLiteral("names-out"));
        for (const auto& name : {QStringLiteral("../../evil.jpg"), QStringLiteral("..\\..\\evil.jpg"),
                                 QStringLiteral("a/b:c*?.jpg"), QStringLiteral(".."), QStringLiteral("NUL.jpg"),
                                 QStringLiteral("NUL .jpg"), QStringLiteral("CONIN$"), QStringLiteral("conout$.txt"),
                                 QString{QStringLiteral("COM") + QChar(0x00B9) + QStringLiteral(".jpg")},
                                 QString{QStringLiteral("lpt") + QChar(0x00B3)}}) {
            const auto entry = vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("src")), "data"),
                                             QStringLiteral("s"), QStringLiteral("id"), name, nullptr);
            QVERIFY(entry.has_value());
            QString restored;
            QString error;
            QVERIFY2(vault.restore(entry->id, out, &restored, &error), qPrintable(name + QStringLiteral(": ") + error));
            QCOMPARE(QFileInfo{restored}.absolutePath(), QFileInfo{out}.absoluteFilePath());
            QCOMPARE(read_file(restored), QByteArray{"data"});
            const auto file_name = QFileInfo{restored}.fileName();
            if (name.startsWith(QStringLiteral("NUL"), Qt::CaseInsensitive) || name.startsWith(QStringLiteral("CON"), Qt::CaseInsensitive) ||
                name.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive) || name.startsWith(QStringLiteral("lpt"), Qt::CaseInsensitive)) {
                QVERIFY2(file_name.startsWith(QLatin1Char('_')), qPrintable(file_name));
            }
        }
        QVERIFY(!QFile::exists(directory_.filePath(QStringLiteral("evil.jpg"))));
    }

    void refusesWorkBeforeOpenAndMissingFiles() {
        Vault vault{vault_dir_};
        QString error;
        QVERIFY(!vault.archive(write_file(QDir{sources_}.filePath(QStringLiteral("a")), "a"), QStringLiteral("s"),
                               QStringLiteral("a"), QStringLiteral("a"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(vault.open(nullptr));
        QVERIFY(!vault.archive(QDir{sources_}.filePath(QStringLiteral("missing")), QStringLiteral("s"), QStringLiteral("m"),
                               QStringLiteral("m"), &error));
        QString restored;
        QVERIFY(!vault.restore(QStringLiteral("not-an-entry"), directory_.path(), &restored, &error));
        QVERIFY(vault.entries().isEmpty());
    }
};

QTEST_MAIN(VaultTests)
#include "vault_tests.moc"
