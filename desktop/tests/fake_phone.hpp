#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "link/byte_stream.hpp"
#include "pms/link/frames.hpp"
#include "pms/link/messages.hpp"
#include "pms/link/session.hpp"

namespace pms::desktop::testing {

// Pins kept in memory, for tests.
class MemoryPins final : public pms::link::PinStore {
public:
    [[nodiscard]] std::optional<pms::link::Pinned> find(const pms::link::Key32& key) const override {
        const auto it = pins_.find(key);
        return it == pins_.end() ? std::nullopt : std::optional{it->second};
    }
    void pin(const pms::link::Pinned& p) override { pins_[p.phone_key] = p; }
    void unpin(const pms::link::Key32& key) override { pins_.erase(key); }
    [[nodiscard]] std::size_t size() const noexcept { return pins_.size(); }

private:
    std::map<pms::link::Key32, pms::link::Pinned> pins_;
};

// A scripted phone: a real Noise IK initiator (first pairing) on one end of a ByteStream, serving a generated catalog
// of ids 1..catalogSize with previews, keyframes, hashes and trash outcomes. It answers one response per event loop
// turn, so requests pile up as they would on a slow link, and it enforces the 16-credit rule as the protocol says:
// Error FlowControl, then close.
class FakePhone final : public QObject {
    Q_OBJECT

public:
    struct Script {
        std::uint64_t catalogSize{0};
        std::string helloModel{"Pixel 9"};  // encoded by hand, so a test can break the 1024-byte string cap
        std::uint16_t helloProtocol{1};
        bool sendHello{true};               // false: the test sends whatever comes first itself
        QSet<quint64> declineTrash;         // the user declines Android's trash dialog for these
        QSet<quint64> busyKeyframes;        // a KeyframesRequest for these is ended by Error Busy
        int trashDelayMs{0};                // time the user spends in the trash dialog
        bool burst{false};                  // answer everything queued in one write: many frames in one read
    };

    FakePhone(ByteStream* stream, const pms::link::Key32& desktopKey, const std::array<std::uint8_t, 16>& serviceUuid,
              const std::array<std::uint8_t, 16>& token, Script script, QObject* parent = nullptr);

    void start();                                  // sends message 1
    void deleteItem(quint64 id);                   // gone from the phone from now on
    void corruptFrameAt(std::size_t n);            // flips a ciphertext bit in the n-th transport frame sent (Hello = 1)
    void sendMessage(const pms::link::Message& m); // sends m as is, outside the script
    void hangUp();                                 // closes the phone's end of the stream

    [[nodiscard]] std::size_t maxOutstanding() const noexcept { return maxOutstanding_; }
    [[nodiscard]] const std::vector<std::size_t>& trashRequestSizes() const noexcept { return trashSizes_; }
    [[nodiscard]] std::optional<quint16> errorReceived() const noexcept { return errorReceived_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool established() const noexcept { return established_; }  // message 2 accepted

private:
    void onBytes(const QByteArray& bytes);
    void onMessage(const pms::link::Message& m);
    bool admit(std::size_t credits);  // false: over 16 credits, the link is closed
    void respond(const pms::link::Message& m);
    void sendPlain(const std::vector<std::byte>& plaintext);
    void sendFrame(const std::vector<std::byte>& payload);
    void serveOne();
    void closeLink();
    [[nodiscard]] bool exists(std::uint64_t id) const;
    [[nodiscard]] pms::link::CatalogPage page(const pms::link::CatalogSince& since) const;

    QPointer<ByteStream> stream_;
    std::array<std::uint8_t, 16> token_;
    Script script_;
    pms::link::InitiatorSession session_;
    pms::link::FrameDecoder decoder_{pms::link::max_preauth_frame};
    bool established_{false};
    bool helloAcked_{false};
    bool closed_{false};
    std::deque<std::function<void()>> jobs_;
    QTimer serve_;
    std::size_t transportSent_{0};
    std::size_t corruptAt_{0};
    bool batching_{false};
    QByteArray batch_;
    std::size_t outstanding_{0};
    std::size_t maxOutstanding_{0};
    std::vector<std::size_t> trashSizes_;
    std::optional<quint16> errorReceived_;
    QSet<quint64> deleted_;
};

}  // namespace pms::desktop::testing
