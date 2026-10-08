#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace pms::desktop {

// One connection's bytes, in order: an RFCOMM socket in the app, a PipeStream in tests. Transports emit
// bytesReceived with at most 64 KiB at a time (PROTOCOL.md section 10 bounds the frame decoder by one read); the
// client also slices anything larger.
class ByteStream : public QObject {
    Q_OBJECT

public:
    explicit ByteStream(QObject* parent = nullptr) : QObject(parent) {}

    virtual void write(QByteArray bytes) = 0;
    virtual void close() = 0;  // idempotent; emits closed() once

signals:
    void bytesReceived(QByteArray bytes);
    void closed(QString reason);
};

}  // namespace pms::desktop
