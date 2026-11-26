#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>

#include "fake_phone.hpp"
#include "link/companion_client.hpp"
#include "link/pipe_stream.hpp"
#include "link/rfcomm_server.hpp"
#include "pms/link/qr.hpp"

namespace {

using namespace std::chrono_literals;
using pms::desktop::CompanionClient;
using pms::desktop::ConnectionGuard;
using pms::desktop::PipeStream;
using pms::desktop::RfcommServer;
using pms::desktop::testing::FakePhone;
using pms::desktop::testing::MemoryPins;
namespace pl = pms::link;

const QBluetoothUuid service_uuid{QStringLiteral("{8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf}")};

// A server listening without Bluetooth, and phones that reach it through PipeStream ends instead of sockets.
class Bench {
public:
    Bench() : server_(pl::generate_keypair(), pins_) { server_.startWithoutRadio(service_uuid); }

    RfcommServer& server() { return server_; }
    MemoryPins& pins() { return pins_; }
    [[nodiscard]] pl::PairingCode code() {
        const auto code = pl::decode_pairing(server_.showPairingCode().toStdString(), 0);
        if (!code) {
            throw std::runtime_error("showPairingCode returned an invalid code");
        }
        return *code;
    }

    // A fake phone that connects and sends message 1 with code's token.
    FakePhone& dial(const pl::PairingCode& code) {
        auto [desktop, phone] = PipeStream::create(&owner_);
        phones_.push_back(std::make_unique<FakePhone>(phone, code.desktop_key, code.service_uuid, code.token,
                                                      FakePhone::Script{.catalogSize = 4}));
        server_.accept(desktop);
        phones_.back()->start();
        return *phones_.back();
    }

    // A connection whose phone end the test drives by hand.
    PipeStream* dialRaw() {
        auto [desktop, phone] = PipeStream::create(&owner_);
        server_.accept(desktop);
        return phone;
    }

private:
    QObject owner_;  // owns the pipe ends the server has not deleted; destroyed last
    MemoryPins pins_;
    RfcommServer server_;
    std::vector<std::unique_ptr<FakePhone>> phones_;
};

QString reason(const QSignalSpy& rejected, const qsizetype i) {
    return rejected.at(i).front().toString();
}

}  // namespace

class RfcommServerTests final : public QObject {
    Q_OBJECT

private slots:
    void guardAllowsOneAttemptPerSecond() {
        ConnectionGuard guard;
        const auto t0 = ConnectionGuard::Clock::time_point{} + 1h;
        QVERIFY(guard.allow_attempt(t0));
        QVERIFY(!guard.allow_attempt(t0 + 999ms));
        QVERIFY(guard.allow_attempt(t0 + 1000ms));
        QVERIFY(!guard.allow_attempt(t0 + 1500ms));  // a refused attempt does not start a new second
        QVERIFY(guard.allow_attempt(t0 + 2000ms));
    }

    void guardExpiresTheHandshakeAfterFiveSeconds() {
        const ConnectionGuard guard;
        const auto t0 = ConnectionGuard::Clock::time_point{} + 1h;
        QVERIFY(!guard.handshake_expired(t0, t0));
        QVERIFY(!guard.handshake_expired(t0, t0 + 4900ms));
        QVERIFY(guard.handshake_expired(t0, t0 + 5100ms));
    }

    void pairsAPhoneAndHandsItToAClient() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        auto& phone = bench.dial(bench.code());

        QTRY_COMPARE(started.size(), 1);
        auto* client = started.front().at(0).value<CompanionClient*>();
        QVERIFY(client != nullptr);
        QCOMPARE(started.front().at(1).toString(), QStringLiteral("Pixel 9"));
        QVERIFY(QRegularExpression(QStringLiteral("^\\d{6}$")).match(started.front().at(2).toString()).hasMatch());
        QCOMPARE(bench.pins().size(), std::size_t{1});
        QVERIFY(phone.established());

