#pragma once

#include <QBluetoothUuid>
#include <QObject>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "link/byte_stream.hpp"
#include "link/companion_client.hpp"
#include "pms/link/session.hpp"

class QBluetoothServer;

namespace pms::desktop {

namespace detail {
class Connection;
}

// The connection limits of PROTOCOL.md section 2 as pure functions of the time, tested without Bluetooth. The idle
// timeout is not here: CompanionClient owns it.
class ConnectionGuard {
public:
    using Clock = std::chrono::steady_clock;

    // True at most once per attempt_gap; only an allowed attempt starts the next gap.
    [[nodiscard]] bool allow_attempt(Clock::time_point now);
    [[nodiscard]] bool handshake_expired(Clock::time_point started, Clock::time_point now) const;

    std::chrono::milliseconds attempt_gap{1000};
    std::chrono::milliseconds handshake_limit{5000};

private:
    std::optional<Clock::time_point> last_attempt_;
};

// The desktop end of the phone link: an insecure RFCOMM listener (PROTOCOL.md section 2) that runs the Noise handshake
// on each connection and hands an established one to a CompanionClient. One connection at a time, handshaking or
// established; any other is refused. Before the handshake completes it reads one frame of at most 512 bytes; the
// handshake must finish within 5 s, and at most one attempt per second gets that far.
//
// Each pairing QR code has one PairingWindow, created by showPairingCode() and shared by every connection accepted
// while the code is up; a connection keeps its window alive until it ends. A first pairing burns it, and so do
// hidePairingCode(), a newer code and stop().
//
// The Bluetooth part (QBluetoothServer, one socket per QThread) stays inside this class, so spike 2 can swap it for a
// Winsock AF_BTH server without touching callers.
class RfcommServer : public QObject {
    Q_OBJECT

public:
    // pins must outlive the server.
    RfcommServer(pms::link::KeyPair desktop, pms::link::PinStore& pins, QObject* parent = nullptr);
    ~RfcommServer() override;  // wipes its copy of the desktop private key
    RfcommServer(const RfcommServer&) = delete;
    RfcommServer& operator=(const RfcommServer&) = delete;

    // Registers the SDP record and listens; returns the local adapter MAC for the QR, or nullopt without a usable
    // adapter. Listening stops on stop().
    [[nodiscard]] std::optional<std::array<std::uint8_t, 6>> start(const QBluetoothUuid& service);
    // Stops listening, drops a handshake in progress and burns the pairing window. An established session goes on
    // until its client fails.
    void stop();

    // The text of a new pairing QR code: a fresh random token valid for 120 s, in a window of its own. The previous
    // window is burned. Throws std::logic_error unless listening.
    [[nodiscard]] QString showPairingCode();
    void hidePairingCode();  // the code on screen can no longer pair

    // Test seam: listen as start() does, without Bluetooth (MAC 00:00:00:00:00:00).
    void startWithoutRadio(const QBluetoothUuid& service);
    // A new connection. The radio hands every accepted socket here; tests hand PipeStream ends. The server owns stream
    // from now on and deletes it when the connection ends.
    void accept(ByteStream* stream);
    void setHandshakeTimeout(std::chrono::milliseconds limit);  // 5 s unless a test shortens it

signals:
    // A phone completed the handshake. The server owns client and deletes it, with the stream, after the client emits
    // failed() (use deleteLater semantics: unanswered() can still be read in a slot of failed()). sas is the 6-digit
    // code to show after a first pairing, empty on a reconnect.
    void sessionStarted(pms::desktop::CompanionClient* client, QString phoneModel, QString sas);
    // A connection refused or dropped before its session started: not listening, busy, rate limit, garbage, timeout,
    // handshake refused, or closed by the phone.
    void rejected(QString reason);

private:
    void listenOn(const QBluetoothUuid& service, const std::array<std::uint8_t, 6>& mac);
    void onPreauthBytes(detail::Connection* c, const QByteArray& bytes);
    void drop(detail::Connection* c, const QString& reason);
    void end(detail::Connection* c);

    pms::link::KeyPair desktop_;
    pms::link::PinStore& pins_;
    std::array<std::uint8_t, 16> uuid_{};
    std::array<std::uint8_t, 6> mac_{};
    bool listening_{false};
    std::shared_ptr<pms::link::PairingWindow> window_;
    ConnectionGuard guard_;
    std::unique_ptr<QBluetoothServer> radio_;
    detail::Connection* live_{nullptr};  // a child of this server
};

}  // namespace pms::desktop
