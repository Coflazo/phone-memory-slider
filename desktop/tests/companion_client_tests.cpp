#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QtTest>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include "fake_phone.hpp"
#include "link/companion_client.hpp"
#include "link/pipe_stream.hpp"
#include "pms/link/frames.hpp"
#include "pms/link/session.hpp"

namespace {

using namespace std::chrono_literals;
using pms::desktop::CompanionClient;
using pms::desktop::PipeStream;
using pms::desktop::testing::FakePhone;
namespace pl = pms::link;

class MemoryPins final : public pl::PinStore {
public:
    [[nodiscard]] std::optional<pl::Pinned> find(const pl::Key32& key) const override {
        const auto it = pins_.find(key);
        return it == pins_.end() ? std::nullopt : std::optional{it->second};
    }
    void pin(const pl::Pinned& p) override { pins_[p.phone_key] = p; }
    void unpin(const pl::Key32& key) override { pins_.erase(key); }

private:
    std::map<pl::Key32, pl::Pinned> pins_;
};

std::uint32_t now_unix_s() {
    return static_cast<std::uint32_t>(QDateTime::currentSecsSinceEpoch());
}

// One paired session over an in-memory pipe: the fake phone runs a real IK handshake against a ResponderSession,
// the way the RFCOMM server will, then a CompanionClient takes over the desktop end.
class Link {
public:
    explicit Link(FakePhone::Script script)
        : window_(token_, now_unix_s() + 120), session_(desktopKeys_, uuid_, pins_) {
        session_.open_pairing_window(window_);
        std::tie(desktop_, phoneEnd_) = PipeStream::create(&owner_);
        phone_ = std::make_unique<FakePhone>(phoneEnd_, desktopKeys_.public_key, uuid_, token_, std::move(script));
    }

    // Reads message 1 under the pre-handshake frame cap, answers message 2, then hands the stream to the client.
    [[nodiscard]] bool connect() {
        pl::FrameDecoder decoder{pl::max_preauth_frame};
        std::optional<std::vector<std::byte>> msg1;
        bool bad = false;
        const auto reader = QObject::connect(desktop_, &PipeStream::bytesReceived, [&](const QByteArray& bytes) {
            bad = bad || !decoder.feed(std::as_bytes(std::span{bytes.constData(), static_cast<std::size_t>(bytes.size())}));
            if (!msg1) {
                msg1 = decoder.next();
            }
        });
        phone_->start();
        const bool arrived = QTest::qWaitFor([&] { return msg1.has_value() || bad; }, 5000);
        QObject::disconnect(reader);
        std::vector<std::byte> msg2;
        if (!arrived || bad || session_.on_message1(*msg1, now_unix_s(), msg2) != pl::HandshakeResult::Established) {
            return false;
        }
        const auto frame = pl::encode_frame(msg2);
        desktop_->write(QByteArray(reinterpret_cast<const char*>(frame.data()), static_cast<qsizetype>(frame.size())));
        client_ = std::make_unique<CompanionClient>(desktop_, &session_);
        return true;
    }

