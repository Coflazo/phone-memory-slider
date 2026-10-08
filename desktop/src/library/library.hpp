#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QList>
#include <QLockFile>
#include <QSqlDatabase>
#include <QString>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "pms/embeddings.hpp"
#include "pms/media_format.hpp"
#include "pms/media_kind.hpp"
#include "pms/pdq.hpp"

namespace pms::desktop {

struct ItemRow {
    qint64 id{};                 // local row id
    QString source_id;           // stable id from the source (device object id, MediaStore _ID, relative path)
    QString relative_path; QString mime; pms::MediaFormat format{}; pms::MediaKind kind{};
    quint64 bytes{}; quint32 width{}, height{}; double duration_s{}; qint64 captured_ms{}, modified_s{};
    QByteArray sha256;           // empty until known
    bool favorite{}; bool seed{};
    qint64 unit_id{};            // deletion unit (Live Photo, AAE): 0 = alone
    int embedding_row{-1};       // row in embeddings.f16, -1 = not yet embedded
    float sharpness{}, clipped{}, noise{}; std::array<quint64, 4> pdq{}; int pdq_quality{};
    QString error;               // non-empty = unreadable, with reason
};

enum class Decision : quint8 { None, Keep, Delete };

// The local catalog: <dir>/library.db (SQLite, schema version 2) and <dir>/embeddings.f16 (an 8-byte magic
// "PMSEMB1\0", a little-endian u32 dimension, then fp16 rows). Work survives a restart: rows, analysis,
// embeddings and decisions are on disk as soon as the call returns (embedding rows are fsync'd every 256
// appends and on close; open() drops a cut-off last row and re-queues items whose row was lost, or whose row in
// the last 256 reads as zeros or non-finite). One process at a time: open() takes <dir>/library.lock and fails
// with "open in another window" while another instance holds it. One thread at a time: the SQLite connection
// belongs to the thread that calls open().
class Library final {
public:
    explicit Library(QString directory);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] bool open(QString* error);

    // Upsert by (source_key, source_id); returns ids in row order, or nothing if any row is invalid (the batch
    // is all or nothing). Only source facts are written: analysis, embedding and error stay unless bytes or
    // modified_s changed, which clears them so the file is analysed again. A full sync marks this source's
    // items that are not in `rows` removed; items() hides them and they keep their id and decisions.
    QList<qint64> upsertItems(const QString& source_key, const QList<ItemRow>& rows, bool full_sync);
    void setAnalysis(qint64 id, float sharpness, float clipped, float noise, const pms::PdqHash& pdq);
    // Normalizes like EmbeddingMatrix::append and stores fp16; returns the row, or -1 for an unknown item, a
    // zero or non-finite vector, or a dimension other than the file's.
    int appendEmbedding(qint64 id, std::span<const float> unit_vector);
    // A row holding inf or NaN comes back as zeros and its item is re-queued (embedding_row -1); the other rows
    // are untouched. That re-queue is the only write this const call makes.
    [[nodiscard]] pms::EmbeddingMatrix loadEmbeddings() const;
    void setError(qint64 id, const QString& reason);
    // Keeps history for undo, and records what was reviewed: the item's (sha256, bytes, modified_s) right now.
    void setDecision(qint64 id, Decision d, qint64 at_ms);
    // Takes back the latest decision made since open() on an item that is not removed, and returns it; nothing
    // once there is none (decisions from an earlier session are never undone).
    [[nodiscard]] std::optional<std::pair<qint64, Decision>> undoLast();
    [[nodiscard]] QList<ItemRow> items(const QString& source_key) const;  // not removed, by id
    // Latest decision per item, only where the item still matches what was reviewed (same bytes and modified_s,
    // and the same sha256 when both are known).
    [[nodiscard]] QHash<qint64, Decision> decisions() const;
    // Items whose latest decision was made on content that has changed since ("Changed since you reviewed it").
    [[nodiscard]] QList<qint64> staleDecisions() const;
    void setSourceCursor(const QString& source_key, const QString& store_version, quint64 generation);
    [[nodiscard]] std::pair<QString, quint64> sourceCursor(const QString& source_key) const;

private:
    [[nodiscard]] bool openEmbeddings(QString& error);
    void syncEmbeddings();
    void requeueRows(const QList<qint64>& rows) const;
    [[nodiscard]] QHash<qint64, std::pair<Decision, bool>> latestDecisions() const;  // item -> (decision, fresh)

    QString directory_;
    std::unique_ptr<QLockFile> lock_;
    QString connection_name_;
    QSqlDatabase database_;
    QFile embeddings_;
    std::size_t dim_{};   // 0 until the first row fixes it
    qint64 rows_{};
    int unsynced_{};
    qint64 session_seq_{};  // decisions with a larger seq were made since open()
};

}  // namespace pms::desktop
