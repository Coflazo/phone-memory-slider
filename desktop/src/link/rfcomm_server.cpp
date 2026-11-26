#include "link/rfcomm_server.hpp"

#include <QBluetoothAddress>
#include <QBluetoothLocalDevice>
#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QDateTime>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pms/link/frames.hpp"
#include "pms/link/qr.hpp"

namespace pms::desktop {

namespace {

constexpr std::uint32_t pairing_window_s = 120;  // PROTOCOL.md section 3
constexpr qint64 max_read = 64 * 1024;           // PROTOCOL.md section 10: reads of at most 64 KiB

[[nodiscard]] std::uint32_t now_unix_s() {
    return static_cast<std::uint32_t>(QDateTime::currentSecsSinceEpoch());
}

[[nodiscard]] QByteArray to_qbytes(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}

// One accepted RFCOMM socket on a thread of its own: Qt's WinRT backend blocks the caller of QBluetoothSocket::write
// until the bytes are out. The socket and a plain QObject that owns it live on that thread; this object stays on the
// GUI thread and talks to it only through queued calls, so the client never waits on the radio.
class SocketStream final : public ByteStream {
public:
    explicit SocketStream(QBluetoothSocket* socket) : worker_(new QObject), socket_(socket) {
        socket->setParent(worker_);
        const auto drain = [this, socket] {
            while (socket->bytesAvailable() > 0) {
                const QByteArray read = socket->read(max_read);
                if (read.isEmpty()) {
                    break;
                }
                QMetaObject::invokeMethod(
                    this, [this, read] { if (open_) emit bytesReceived(read); }, Qt::QueuedConnection);
            }
        };
        const auto gone = [this](const QString& reason) {
            QMetaObject::invokeMethod(
                this,
                [this, reason] {
                    if (open_) {
                        open_ = false;
                        emit closed(reason);
                    }
                },
                Qt::QueuedConnection);
        };
        QObject::connect(socket, &QBluetoothSocket::readyRead, worker_, drain);
        QObject::connect(socket, &QBluetoothSocket::disconnected, worker_,
                         [gone] { gone(QStringLiteral("the phone disconnected")); });
        QObject::connect(socket, &QBluetoothSocket::errorOccurred, worker_,
                         [gone, socket] { gone(socket->errorString()); });
        worker_->moveToThread(&thread_);  // takes the socket with it
        QObject::connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
        thread_.start();
        // Bytes or a hang-up that arrived before the connections above were made.
        QMetaObject::invokeMethod(
            worker_,
            [drain, gone, socket] {
                drain();
                if (socket->state() == QBluetoothSocket::SocketState::UnconnectedState) {
                    gone(QStringLiteral("the phone disconnected"));
                }
            },
            Qt::QueuedConnection);
    }

    // ponytail: waits for a write that is blocked in the radio to return; a Winsock server could cancel it instead.
    ~SocketStream() override {
        thread_.quit();
        thread_.wait();  // the worker, and with it the socket, is deleted on the thread as it finishes
    }

    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    void write(QByteArray bytes) override {
        if (open_) {
            QMetaObject::invokeMethod(
                worker_, [socket = socket_, bytes] { socket->write(bytes); }, Qt::QueuedConnection);
        }
    }

    void close() override {
        if (!open_) {
            return;
        }
        open_ = false;
        // Queued behind the writes already made, so they go out first.
        QMetaObject::invokeMethod(worker_, [socket = socket_] { socket->close(); }, Qt::QueuedConnection);
        emit closed(QStringLiteral("closed locally"));
    }

private:
    QThread thread_;
    QObject* worker_;           // lives on thread_, owns socket_
    QBluetoothSocket* socket_;  // touched only on thread_
    bool open_{true};           // GUI thread only
};

}  // namespace

namespace detail {

// One accepted connection: its handshake state, then its client. A child of the server; deleted later, never inside a
// signal it is handling.
class Connection final : public QObject {
public:
    Connection(QObject* parent, ByteStream* stream_, std::shared_ptr<link::PairingWindow> window_,
               std::unique_ptr<link::ResponderSession> session_, const ConnectionGuard::Clock::time_point started_)
        : QObject(parent), stream(stream_), window(std::move(window_)), session(std::move(session_)), started(started_) {}

