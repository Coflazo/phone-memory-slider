#include "fake_phone.hpp"

#include <QByteArray>

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace pms::desktop::testing {

namespace {

namespace pl = pms::link;

constexpr std::size_t credit_window = 16;  // PROTOCOL.md section 7

// Hello by hand rather than through encode(), which refuses a model over 1024 bytes.
std::vector<std::byte> hello_bytes(const std::string_view model, const std::uint16_t protocol) {
    std::vector<std::byte> b;
    const auto put = [&b](const std::uint64_t value, const int size) {
        for (int i = size - 1; i >= 0; --i) {
            b.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
        }
    };
    const auto str = [&](const std::string_view text) {
        put(text.size(), 2);
        for (const char c : text) {
            b.push_back(static_cast<std::byte>(c));
        }
    };
    put(static_cast<std::uint8_t>(pl::MsgType::Hello), 1);
    put(0, 4);  // request_id
    put(protocol, 2);
    str(model);
    str("Android 15");
    str("2.0.0-test");
    put(0, 4);  // capabilities
    return b;
}

pl::CatalogItem catalog_item(const std::uint64_t id) {
    pl::CatalogItem item;
    item.id = id;
    item.media_type = 1;
    item.relative_path = "DCIM/Camera/IMG_" + std::to_string(id) + ".jpg";
    item.mime = "image/jpeg";
    item.size = 1'000'000 + id;
    item.width = 4000;
    item.height = 3000;
    item.date_taken_ms = 1'700'000'000'000 + static_cast<std::int64_t>(id);
    item.date_modified_s = 1'700'000'000 + static_cast<std::int64_t>(id);
    item.generation_modified = id;  // one item per generation
    return item;
}

std::vector<std::byte> fake_jpeg(const std::uint64_t id, const std::size_t size) {
    return std::vector<std::byte>(size, static_cast<std::byte>(id & 0xFFU));
}

}  // namespace

FakePhone::FakePhone(ByteStream* stream, const pl::Key32& desktopKey, const std::array<std::uint8_t, 16>& serviceUuid,
                     const std::array<std::uint8_t, 16>& token, Script script, QObject* parent)
    : QObject(parent), stream_(stream), token_(token), script_(std::move(script)),
      session_(pl::generate_keypair(), desktopKey, serviceUuid) {
    connect(stream, &ByteStream::bytesReceived, this, &FakePhone::onBytes);
    connect(stream, &ByteStream::closed, this, [this] {
        closed_ = true;
        serve_.stop();
    });
    connect(&serve_, &QTimer::timeout, this, &FakePhone::serveOne);
    serve_.setInterval(0);
}

void FakePhone::start() {
    sendFrame(session_.message1_first_pairing(token_, "Pixel 9"));
}

void FakePhone::deleteItem(const quint64 id) {
    deleted_.insert(id);
}

void FakePhone::corruptFrameAt(const std::size_t n) {
    corruptAt_ = n;
}

void FakePhone::sendMessage(const pl::Message& m) {
    sendPlain(pl::encode(m));
}

void FakePhone::hangUp() {
    closeLink();
}

bool FakePhone::exists(const std::uint64_t id) const {
    return id >= 1 && id <= script_.catalogSize && !deleted_.contains(id);
}

void FakePhone::onBytes(const QByteArray& bytes) {
    if (closed_) {
        return;
    }
    if (!decoder_.feed(std::as_bytes(std::span{bytes.constData(), static_cast<std::size_t>(bytes.size())}))) {
        closeLink();
        return;
    }
    while (!closed_) {
        const auto frame = decoder_.next();
        if (!frame) {
            return;
        }
        if (!established_) {
            if (!session_.on_message2(*frame)) {
                closeLink();
                return;
            }
            established_ = true;
            decoder_.set_max_payload(pl::max_frame);
            if (script_.sendHello) {
                sendPlain(hello_bytes(script_.helloModel, script_.helloProtocol));
            }
            continue;
        }
        const auto plaintext = session_.open(*frame);
        const auto message = plaintext ? pl::decode(*plaintext) : std::nullopt;
        if (!message) {
            closeLink();
            return;
        }
        onMessage(*message);
    }
}

void FakePhone::onMessage(const pl::Message& m) {
    using pl::MsgType;
    if (m.type == MsgType::HelloAck) {
        helloAcked_ = true;
        return;
    }
    if (m.type == MsgType::Error) {
        errorReceived_ = std::get<pl::ErrorMsg>(m.body).code;
        return;
    }
    if (m.type == MsgType::Bye || !helloAcked_) {
        closeLink();  // also: a request before HelloAck breaks the session order
        return;
    }
    const auto id = m.request_id;
    switch (m.type) {
    case MsgType::CatalogSince:
        if (admit(1)) {
            jobs_.push_back([this, id, since = std::get<pl::CatalogSince>(m.body)] {
                respond({MsgType::CatalogPage, id, page(since)});
            });
        }
        break;
    case MsgType::PreviewRequest:
    case MsgType::HashRequest: {
        const auto& ids = std::get<pl::IdList>(m.body).ids;
        if (!admit(ids.size())) {
            return;
        }
        const bool preview = m.type == MsgType::PreviewRequest;
        for (const auto item : ids) {
            jobs_.push_back([this, id, item, preview] {
                const auto status = exists(item) ? pl::ItemStatus::Ok : pl::ItemStatus::Missing;
                const bool ok = status == pl::ItemStatus::Ok;
                if (preview) {
                    respond({MsgType::Preview, id,
                             pl::Preview{item, status, ok ? 256U : 0U, ok ? 192U : 0U, ok ? fake_jpeg(item, 1024) : std::vector<std::byte>{}}});
                } else {
                    pl::Hash hash{item, status, {}};
                    if (ok) {
                        hash.sha256.fill(static_cast<std::uint8_t>(item & 0xFFU));
                    }
                    respond({MsgType::Hash, id, hash});
                }
            });
        }
        break;
    }
    case MsgType::KeyframesRequest:
        if (admit(1)) {
            jobs_.push_back([this, id, request = std::get<pl::KeyframesRequest>(m.body)] {
                if (script_.busyKeyframes.contains(request.id)) {
                    respond({MsgType::Error, id,
                             pl::ErrorMsg{static_cast<std::uint16_t>(pl::ErrorCode::Busy), "decoder busy"}});
                    return;
                }
                pl::Keyframes frames{request.id, exists(request.id) ? pl::ItemStatus::Ok : pl::ItemStatus::Missing, {}};
                for (std::uint8_t i = 0; frames.status == pl::ItemStatus::Ok && i < request.count; ++i) {
                    frames.jpegs.push_back(fake_jpeg(request.id + i, 512));
                }
                respond({MsgType::Keyframes, id, frames});
            });
        }
        break;
    case MsgType::TrashRequest: {
        const auto& items = std::get<pl::TrashRequest>(m.body).items;
        trashSizes_.push_back(items.size());
        if (!admit(1)) {
            return;
        }
        jobs_.push_back([this, id, items] {
            pl::TrashResult result;
            for (const auto& item : items) {
                auto outcome = pl::TrashOutcome::Trashed;
                if (!exists(item.id)) {
                    outcome = pl::TrashOutcome::Missing;
                } else if (script_.declineTrash.contains(item.id)) {
                    outcome = pl::TrashOutcome::UserDeclined;
                } else {
                    deleted_.insert(item.id);
                }
                result.items.emplace_back(item.id, outcome);
            }
            if (script_.trashDelayMs > 0) {
                QTimer::singleShot(script_.trashDelayMs, this, [this, id, result] { respond({MsgType::TrashResult, id, result}); });
            } else {
                respond({MsgType::TrashResult, id, result});
            }
        });
        break;
    }
    default:
        closeLink();  // a phone receives only HelloAck, requests, Error and Bye
        return;
    }
    if (!jobs_.empty() && !serve_.isActive()) {
        serve_.start();
    }
}

bool FakePhone::admit(const std::size_t credits) {
    outstanding_ += credits;
    maxOutstanding_ = std::max(maxOutstanding_, outstanding_);
    if (outstanding_ <= credit_window) {
        return true;
    }
    sendPlain(pl::encode({pl::MsgType::Error, 0,
                          pl::ErrorMsg{static_cast<std::uint16_t>(pl::ErrorCode::FlowControl), "over 16 credits"}}));
    closeLink();
    return false;
}

void FakePhone::respond(const pl::Message& m) {
    if (closed_) {
        return;
    }
    --outstanding_;  // each response, or an Error ending a one-credit request, returns one credit
    sendPlain(pl::encode(m));
}

void FakePhone::serveOne() {
    if (closed_ || jobs_.empty()) {
        serve_.stop();
        return;
    }
    batching_ = script_.burst;
    do {
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        job();
    } while (batching_ && !jobs_.empty() && !closed_);
    batching_ = false;
    if (!batch_.isEmpty() && !closed_ && stream_) {
        stream_->write(std::exchange(batch_, {}));
    }
}

pl::CatalogPage FakePhone::page(const pl::CatalogSince& since) const {
    static constexpr std::string_view store = "v1";
    pl::CatalogPage out;
    out.store_version = store;
    out.full_resync = since.store_version != store;
    const std::uint64_t after = out.full_resync ? 0 : since.generation;
    // type, request_id, store_version, next_generation, two flags, two list counts
    std::size_t size = 1 + 4 + 2 + store.size() + 8 + 1 + 1 + 2 + 2;
    std::uint64_t id = after + 1;
    for (; id <= script_.catalogSize && out.items.size() < since.max_items; ++id) {
        if (deleted_.contains(id)) {
            continue;
        }
        auto item = catalog_item(id);
        const auto bytes = 58 + item.relative_path.size() + item.mime.size();
        if (size + bytes > pl::max_plaintext) {
            break;  // the message size limit binds before max_items
        }
        size += bytes;
        out.items.push_back(std::move(item));
    }
    out.next_generation = id - 1;  // every generation up to here has been sent
    out.more = id <= script_.catalogSize;
    return out;
}

void FakePhone::sendPlain(const std::vector<std::byte>& plaintext) {
    auto ciphertext = session_.seal(plaintext);
    if (++transportSent_ == corruptAt_) {
        ciphertext[ciphertext.size() / 2] ^= std::byte{0x01};
    }
    sendFrame(ciphertext);
}

void FakePhone::sendFrame(const std::vector<std::byte>& payload) {
    if (closed_ || !stream_) {
        return;
    }
    const auto frame = pl::encode_frame(payload);
    const QByteArray bytes(reinterpret_cast<const char*>(frame.data()), static_cast<qsizetype>(frame.size()));
    if (batching_) {
        batch_ += bytes;
    } else {
        stream_->write(bytes);
    }
}

void FakePhone::closeLink() {
    closed_ = true;
    serve_.stop();
    if (stream_) {
        stream_->close();
    }
}

}  // namespace pms::desktop::testing
