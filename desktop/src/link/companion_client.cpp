#include "link/companion_client.hpp"

#include <QDebug>
#include <QSysInfo>

#include <algorithm>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pms::desktop {

namespace {

constexpr std::size_t credit_window = 16;      // PROTOCOL.md section 7
constexpr qsizetype max_read = 64 * 1024;      // PROTOCOL.md section 10: feed at most 64 KiB, then drain
constexpr std::size_t max_session_errors = 8;  // session-level Errors tolerated before the client gives up

[[nodiscard]] bool is_id_list(const link::MsgType type) noexcept {
    return type == link::MsgType::PreviewRequest || type == link::MsgType::HashRequest;
}

// The response type that answers a request type.
[[nodiscard]] link::MsgType answer_to(const link::MsgType request) noexcept {
    return static_cast<link::MsgType>(static_cast<std::uint8_t>(request) + 1);  // 0x10 -> 0x11, ..., 0x20 -> 0x21
}

}  // namespace

CompanionClient::CompanionClient(ByteStream* stream, link::ResponderSession* session, QObject* parent)
    : QObject(parent), stream_(stream), session_(session), idleTimer_(this) {
    connect(stream, &ByteStream::bytesReceived, this, &CompanionClient::onBytes);
    connect(stream, &ByteStream::closed, this,
            [this](const QString& reason) { fail(QStringLiteral("stream closed: %1").arg(reason)); });
    idleTimer_.setSingleShot(true);
    connect(&idleTimer_, &QTimer::timeout, this, [this] { fail(QStringLiteral("idle timeout")); });
    sinceFrame_.start();  // the handshake's message 1 was the last frame received
    armIdle();
}

void CompanionClient::requestCatalog(QString storeVersion, const quint64 generation) {
    const auto max_items = static_cast<std::uint32_t>(link::max_catalog_items);
    enqueue({link::MsgType::CatalogSince, link::CatalogSince{storeVersion.toStdString(), generation, max_items}, {}});
}

void CompanionClient::requestPreviews(QList<quint64> ids) {
    if (!ids.isEmpty()) {
        enqueue({link::MsgType::PreviewRequest, {}, {ids.begin(), ids.end()}});
    }
}

void CompanionClient::requestKeyframes(const quint64 id, const int count) {
    if (count < 1 || count > static_cast<int>(link::max_keyframes)) {
        throw std::invalid_argument("keyframe count must be 1 to 8");
    }
    enqueue({link::MsgType::KeyframesRequest, link::KeyframesRequest{id, static_cast<std::uint8_t>(count)}, {}});
}

void CompanionClient::requestHashes(QList<quint64> ids) {
    if (!ids.isEmpty()) {
        enqueue({link::MsgType::HashRequest, {}, {ids.begin(), ids.end()}});
    }
}

void CompanionClient::requestTrash(QList<link::TrashItem> items) {
    for (qsizetype at = 0; at < items.size(); at += static_cast<qsizetype>(link::max_trash_items)) {
        const auto chunk = items.mid(at, static_cast<qsizetype>(link::max_trash_items));
        enqueue({link::MsgType::TrashRequest, link::TrashRequest{{chunk.begin(), chunk.end()}}, {}});
    }
}

void CompanionClient::setIdleTimeouts(const std::chrono::milliseconds normal,
                                      const std::chrono::milliseconds whileTrashing) {
    idleLimit_ = normal;
    trashIdleLimit_ = whileTrashing;
    armIdle();
}

CompanionClient::Unanswered CompanionClient::unanswered() const {
    Unanswered left;
    const auto add = [&left](const Request& r, const bool sent) {
        switch (r.type) {
        case link::MsgType::CatalogSince: left.catalog = true; break;
        case link::MsgType::PreviewRequest: left.previews.append({r.ids.begin(), r.ids.end()}); break;
        case link::MsgType::HashRequest: left.hashes.append({r.ids.begin(), r.ids.end()}); break;
        case link::MsgType::KeyframesRequest: left.keyframes.push_back(std::get<link::KeyframesRequest>(r.body)); break;
        case link::MsgType::TrashRequest: {
            const auto& items = std::get<link::TrashRequest>(r.body).items;
            (sent ? left.trashInFlight : left.trashQueued).append({items.begin(), items.end()});
            break;
        }
        default: break;
        }
    };
    for (const auto& [id, r] : inflight_) {
        add(r, true);  // sent first, so they come first
    }
    for (const auto& r : queue_) {
        add(r, false);
    }
    return left;
}

void CompanionClient::enqueue(Request request) {
    queue_.push_back(std::move(request));
    pump();
}

