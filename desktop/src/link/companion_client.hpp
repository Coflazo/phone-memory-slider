#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "link/byte_stream.hpp"
#include "pms/link/frames.hpp"
#include "pms/link/messages.hpp"
#include "pms/link/session.hpp"

namespace pms::desktop {

// The desktop side of one phone session after the Noise handshake (docs/PROTOCOL.md sections 6 to 8). It waits for
// the phone's Hello, answers HelloAck, then sends the queued requests in order while at most 16 credits are
// outstanding (a request for n ids holds n). Trash requests go one at a time, the next after the previous result, and
// requests of other types queued behind a waiting one go ahead of it.
//
// Per-item statuses (Missing, Unreadable, Busy) arrive through the normal signals. A protocol violation, a frame that
// fails to decrypt, an Error that closes the session, more than 8 session-level Errors, Bye, an idle timeout, a closed
// stream or a stream deleted without closing emits failed() once and closes the stream if it is still there; frames
// after that are not processed. One client serves one connection and re-sends nothing: after failed(), read
// unanswered() and queue it on the next connection's client. Do not delete the client from inside one of its signals.
class CompanionClient : public QObject {
    Q_OBJECT

public:
    // session must be Established and outlive the client. Construct the client before control returns to the event
    // loop after message 2 is written, so it sees every transport frame from the phone's Hello on.
    CompanionClient(ByteStream* stream, pms::link::ResponderSession* session, QObject* parent = nullptr);

    void requestCatalog(QString storeVersion, quint64 generation);  // one CatalogPage; ask again while page.more
    void requestPreviews(QList<quint64> ids);
    void requestKeyframes(quint64 id, int count);                   // count 1 to 8, else std::invalid_argument
    void requestHashes(QList<quint64> ids);
    void requestTrash(QList<pms::link::TrashItem> items);           // sent as requests of at most 500 items

    // Idle limits, measured from the last received frame (or from a request sent while nothing was outstanding); the
    // second applies while a TrashRequest is outstanding. Session-level Errors do not count as frames here.
    void setIdleTimeouts(std::chrono::milliseconds normal, std::chrono::milliseconds whileTrashing);

    // Everything queued or sent and not answered yet, sent requests first.
    struct Unanswered {
        bool catalog{};
        QList<quint64> previews;
        QList<quint64> hashes;
        QList<pms::link::KeyframesRequest> keyframes;
        QList<pms::link::TrashItem> trashQueued;  // never sent: safe to send on the next connection
        // Sent, outcome unknown (the phone may have trashed them): sync the catalog first, then send again only the
        // items that still exist (PROTOCOL.md section 8).
        QList<pms::link::TrashItem> trashInFlight;
    };
    [[nodiscard]] Unanswered unanswered() const;

signals:
    void helloReceived(pms::link::Hello hello);
    void catalogPage(pms::link::CatalogPage page);
    void preview(pms::link::Preview item);
    void keyframes(pms::link::Keyframes frames);
    void hash(pms::link::Hash item);
    void trashResult(pms::link::TrashResult result);
    // The phone ended a request with an Error that keeps the session open (PermissionDenied, Busy, Internal or an
    // unknown code). ids are what it left unanswered: preview or hash ids, the video id, the trash item ids, or none
    // for a catalog request.
    void requestError(pms::link::MsgType request, QList<quint64> ids, quint16 code, QString message);
    void failed(QString reason);

private:
    struct Request {
        pms::link::MsgType type{};
        pms::link::Body body;            // CatalogSince, KeyframesRequest or TrashRequest
        std::vector<std::uint64_t> ids;  // PreviewRequest, HashRequest: ids not answered yet, one credit each
    };

    void enqueue(Request request);
    void pump();
    void onBytes(const QByteArray& bytes);
    void onFrame(const std::vector<std::byte>& frame);
    void onMessage(pms::link::Message message);
    void onHello(const pms::link::Message& message);
    void onResponse(pms::link::Message message);
    void onError(std::uint32_t requestId, const pms::link::ErrorMsg& error);
    void send(const pms::link::Message& message);
    void violation(pms::link::ErrorCode code, const QString& reason);
    void fail(const QString& reason);
    void armIdle();
    [[nodiscard]] std::uint32_t nextRequestId();

    QPointer<ByteStream> stream_;
    pms::link::ResponderSession* session_;
    pms::link::FrameDecoder decoder_{pms::link::max_frame};
    std::deque<Request> queue_;
    std::map<std::uint32_t, Request> inflight_;
    std::size_t credits_{0};
    std::size_t sessionErrors_{0};
    std::uint32_t lastRequestId_{0};
    bool hello_{false};
    bool trashInFlight_{false};
    bool failed_{false};
    std::chrono::milliseconds idleLimit_{std::chrono::minutes{5}};
    std::chrono::milliseconds trashIdleLimit_{std::chrono::minutes{10}};
    QElapsedTimer sinceFrame_;
    QTimer idleTimer_;
};

}  // namespace pms::desktop
