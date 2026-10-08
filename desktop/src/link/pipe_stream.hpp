#pragma once

#include <QPointer>

#include <utility>

#include "link/byte_stream.hpp"

namespace pms::desktop {

// Two connected in-memory ends. What one end writes the other receives on a later event loop turn, in order and in
// reads of at most 64 KiB; closing one end closes the other after the bytes already written have arrived.
class PipeStream final : public ByteStream {
    Q_OBJECT

public:
    [[nodiscard]] static std::pair<PipeStream*, PipeStream*> create(QObject* parent);

    void write(QByteArray bytes) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

private:
    explicit PipeStream(QObject* parent) : ByteStream(parent) {}

    QPointer<PipeStream> peer_;
    bool open_{true};
};

}  // namespace pms::desktop