// Sends queued requests in order while credits allow. An id list takes as many ids as there are free credits (at most
// 16 per request), so the phone always has work; the next TrashRequest goes out after the previous TrashResult.
void CompanionClient::pump() {
    while (hello_ && !failed_ && !queue_.empty()) {
        Request& next = queue_.front();
        const std::size_t free = credit_window - credits_;
        Request sent;
        link::Body body;
        if (is_id_list(next.type)) {
            const auto take = static_cast<std::ptrdiff_t>(std::min({free, next.ids.size(), link::max_request_ids}));
            if (take == 0) {
                return;
            }
            sent.type = next.type;
            sent.ids.assign(next.ids.begin(), next.ids.begin() + take);
            next.ids.erase(next.ids.begin(), next.ids.begin() + take);
            body = link::IdList{sent.ids};
            if (next.ids.empty()) {
                queue_.pop_front();
            }
        } else {
            if (free == 0 || (next.type == link::MsgType::TrashRequest && trashInFlight_)) {
                return;
            }
            sent = std::move(next);
            queue_.pop_front();
            body = sent.body;
        }
        const auto id = nextRequestId();
        if (inflight_.empty()) {
            sinceFrame_.restart();  // a request after a quiet spell gets a full idle window for its answer
        }
        credits_ += is_id_list(sent.type) ? sent.ids.size() : 1;
        trashInFlight_ = trashInFlight_ || sent.type == link::MsgType::TrashRequest;
        const auto type = sent.type;
        inflight_.emplace(id, std::move(sent));
        armIdle();
        send({type, id, std::move(body)});
    }
}

std::uint32_t CompanionClient::nextRequestId() {
    do {
        ++lastRequestId_;
    } while (lastRequestId_ == 0 || inflight_.contains(lastRequestId_));
    return lastRequestId_;
}

void CompanionClient::onBytes(const QByteArray& bytes) {
    for (qsizetype at = 0; at < bytes.size() && !failed_; at += max_read) {
        const auto size = static_cast<std::size_t>(std::min(max_read, bytes.size() - at));
        if (!decoder_.feed(std::as_bytes(std::span{bytes.constData() + at, size}))) {
            violation(link::ErrorCode::Malformed, QStringLiteral("malformed frame length"));
            return;
        }
        while (!failed_) {
            const auto frame = decoder_.next();
            if (!frame) {
                break;
            }
            onFrame(*frame);
        }
    }
}

void CompanionClient::onFrame(const std::vector<std::byte>& frame) {
    const auto plaintext = session_->open(frame);
    if (!plaintext) {
        fail(QStringLiteral("a frame failed to decrypt"));  // the session is dead: no Error can be sealed
        return;
    }
    auto message = link::decode(*plaintext);
    if (!message) {
        violation(link::ErrorCode::Malformed, QStringLiteral("malformed message"));
        return;
    }
    // A session-level Error is not activity: a phone that sends nothing else still reaches the idle limit.
    if (message->type != link::MsgType::Error || message->request_id != 0) {
        sinceFrame_.restart();
        armIdle();
    }
    onMessage(std::move(*message));
}

void CompanionClient::onMessage(link::Message message) {
    using link::MsgType;
    switch (message.type) {
    case MsgType::Bye:
        fail(QStringLiteral("the phone ended the session"));
        return;
    case MsgType::Error:
        onError(message.request_id, std::get<link::ErrorMsg>(message.body));
        return;
    case MsgType::Hello:
        onHello(message);
        return;
    case MsgType::CatalogPage:
    case MsgType::Preview:
    case MsgType::Keyframes:
    case MsgType::Hash:
    case MsgType::TrashResult:
        if (hello_) {
            onResponse(std::move(message));
            return;
        }
        break;
    default:
        break;  // HelloAck and requests only travel from the desktop to the phone
    }
    violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("unexpected message"));
}

void CompanionClient::onHello(const link::Message& message) {
    if (hello_ || message.request_id != 0) {
        violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("unexpected Hello"));
        return;
    }
    const auto& hello = std::get<link::Hello>(message.body);
    if (hello.protocol != 1) {
        violation(link::ErrorCode::UnsupportedProtocol, QStringLiteral("unsupported protocol version"));
        return;
    }
    send({link::MsgType::HelloAck, 0, link::HelloAck{1, QSysInfo::machineHostName().left(255).toStdString()}});
    if (failed_) {
        return;
    }
    hello_ = true;
    emit helloReceived(hello);
    pump();
}

