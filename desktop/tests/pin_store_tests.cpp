#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "link/pin_store.hpp"

namespace {

namespace pl = pms::link;
using pms::desktop::VaultPinStore;

pl::Key32 key(const std::uint8_t fill) {
    pl::Key32 k{};
    k.fill(fill);
    return k;
}

pl::Pinned phone(const std::uint8_t fill, std::string model, const std::int64_t paired) {
    return {key(fill), std::move(model), paired};
}

class FailingVault final : public pms::desktop::SecretVault {
public:
    [[nodiscard]] std::optional<QByteArray> read() override { return std::nullopt; }
    void write(const QByteArray&) override { throw std::runtime_error("disk full"); }
};

const QRegularExpression unreadable{QStringLiteral("^pinned phones unreadable, starting empty: ")};

void write_file(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

}  // namespace

class PinStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsFileRoundTrip() {
#ifndef Q_OS_WIN
        QSKIP("DPAPI exists only on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("pins.dpapi"));
        {
            VaultPinStore store(pms::desktop::dpapi_file_vault(path));
            store.pin(phone(0xA1, "Pixel 9", 1'700'000'000));
            store.pin(phone(0xB2, "Galaxy S24 \xC3\xA9", 1'700'000'100));
        }

        VaultPinStore again(pms::desktop::dpapi_file_vault(path));
        QCOMPARE(again.size(), std::size_t{2});
        const auto a = again.find(key(0xA1));
        QVERIFY(a.has_value());
        QVERIFY(a->phone_key == key(0xA1));
        QCOMPARE(a->model, std::string("Pixel 9"));
        QCOMPARE(a->paired_unix_s, std::int64_t{1'700'000'000});
        const auto b = again.find(key(0xB2));
        QVERIFY(b.has_value());
        QCOMPARE(b->model, std::string("Galaxy S24 \xC3\xA9"));
        QVERIFY(!again.find(key(0xC3)).has_value());

        // DPAPI ciphertext: neither the model nor the key appears in the file.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto raw = file.readAll();
        QVERIFY(!raw.contains("Pixel 9"));
        QVERIFY(!raw.contains(QByteArray(32, static_cast<char>(0xA1))));
        QVERIFY(!raw.toLower().contains("a1a1a1a1"));
        file.close();

        again.unpin(key(0xA1));
        VaultPinStore third(pms::desktop::dpapi_file_vault(path));
        QVERIFY(!third.find(key(0xA1)).has_value());
        QVERIFY(third.find(key(0xB2)).has_value());
#endif
    }

    void corruptedWindowsFileStartsEmptyWithAWarning() {
#ifndef Q_OS_WIN
        QSKIP("DPAPI exists only on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("pins.dpapi"));
        write_file(path, "not a DPAPI blob at all");

        QTest::ignoreMessage(QtWarningMsg, unreadable);
        VaultPinStore store(pms::desktop::dpapi_file_vault(path));
        QCOMPARE(store.size(), std::size_t{0});

        store.pin(phone(0xA1, "Pixel 9", 1));  // the next change replaces the unreadable file
        VaultPinStore again(pms::desktop::dpapi_file_vault(path));
        QVERIFY(again.find(key(0xA1)).has_value());
#endif
    }

    void malformedContentsStartEmptyWithAWarning() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("pins.json"));
        const QByteArray bad[] = {
            "",
            "{",
            "[]",
            R"({"version":2,"pins":[]})",
            R"({"version":1,"pins":{}})",
            R"({"version":1,"pins":[{"key":"zz","model":"x","paired":1}]})",
            R"({"version":1,"pins":[{"key":"a1a1","model":"x","paired":1}]})",
            R"({"version":1,"pins":[{"key":"a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1","paired":1}]})",
            R"({"version":1,"pins":[{"key":"a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1","model":"x","paired":"1"}]})",
        };
        for (const auto& contents : bad) {
            write_file(path, contents);
            QTest::ignoreMessage(QtWarningMsg, unreadable);
            const VaultPinStore store(pms::desktop::owner_only_file_vault(path));
            QCOMPARE(store.size(), std::size_t{0});
        }
    }

    void ownerOnlyFileRoundTrip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("pins.json"));
        {
            VaultPinStore store(pms::desktop::owner_only_file_vault(path));
            store.pin(phone(0xA1, "Pixel 9", 1'700'000'000));
            store.pin(phone(0xA1, "Pixel 9 Pro", 1'700'000'500));  // pairing the same phone again replaces its pin
        }
        const VaultPinStore again(pms::desktop::owner_only_file_vault(path));
        QCOMPARE(again.size(), std::size_t{1});
        QCOMPARE(again.find(key(0xA1))->model, std::string("Pixel 9 Pro"));
        QCOMPARE(again.find(key(0xA1))->paired_unix_s, std::int64_t{1'700'000'500});
#ifndef Q_OS_WIN
        const auto others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther |
                            QFile::WriteOther | QFile::ExeOther;
        QCOMPARE(QFile::permissions(path) & others, QFile::Permissions{});
#endif
    }

    void aFailedWriteChangesNothing() {
        VaultPinStore store(std::make_unique<FailingVault>());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.pin(phone(0xA1, "Pixel 9", 1)));
        QVERIFY(!store.find(key(0xA1)).has_value());
        QCOMPARE(store.size(), std::size_t{0});
    }
};

QTEST_MAIN(PinStoreTests)
#include "pin_store_tests.moc"