    ~Connection() override {
        client.reset();  // it holds a pointer to session and is connected to stream
        delete stream;
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ByteStream* stream;                              // owned
    std::shared_ptr<link::PairingWindow> window;     // kept alive while session may use it
    std::unique_ptr<link::ResponderSession> session;
    ConnectionGuard::Clock::time_point started;
    link::FrameDecoder preauth{link::max_preauth_frame};  // one frame of at most 512 bytes before the handshake
    QTimer timeout;
    std::unique_ptr<CompanionClient> client;
};

}  // namespace detail

bool ConnectionGuard::allow_attempt(const Clock::time_point now) {
    if (last_attempt_ && now - *last_attempt_ < attempt_gap) {
        return false;
    }
    last_attempt_ = now;
    return true;
}

bool ConnectionGuard::handshake_expired(const Clock::time_point started, const Clock::time_point now) const {
    return now - started > handshake_limit;
}

RfcommServer::RfcommServer(link::KeyPair desktop, link::PinStore& pins, QObject* parent)
    : QObject(parent), desktop_(desktop), pins_(pins) {}

// The radio closes with radio_; each connection, a child, closes its stream as QObject deletes it.
RfcommServer::~RfcommServer() {
    volatile auto* key = desktop_.private_key.data();
    for (std::size_t i = 0; i < desktop_.private_key.size(); ++i) {
        key[i] = 0;
    }
}

std::optional<std::array<std::uint8_t, 6>> RfcommServer::start(const QBluetoothUuid& service) {
    stop();
    const QBluetoothLocalDevice local;
    if (!local.isValid()) {
        return std::nullopt;
    }
    auto radio = std::make_unique<QBluetoothServer>(QBluetoothServiceInfo::RfcommProtocol);
    radio->setSecurityFlags(QBluetooth::Security::NoSecurity);  // insecure RFCOMM: no OS pairing (PROTOCOL.md section 2)
    connect(radio.get(), &QBluetoothServer::newConnection, this, [this] {
        while (radio_ && radio_->hasPendingConnections()) {
            QBluetoothSocket* socket = radio_->nextPendingConnection();
            socket->setParent(nullptr);  // it moves to a thread of its own
            accept(new SocketStream(socket));
        }
    });
    if (!radio->listen(service, QStringLiteral("Phone Memory Slider")).isValid()) {
        return std::nullopt;
    }
    radio_ = std::move(radio);
    const auto address = local.address().toUInt64();
    std::array<std::uint8_t, 6> mac{};
    for (std::size_t i = 0; i < mac.size(); ++i) {
        mac[i] = static_cast<std::uint8_t>(address >> (8 * (mac.size() - 1 - i)));  // printed order, AA first
    }
    listenOn(service, mac);
    return mac;
}

void RfcommServer::startWithoutRadio(const QBluetoothUuid& service) {
    stop();
    listenOn(service, {});
}

void RfcommServer::listenOn(const QBluetoothUuid& service, const std::array<std::uint8_t, 6>& mac) {
    const QByteArray uuid = service.toRfc4122();
    std::memcpy(uuid_.data(), uuid.constData(), uuid_.size());
    mac_ = mac;
    listening_ = true;
}

void RfcommServer::stop() {
    listening_ = false;
    radio_.reset();  // unregisters the SDP record and closes the listening socket
    hidePairingCode();
    if (live_ && !live_->client) {
        drop(live_, QStringLiteral("the server stopped"));
    }
}

QString RfcommServer::showPairingCode() {
    if (!listening_) {
        throw std::logic_error("start the server before showing a pairing code");
    }
    hidePairingCode();
    link::PairingCode code;
    code.mac = mac_;
    code.service_uuid = uuid_;
    code.desktop_key = desktop_.public_key;
    std::array<quint32, 4> words{};
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));  // the OS CSPRNG
    std::memcpy(code.token.data(), words.data(), code.token.size());
    code.expiry_unix_s = now_unix_s() + pairing_window_s;
    window_ = std::make_shared<link::PairingWindow>(code.token, code.expiry_unix_s);
    return QString::fromStdString(link::encode_pairing(code));
}

