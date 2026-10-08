#include "link/pipe_stream.hpp"

#include <QMetaObject>

namespace pms::desktop {

std::pair<PipeStream*, PipeStream*> PipeStream::create(QObject* parent) {
    auto* a = new PipeStream(parent);
    auto* b = new PipeStream(parent);
    a->peer_ = b;
    b->peer_ = a;
    return {a, b};
}

void PipeStream::write(QByteArray bytes) {
    constexpr qsizetype max_read = 64 * 1024;
    for (qsizetype at = 0; open_ && peer_ && at < bytes.size(); at += max_read) {
        QMetaObject::invokeMethod(
            peer_,
            [peer = peer_, read = bytes.mid(at, max_read)] {
                if (peer && peer->open_) {
                    emit peer->bytesReceived(read);
                }
            },
            Qt::QueuedConnection);
    }
}

void PipeStream::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (peer_) {
        // Queued behind any bytes this end already wrote, so the peer reads them before it sees the close.
        QMetaObject::invokeMethod(
            peer_,
            [peer = peer_] {
                if (peer && peer->open_) {
                    peer->open_ = false;
                    emit peer->closed(QStringLiteral("the other end closed"));
                }
            },
            Qt::QueuedConnection);
    }
    emit closed(QStringLiteral("closed locally"));
}

}  // namespace pms::desktop