// Matches a response to its outstanding request, returns its credit and hands it on. Anything that does not answer
// an outstanding request exactly (type, id, count, order) is a violation.
void CompanionClient::onResponse(link::Message message) {
    using link::MsgType;
    const auto it = inflight_.find(message.request_id);
    if (it == inflight_.end() || answer_to(it->second.type) != message.type) {
        violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("a response to no outstanding request"));
        return;
    }
    Request& request = it->second;
    bool done = true;
    bool valid = true;
    switch (message.type) {
    case MsgType::Preview:
    case MsgType::Hash: {
        const auto id = message.type == MsgType::Preview ? std::get<link::Preview>(message.body).id
                                                         : std::get<link::Hash>(message.body).id;
        const auto pos = std::ranges::find(request.ids, id);
        valid = pos != request.ids.end();
        if (valid) {
            request.ids.erase(pos);
            done = request.ids.empty();
        }
        break;
    }
    case MsgType::CatalogPage:
        valid = std::get<link::CatalogPage>(message.body).items.size() <=
                std::get<link::CatalogSince>(request.body).max_items;
        break;
    case MsgType::Keyframes: {
        const auto& asked = std::get<link::KeyframesRequest>(request.body);
        const auto& frames = std::get<link::Keyframes>(message.body);
        valid = frames.id == asked.id && frames.jpegs.size() <= asked.count;
        break;
    }
    case MsgType::TrashResult:
        valid = std::ranges::equal(std::get<link::TrashResult>(message.body).items,
                                   std::get<link::TrashRequest>(request.body).items,
                                   [](const auto& entry, const auto& item) { return entry.first == item.id; });
        break;
    default:
        break;
    }
    if (!valid) {
        violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("a response that does not match its request"));
        return;
    }
    --credits_;
    if (done) {
        if (request.type == MsgType::TrashRequest) {
            trashInFlight_ = false;
            armIdle();
        }
        inflight_.erase(it);
    }
    switch (message.type) {
    case MsgType::CatalogPage: emit catalogPage(std::get<link::CatalogPage>(std::move(message.body))); break;
    case MsgType::Preview: emit preview(std::get<link::Preview>(std::move(message.body))); break;
    case MsgType::Keyframes: emit keyframes(std::get<link::Keyframes>(std::move(message.body))); break;
    case MsgType::Hash: emit hash(std::get<link::Hash>(std::move(message.body))); break;
    case MsgType::TrashResult: emit trashResult(std::get<link::TrashResult>(std::move(message.body))); break;
    default: break;
    }
    pump();
}

void CompanionClient::onError(const std::uint32_t requestId, const link::ErrorMsg& error) {
    const auto text = QString::fromStdString(error.message);
    if (error.code >= static_cast<std::uint16_t>(link::ErrorCode::Malformed) &&
        error.code <= static_cast<std::uint16_t>(link::ErrorCode::UnexpectedMessage)) {
        fail(QStringLiteral("phone error %1: %2").arg(error.code).arg(text));  // the phone closes after these
        return;
    }
    if (requestId == 0) {
        if (!hello_) {
            violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("an Error before Hello"));
        } else if (++sessionErrors_ > max_session_errors) {
            fail(QStringLiteral("too many session errors from the phone"));
        } else {
            qWarning().noquote() << "phone session error" << error.code << text;  // the session stays open
        }
        return;
    }
    const auto it = inflight_.find(requestId);
    if (it == inflight_.end()) {
        violation(link::ErrorCode::UnexpectedMessage, QStringLiteral("an Error for no outstanding request"));
        return;
    }
    const Request request = std::move(it->second);
    inflight_.erase(it);
    QList<quint64> ids;
    switch (request.type) {
    case link::MsgType::PreviewRequest:
    case link::MsgType::HashRequest:
        credits_ -= request.ids.size();
        ids.assign(request.ids.begin(), request.ids.end());
        break;
    case link::MsgType::KeyframesRequest:
        --credits_;
        ids.push_back(std::get<link::KeyframesRequest>(request.body).id);
        break;
    case link::MsgType::TrashRequest:
        --credits_;
        for (const auto& item : std::get<link::TrashRequest>(request.body).items) {
            ids.push_back(item.id);
        }
        trashInFlight_ = false;
        armIdle();
        break;
    default:
        --credits_;
        break;
    }
    emit requestError(request.type, ids, error.code, text);
    pump();
}

void CompanionClient::send(const link::Message& message) {
    if (failed_ || !stream_) {
        return;
    }
    try {
        const auto frame = link::encode_frame(session_->seal(link::encode(message)));
        stream_->write(QByteArray(reinterpret_cast<const char*>(frame.data()), static_cast<qsizetype>(frame.size())));
    } catch (const std::exception& e) {
        fail(QStringLiteral("cannot send: %1").arg(QString::fromUtf8(e.what())));
    }
}

void CompanionClient::violation(const link::ErrorCode code, const QString& reason) {
    send({link::MsgType::Error, 0, link::ErrorMsg{static_cast<std::uint16_t>(code), reason.toStdString()}});
    fail(reason);
}

void CompanionClient::fail(const QString& reason) {
    if (failed_) {
        return;
    }
    failed_ = true;
    idleTimer_.stop();
    if (stream_) {
        disconnect(stream_, nullptr, this, nullptr);
        stream_->close();
    }
    emit failed(reason);
}

void CompanionClient::armIdle() {
    if (failed_) {
        return;
    }
    const auto limit = trashInFlight_ ? trashIdleLimit_ : idleLimit_;
    const auto left = limit - std::chrono::milliseconds{sinceFrame_.elapsed()};
    idleTimer_.start(std::max(left, std::chrono::milliseconds{0}));
}

}  // namespace pms::desktop