    CompanionClient& client() { return *client_; }
    FakePhone& phone() { return *phone_; }
    PipeStream& desktopEnd() { return *desktop_; }

private:
    QObject owner_;  // owns both pipe ends; destroyed last
    MemoryPins pins_;
    pl::KeyPair desktopKeys_ = pl::generate_keypair();
    std::array<std::uint8_t, 16> uuid_{0x8f, 0x4d, 0x9d, 0x6a, 0x0c, 0x84, 0x4a, 0x7f,
                                       0xa3, 0x6a, 0x6f, 0xc1, 0x2c, 0x4b, 0x34, 0xbf};
    std::array<std::uint8_t, 16> token_{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    pl::PairingWindow window_;
    pl::ResponderSession session_;
    PipeStream* desktop_{nullptr};
    PipeStream* phoneEnd_{nullptr};
    std::unique_ptr<FakePhone> phone_;
    std::unique_ptr<CompanionClient> client_;
};

QList<quint64> ids_up_to(const quint64 last) {
    QList<quint64> ids;
    for (quint64 id = 1; id <= last; ++id) {
        ids.push_back(id);
    }
    return ids;
}

}  // namespace

class CompanionClientTests final : public QObject {
    Q_OBJECT

private slots:
    void catalogArrivesInPagesUntilMoreIsFalse() {
        Link link{{.catalogSize = 2500}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        std::vector<pl::CatalogPage> pages;
        QObject::connect(&client, &CompanionClient::catalogPage, &client, [&](const pl::CatalogPage& page) {
            pages.push_back(page);
            if (page.more) {  // the consumer stores the cursor, then echoes it
                client.requestCatalog(QString::fromStdString(page.store_version), page.next_generation);
            }
        });

        client.requestCatalog({}, 0);

        QTRY_VERIFY_WITH_TIMEOUT(!pages.empty() && !pages.back().more, 10000);
        // 2,500 items cannot fit two pages: a page is at most 65,519 bytes, about 710 items with these paths.
        QVERIFY(pages.size() >= 2);
        QVERIFY(pages.front().full_resync);
        QSet<quint64> ids;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            QCOMPARE(pages[i].more, i + 1 < pages.size());
            for (const auto& item : pages[i].items) {
                ids.insert(item.id);
            }
        }
        QCOMPARE(ids.size(), 2500);
        QCOMPARE(pages.back().next_generation, std::uint64_t{2500});
        QVERIFY(failed.isEmpty());
        QVERIFY(!client.unanswered().catalog);
        qInfo("catalog of 2,500 items arrived in %d pages", static_cast<int>(pages.size()));
    }

    void previewsStayWithinSixteenCredits() {
        Link link{{.catalogSize = 40}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSet<quint64> answered;
        QObject::connect(&client, &CompanionClient::preview, &client, [&](const pl::Preview& preview) {
            QCOMPARE(preview.status, pl::ItemStatus::Ok);
            answered.insert(preview.id);
        });

        client.requestPreviews(ids_up_to(40));

        QTRY_COMPARE_WITH_TIMEOUT(answered.size(), 40, 10000);
        QCOMPARE(link.phone().maxOutstanding(), std::size_t{16});  // the window is full, never over
        QVERIFY(failed.isEmpty());
        QVERIFY(client.unanswered().previews.isEmpty());
    }

    void itemDeletedMidRunArrivesMissing() {
        Link link{{.catalogSize = 40}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        std::map<quint64, pl::ItemStatus> statuses;
        QObject::connect(&client, &CompanionClient::preview, &client, [&](const pl::Preview& preview) {
            if (statuses.empty()) {
                link.phone().deleteItem(40);  // the user deletes the last photo while the previews stream in
            }
            statuses[preview.id] = preview.status;
        });

        client.requestPreviews(ids_up_to(40));

        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), std::size_t{40}, 10000);
        QCOMPARE(statuses[40], pl::ItemStatus::Missing);
        for (quint64 id = 1; id < 40; ++id) {
            QCOMPARE(statuses[id], pl::ItemStatus::Ok);
        }
        QVERIFY(failed.isEmpty());
        QVERIFY(link.desktopEnd().isOpen());
    }