        // The client was in place before message 2 left, so it saw the phone's Hello and serves requests.
        QSignalSpy previews(client, &CompanionClient::preview);
        client->requestPreviews({1, 2});
        QTRY_COMPARE(previews.size(), 2);
        QVERIFY(rejected.isEmpty());
    }

    void refusesASecondConnectionWhileASessionIsLive() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        const auto code = bench.code();
        bench.dial(code);
        QTRY_COMPARE(started.size(), 1);

        auto& second = bench.dial(code);

        QCOMPARE(rejected.size(), 1);
        QVERIFY(reason(rejected, 0).startsWith(QStringLiteral("busy")));
        QTRY_VERIFY(second.closed());
        QCOMPARE(started.size(), 1);
    }

    void refusesASecondConnectionDuringAHandshake() {
        Bench bench;
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        auto* first = bench.dialRaw();  // connected, message 1 not sent yet

        auto* second = bench.dialRaw();

        QCOMPARE(rejected.size(), 1);
        QVERIFY(reason(rejected, 0).startsWith(QStringLiteral("busy")));
        QTRY_VERIFY(!second->isOpen());
        QVERIFY(first->isOpen());
    }

    void allowsOneHandshakeAttemptPerSecond() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        const auto code = bench.code();
        auto* garbage = bench.dialRaw();
        garbage->write(QByteArray("\x02\x01", 2));  // announces 513 bytes, over the pre-handshake cap
        QTRY_COMPARE(rejected.size(), 1);
        QVERIFY(reason(rejected, 0).startsWith(QStringLiteral("garbage")));

        auto& early = bench.dial(code);  // within a second of the first attempt

        QCOMPARE(rejected.size(), 2);
        QVERIFY(reason(rejected, 1).startsWith(QStringLiteral("rate limit")));
        QTRY_VERIFY(early.closed());

        QTest::qWait(1000);
        bench.dial(code);
        QTRY_COMPARE(started.size(), 1);  // refused attempts never burned the token
    }

    void refusesASecondFrameBeforeTheHandshake() {
        Bench bench;
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        auto* raw = bench.dialRaw();
        QByteArray two;
        for (int i = 0; i < 2; ++i) {
            two += QByteArray("\x00\x10", 2);
            two += QByteArray(16, 'x');
        }

        raw->write(two);  // two 16-byte frames in one read: only one frame is allowed before the handshake

        QTRY_COMPARE(rejected.size(), 1);
        QVERIFY(reason(rejected, 0).startsWith(QStringLiteral("garbage")));
        QTRY_VERIFY(!raw->isOpen());
    }

    void dropsAHandshakeThatTakesTooLong() {
        Bench bench;
        bench.server().setHandshakeTimeout(200ms);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);

        auto* silent = bench.dialRaw();

        QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 3000);
        QCOMPARE(reason(rejected, 0), QStringLiteral("handshake timeout"));
        QTRY_VERIFY(!silent->isOpen());
    }

    void listensOnlyBetweenStartAndStop() {
        QObject owner;
        MemoryPins pins;
        RfcommServer server(pl::generate_keypair(), pins);
        QSignalSpy rejected(&server, &RfcommServer::rejected);
        const auto before = PipeStream::create(&owner);

        server.accept(before.first);

        QCOMPARE(rejected.size(), 1);
        QCOMPARE(reason(rejected, 0), QStringLiteral("not listening"));
        QTRY_VERIFY(!before.second->isOpen());
        QVERIFY_THROWS_EXCEPTION(std::logic_error, (void)server.showPairingCode());

        server.startWithoutRadio(service_uuid);
        const auto during = PipeStream::create(&owner);
        server.accept(during.first);  // handshaking
        server.stop();
        QCOMPARE(rejected.size(), 2);
        QCOMPARE(reason(rejected, 1), QStringLiteral("the server stopped"));
        QTRY_VERIFY(!during.second->isOpen());

        const auto after = PipeStream::create(&owner);
        server.accept(after.first);
        QCOMPARE(rejected.size(), 3);
        QCOMPARE(reason(rejected, 2), QStringLiteral("not listening"));
    }

    void aCodePairsOnePhoneOnly() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        const auto code = bench.code();
        auto& first = bench.dial(code);
        QTRY_COMPARE(started.size(), 1);
        auto* client = started.front().at(0).value<CompanionClient*>();
        QSignalSpy failed(client, &CompanionClient::failed);

        first.hangUp();  // the session ends and frees the server for the next connection
        QTRY_COMPARE(failed.size(), 1);
        QTest::qWait(1000);
        auto& second = bench.dial(code);  // another phone scanned the same code

        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(reason(rejected, 0), QStringLiteral("handshake refused"));
        QTRY_VERIFY(second.closed());
        QCOMPARE(started.size(), 1);
        QCOMPARE(bench.pins().size(), std::size_t{1});
    }

    void aHiddenCodeCannotPair() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        const auto code = bench.code();
        bench.server().hidePairingCode();

        bench.dial(code);

        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(reason(rejected, 0), QStringLiteral("handshake refused"));
        QCOMPARE(started.size(), 0);
        QCOMPARE(bench.pins().size(), std::size_t{0});
    }

    void hidingTheCodeMidHandshakeRefusesIt() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        bench.dial(bench.code());  // message 1 is on its way

        bench.server().hidePairingCode();  // the connection still holds the window, now burned

        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(reason(rejected, 0), QStringLiteral("handshake refused"));
        QCOMPARE(started.size(), 0);
    }

    void aNewCodeReplacesTheOldOne() {
        Bench bench;
        QSignalSpy started(&bench.server(), &RfcommServer::sessionStarted);
        QSignalSpy rejected(&bench.server(), &RfcommServer::rejected);
        const auto old = bench.code();
        const auto fresh = bench.code();
        QVERIFY(old.token != fresh.token);
        QCOMPARE(fresh.service_uuid, old.service_uuid);

        bench.dial(old);
        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(reason(rejected, 0), QStringLiteral("handshake refused"));

        QTest::qWait(1000);
        bench.dial(fresh);
        QTRY_COMPARE(started.size(), 1);
    }
};

QTEST_MAIN(RfcommServerTests)
#include "rfcomm_server_tests.moc"