void RfcommServer::hidePairingCode() {
    if (window_) {
        window_->burn();  // a connection still holding it sees it burned
        window_.reset();
    }
}

void RfcommServer::setHandshakeTimeout(const std::chrono::milliseconds limit) {
    guard_.handshake_limit = limit;
}

void RfcommServer::accept(ByteStream* stream) {
    const auto refuse = [this, stream](const QString& reason) {
        stream->close();
        stream->deleteLater();
        emit rejected(reason);
    };
    if (!listening_) {
        refuse(QStringLiteral("not listening"));
        return;
    }
    if (live_) {
        refuse(QStringLiteral("busy: one session at a time"));
        return;
    }
    const auto now = ConnectionGuard::Clock::now();
    if (!guard_.allow_attempt(now)) {
        refuse(QStringLiteral("rate limit: one handshake attempt per second"));
        return;
    }
    auto* c = new detail::Connection(this, stream, window_,
                                     std::make_unique<link::ResponderSession>(desktop_, uuid_, pins_), now);
    if (c->window) {
        c->session->open_pairing_window(*c->window);
    }
    live_ = c;
    connect(stream, &ByteStream::bytesReceived, c, [this, c](const QByteArray& bytes) { onPreauthBytes(c, bytes); });
    connect(stream, &ByteStream::closed, c,
            [this, c](const QString& why) { drop(c, QStringLiteral("closed during the handshake: %1").arg(why)); });
    c->timeout.setSingleShot(true);
    connect(&c->timeout, &QTimer::timeout, c, [this, c] { drop(c, QStringLiteral("handshake timeout")); });
    c->timeout.start(guard_.handshake_limit);
}

void RfcommServer::onPreauthBytes(detail::Connection* c, const QByteArray& bytes) {
    if (guard_.handshake_expired(c->started, ConnectionGuard::Clock::now())) {
        drop(c, QStringLiteral("handshake timeout"));
        return;
    }
    // The decoder checks each length before allocating and fails on a second frame: the phone sends nothing after
    // message 1 until it has message 2.
    if (!c->preauth.feed(std::as_bytes(std::span{bytes.constData(), static_cast<std::size_t>(bytes.size())}))) {
        drop(c, QStringLiteral("garbage before the handshake"));
        return;
    }
    const auto msg1 = c->preauth.next();
    if (!msg1) {
        return;  // the rest of message 1 is still on its way
    }
    std::vector<std::byte> msg2;
    link::HandshakeResult result{};
    try {
        result = c->session->on_message1(*msg1, now_unix_s(), msg2);
    } catch (const std::exception& e) {
        drop(c, QStringLiteral("pin store failed: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    if (result != link::HandshakeResult::Established) {
        drop(c, QStringLiteral("handshake refused"));
        return;
    }
    disconnect(c->stream, nullptr, c, nullptr);
    c->timeout.stop();
    c->stream->write(to_qbytes(link::encode_frame(msg2)));
    // The phone's Hello may follow message 2 at once: the client takes the stream before control returns to the
    // event loop, with its own decoder at the full 65,535-byte cap now that the session is established.
    c->client = std::make_unique<CompanionClient>(c->stream, c->session.get());
    connect(c->client.get(), &CompanionClient::failed, c, [this, c] { end(c); });
    const auto& phone = c->session->peer();
    const QString sas = c->session->first_pairing() ? QString::fromStdString(c->session->sas()) : QString{};
    emit sessionStarted(c->client.get(), QString::fromStdString(phone.model), sas);
}

void RfcommServer::drop(detail::Connection* c, const QString& reason) {
    if (live_ != c) {
        return;
    }
    live_ = nullptr;
    disconnect(c->stream, nullptr, c, nullptr);
    c->timeout.stop();
    c->stream->close();
    c->deleteLater();
    emit rejected(reason);
}

void RfcommServer::end(detail::Connection* c) {
    if (live_ == c) {
        live_ = nullptr;
    }
    c->deleteLater();  // after every slot of the client's failed() has run
}

}  // namespace pms::desktop