    void trashGoesOutInRequestsOf500OneAtATime() {
        Link link{{.catalogSize = 1200, .declineTrash = {7}}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        std::vector<pl::TrashResult> results;
        QObject::connect(&client, &CompanionClient::trashResult, &client,
                         [&](const pl::TrashResult& result) { results.push_back(result); });
        QList<pl::TrashItem> items;
        for (std::uint64_t id = 1; id <= 1200; ++id) {
            items.push_back({id, 1'000'000 + id, 1'700'000'000 + static_cast<std::int64_t>(id), {}});
        }

        client.requestTrash(items);

        QTRY_COMPARE_WITH_TIMEOUT(results.size(), std::size_t{3}, 10000);
        QVERIFY(link.phone().trashRequestSizes() == (std::vector<std::size_t>{500, 500, 200}));
        QCOMPARE(link.phone().maxOutstanding(), std::size_t{1});  // the next request waits for the previous result
        QCOMPARE(results[0].items.size(), std::size_t{500});
        QCOMPARE(results[1].items.size(), std::size_t{500});
        QCOMPARE(results[2].items.size(), std::size_t{200});
        for (const auto& result : results) {
            for (const auto& [id, outcome] : result.items) {
                QCOMPARE(outcome, id == 7 ? pl::TrashOutcome::UserDeclined : pl::TrashOutcome::Trashed);
            }
        }
        QCOMPARE(results[2].items.back().first, std::uint64_t{1200});
        QVERIFY(failed.isEmpty());
        QVERIFY(client.unanswered().trashQueued.isEmpty() && client.unanswered().trashInFlight.isEmpty());
    }

    void oversizedStringFailsAndClosesTheStream() {
        Link link{{.helloModel = std::string(2048, 'x')}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy hello(&client, &CompanionClient::helloReceived);
        client.requestPreviews({1, 2});

        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.front().front().toString().contains(QStringLiteral("malformed")));
        QTRY_VERIFY(link.phone().closed());
        QVERIFY(!link.desktopEnd().isOpen());
        QCOMPARE(link.phone().errorReceived().value_or(0), quint16{1});  // Malformed, sent before closing
        QTest::qWait(50);
        QCOMPARE(failed.size(), 1);  // exactly once, although the stream closed after it
        QCOMPARE(hello.size(), 0);
        QCOMPARE(client.unanswered().previews, (QList<quint64>{1, 2}));  // for the next connection to send
    }

    void keyframesHashesAndAnErrorEndedRequest() {
        Link link{{.catalogSize = 20, .busyKeyframes = {8}}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        std::vector<pl::Keyframes> frames;
        QSet<quint64> hashed;
        std::vector<std::pair<pl::MsgType, QList<quint64>>> errors;
        QObject::connect(&client, &CompanionClient::keyframes, &client, [&](const pl::Keyframes& f) { frames.push_back(f); });
        QObject::connect(&client, &CompanionClient::hash, &client, [&](const pl::Hash& h) {
            QCOMPARE(h.status, pl::ItemStatus::Ok);
            hashed.insert(h.id);
        });
        QObject::connect(&client, &CompanionClient::requestError, &client,
                         [&](pl::MsgType type, const QList<quint64>& ids, quint16 code, const QString&) {
                             QCOMPARE(code, quint16{6});  // Busy keeps the session open
                             errors.emplace_back(type, ids);
                         });

        client.requestKeyframes(7, 4);
        client.requestKeyframes(8, 4);
        client.requestHashes(ids_up_to(20));

        QTRY_COMPARE_WITH_TIMEOUT(hashed.size(), 20, 10000);
        QTRY_COMPARE(frames.size(), std::size_t{1});
        QCOMPARE(frames[0].id, std::uint64_t{7});
        QCOMPARE(frames[0].jpegs.size(), std::size_t{4});
        QCOMPARE(errors.size(), std::size_t{1});
        QCOMPARE(errors[0].first, pl::MsgType::KeyframesRequest);
        QCOMPARE(errors[0].second, (QList<quint64>{8}));
        QVERIFY(link.phone().maxOutstanding() <= 16);
        QVERIFY(failed.isEmpty());
        const auto left = client.unanswered();
        QVERIFY(left.keyframes.isEmpty() && left.hashes.isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, client.requestKeyframes(9, 9));
    }

    void idleTimeoutWaitsLongerWhileTrashing() {
        Link link{{.catalogSize = 3, .trashDelayMs = 600}};
        QVERIFY(link.connect());
        auto& client = link.client();
        client.setIdleTimeouts(250ms, 5000ms);
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy results(&client, &CompanionClient::trashResult);

        client.requestTrash({{1, 1, 1, {}}, {2, 2, 2, {}}});

        // The phone's dialog takes 600 ms, longer than the 250 ms normal limit: the trash limit applies meanwhile.
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 3000);
        QVERIFY(failed.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QCOMPARE(failed.front().front().toString(), QStringLiteral("idle timeout"));
        QTRY_VERIFY(link.phone().closed());
    }

    void trashLeftUnansweredIsSplitBySentAndQueued() {
        Link link{{.catalogSize = 600, .trashDelayMs = 5000}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QList<pl::TrashItem> items;
        for (std::uint64_t id = 1; id <= 600; ++id) {
            items.push_back({id, 1, 1, {}});
        }
        client.requestTrash(items);
        QTRY_COMPARE(link.phone().trashRequestSizes().size(), std::size_t{1});

        link.phone().hangUp();  // the connection drops while the phone's trash dialog is open

        QTRY_COMPARE(failed.size(), 1);
        const auto left = client.unanswered();
        QCOMPARE(left.trashInFlight.size(), 500);  // outcome unknown: needs a catalog sync before any resend
        QCOMPARE(left.trashQueued.size(), 100);    // never sent
        QCOMPARE(left.trashInFlight.front().id, std::uint64_t{1});
        QCOMPARE(left.trashQueued.front().id, std::uint64_t{501});
    }

    void tamperedFrameMidBurstFailsOnce() {
        Link link{{.catalogSize = 16, .burst = true}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        int previews = 0;
        QObject::connect(&client, &CompanionClient::preview, &client, [&] { ++previews; });
        link.phone().corruptFrameAt(6);  // Hello is frame 1, so this is the 5th of 16 previews sent in one read

        client.requestPreviews(ids_up_to(16));

        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.front().front().toString(), QStringLiteral("a frame failed to decrypt"));
        QCOMPARE(previews, 4);  // the frames behind the bad one in the same read are not processed
        QTRY_VERIFY(link.phone().closed());
        QTest::qWait(50);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(previews, 4);
    }

    void responseBeforeHelloFails() {
        Link link{{.catalogSize = 4, .sendHello = false}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QTRY_VERIFY(link.phone().established());  // it can seal only after message 2

        link.phone().sendMessage({pl::MsgType::Preview, 1, pl::Preview{1, pl::ItemStatus::Missing, 0, 0, {}}});

        QTRY_COMPARE(failed.size(), 1);
        QTRY_COMPARE(link.phone().errorReceived().value_or(0), quint16{4});  // UnexpectedMessage
    }

    void responseToUnknownRequestIdFails() {
        Link link{{.catalogSize = 4}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy hello(&client, &CompanionClient::helloReceived);
        QTRY_COMPARE(hello.size(), 1);

        link.phone().sendMessage({pl::MsgType::Preview, 999, pl::Preview{1, pl::ItemStatus::Missing, 0, 0, {}}});

        QTRY_COMPARE(failed.size(), 1);
        QTRY_COMPARE(link.phone().errorReceived().value_or(0), quint16{4});
    }

    void otherProtocolVersionFails() {
        Link link{{.helloProtocol = 2}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy hello(&client, &CompanionClient::helloReceived);

        QTRY_COMPARE(failed.size(), 1);
        QTRY_COMPARE(link.phone().errorReceived().value_or(0), quint16{2});  // UnsupportedProtocol
        QCOMPARE(hello.size(), 0);
    }

    void remoteCloseFailsOnce() {
        Link link{{.catalogSize = 4}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy hello(&client, &CompanionClient::helloReceived);
        QTRY_COMPARE(hello.size(), 1);

        link.phone().hangUp();

        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.front().front().toString().startsWith(QStringLiteral("stream closed")));
        QVERIFY(!link.desktopEnd().isOpen());
        QTest::qWait(50);
        QCOMPARE(failed.size(), 1);
    }

    void sessionErrorBeforeHelloFails() {
        Link link{{.sendHello = false}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QTRY_VERIFY(link.phone().established());  // it can seal only after message 2

        link.phone().sendMessage({pl::MsgType::Error, 0, pl::ErrorMsg{6, "busy"}});

        QTRY_COMPARE(failed.size(), 1);
        QTRY_COMPARE(link.phone().errorReceived().value_or(0), quint16{4});
    }

    void sessionErrorsAreCappedAtEight() {
        Link link{{.catalogSize = 4}};
        QVERIFY(link.connect());
        auto& client = link.client();
        QSignalSpy failed(&client, &CompanionClient::failed);
        QSignalSpy hello(&client, &CompanionClient::helloReceived);
        QSignalSpy previews(&client, &CompanionClient::preview);
        QTRY_COMPARE(hello.size(), 1);

        for (int i = 0; i < 8; ++i) {
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^phone session error 6 busy$")));
            link.phone().sendMessage({pl::MsgType::Error, 0, pl::ErrorMsg{6, "busy"}});
        }
        client.requestPreviews({1});  // its answer comes after the eight Errors: they have all been handled
        QTRY_COMPARE(previews.size(), 1);
        QVERIFY(failed.isEmpty());

        link.phone().sendMessage({pl::MsgType::Error, 0, pl::ErrorMsg{6, "busy"}});

        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.front().front().toString(), QStringLiteral("too many session errors from the phone"));
    }
};

QTEST_MAIN(CompanionClientTests)
#include "companion_client_tests.moc"
