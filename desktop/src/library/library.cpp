#include "library/library.hpp"

#include <QDir>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pms::desktop {
namespace {

Q_LOGGING_CATEGORY(library_log, "pms.library")

static_assert(std::endian::native == std::endian::little, "embeddings.f16 rows are copied as little-endian halves");

constexpr std::array<char, 8> embeddings_magic{'P', 'M', 'S', 'E', 'M', 'B', '1', '\0'};
constexpr qint64 header_bytes = 12;  // magic + u32 dimension
constexpr int sync_every = 256;
constexpr std::size_t max_dim = 65536;
constexpr int schema_version = 2;

constexpr const char* schema[] = {
    "PRAGMA foreign_keys = ON",
    "PRAGMA journal_mode = WAL",
    "CREATE TABLE IF NOT EXISTS sources("
    "key TEXT PRIMARY KEY, cursor_version TEXT NOT NULL DEFAULT '', cursor_generation INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS items("
    "id INTEGER PRIMARY KEY, source_key TEXT NOT NULL REFERENCES sources(key), source_id TEXT NOT NULL, "
    "relative_path TEXT NOT NULL, mime TEXT NOT NULL, format INTEGER NOT NULL, kind INTEGER NOT NULL, "
    "bytes INTEGER NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL, duration_s REAL NOT NULL, "
    "captured_ms INTEGER NOT NULL, modified_s INTEGER NOT NULL, sha256 BLOB NOT NULL, "
    "favorite INTEGER NOT NULL, seed INTEGER NOT NULL, unit_id INTEGER NOT NULL, "
    "embedding_row INTEGER NOT NULL DEFAULT -1, sharpness REAL NOT NULL DEFAULT 0, clipped REAL NOT NULL DEFAULT 0, "
    "noise REAL NOT NULL DEFAULT 0, pdq BLOB NOT NULL DEFAULT x'', pdq_quality INTEGER NOT NULL DEFAULT 0, "
    "error TEXT NOT NULL DEFAULT '', removed INTEGER NOT NULL DEFAULT 0, UNIQUE(source_key, source_id))",
    // Append-only: undo flags the row instead of deleting it.
    "CREATE TABLE IF NOT EXISTS decisions("
    "seq INTEGER PRIMARY KEY, item_id INTEGER NOT NULL REFERENCES items(id), decision INTEGER NOT NULL, "
    "at_ms INTEGER NOT NULL, undone INTEGER NOT NULL DEFAULT 0)",
    "PRAGMA user_version = 2",
};

// The file's content changed since the row was analysed: same rule for every analysis column.
constexpr auto content_changed =
    "(bytes <> excluded.bytes OR modified_s <> excluded.modified_s OR "
    "(length(sha256) > 0 AND length(excluded.sha256) > 0 AND sha256 <> excluded.sha256))";

bool bind_uint64(QSqlQuery& query, const QString& name, const std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    query.bindValue(name, QVariant::fromValue(static_cast<qint64>(value)));
    return true;
}

// A null QString / QByteArray binds as SQL NULL; the columns are NOT NULL, so bind empty instead.
QString text(const QString& value) {
    return value.isNull() ? QStringLiteral("") : value;
}
QByteArray blob(const QByteArray& value) {
    return value.isNull() ? QByteArray("") : value;
}

bool run(QSqlQuery& query, const char* what) {
    if (query.exec()) {
        return true;
    }
    qCWarning(library_log) << what << query.lastError().text();
    return false;
}

QByteArray encode_pdq(const std::array<std::uint64_t, 4>& bits) {
    QByteArray bytes(32, Qt::Uninitialized);
    for (std::size_t index = 0; index < bits.size(); ++index) {
        qToLittleEndian(bits[index], bytes.data() + index * 8);
    }
    return bytes;
}

template <typename Enum>
Enum checked_enum(const int value, const Enum last, const Enum fallback) {
    return value >= 0 && value <= static_cast<int>(last) ? static_cast<Enum>(value) : fallback;
}

std::optional<Decision> to_decision(const int value) {
    if (value < 0 || value > static_cast<int>(Decision::Delete)) {
        return std::nullopt;
    }
    return static_cast<Decision>(value);
}

void sync_to_disk(QFile& file) {
#ifdef Q_OS_WIN
    const auto ok = FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != FALSE;
#else
    const auto ok = ::fsync(file.handle()) == 0;
#endif
    if (!ok) {
        qCWarning(library_log) << "embeddings.f16 could not be flushed to disk";
    }
}

}  // namespace

Library::Library(QString directory)
    : directory_(std::move(directory)),
      connection_name_(QStringLiteral("pms-library-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Library::~Library() {
    syncEmbeddings();
    if (database_.isValid()) {
        database_.close();
        database_ = {};
    }
    QSqlDatabase::removeDatabase(connection_name_);
}

bool Library::open(QString* error) {
    if (database_.isOpen()) {
        return true;
    }
    const auto fail = [this, error](const QString& message) {
        qCWarning(library_log) << message;
        if (error != nullptr) {
            *error = message;
        }
        embeddings_.close();
        database_.close();
        return false;
    };
    if (directory_.isEmpty() || !QDir{}.mkpath(directory_)) {
        return fail(QStringLiteral("The library folder could not be created"));
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return fail(QStringLiteral("The library folder could not be restricted to the current user"));
    }
#endif
    const QDir directory{directory_};
    // v1 never shipped a release, so its catalog is not migrated; it is kept aside, not deleted.
    const auto v1 = directory.filePath(QStringLiteral("catalog.sqlite"));
    const auto v1_backup = directory.filePath(QStringLiteral("catalog-v1.db.bak"));
    if (QFile::exists(v1) && !QFile::exists(v1_backup)) {
        for (const auto& suffix : {QString{}, QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            if (QFile::exists(v1 + suffix) && !QFile::rename(v1 + suffix, v1_backup + suffix)) {
                qCWarning(library_log) << "could not move the v1 catalog aside:" << v1 + suffix;
            }
        }
    }

    if (!database_.isValid()) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
        database_.setDatabaseName(directory.filePath(QStringLiteral("library.db")));
    }
    if (!database_.open()) {
        return fail(database_.lastError().text());
    }
    QSqlQuery version{database_};
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next()) {
        return fail(version.lastError().text());
    }
    const auto found = version.value(0).toInt();
    version.finish();
    if (found != 0 && found != schema_version) {
        return fail(QStringLiteral("library.db has schema version %1; this version of the app reads version %2")
                        .arg(found)
                        .arg(schema_version));
    }
    for (const auto* statement : schema) {
        QSqlQuery query{database_};
        if (!query.exec(QString::fromLatin1(statement))) {
            return fail(query.lastError().text());
        }
    }
    QString message;
    if (!openEmbeddings(message)) {
        return fail(message);
    }
    return true;
}

bool Library::openEmbeddings(QString& error) {
    embeddings_.setFileName(QDir{directory_}.filePath(QStringLiteral("embeddings.f16")));
    if (!embeddings_.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
        error = QStringLiteral("embeddings.f16 could not be opened: %1").arg(embeddings_.errorString());
        return false;
    }
    const auto size = embeddings_.size();
    dim_ = 0;
    rows_ = 0;
    if (size < header_bytes) {
        // A crash before the header was complete: nothing was stored yet.
        if (size != 0 && !embeddings_.resize(0)) {
            error = QStringLiteral("embeddings.f16 could not be reset");
            return false;
        }
    } else {
        std::array<char, header_bytes> header{};
        if (!embeddings_.seek(0) || embeddings_.read(header.data(), header_bytes) != header_bytes ||
            !std::equal(embeddings_magic.begin(), embeddings_magic.end(), header.begin())) {
            error = QStringLiteral("embeddings.f16 is not an embedding file");
            return false;
        }
        const auto dim = qFromLittleEndian<quint32>(header.data() + embeddings_magic.size());
        if (dim == 0 || dim > max_dim) {
            error = QStringLiteral("embeddings.f16 has an invalid dimension (%1)").arg(dim);
            return false;
        }
        dim_ = dim;
        const auto row_bytes = static_cast<qint64>(dim_ * sizeof(std::uint16_t));
        rows_ = (size - header_bytes) / row_bytes;
        const auto whole = header_bytes + rows_ * row_bytes;
        if (size != whole && !embeddings_.resize(whole)) {  // drop the row a crash cut off
            error = QStringLiteral("embeddings.f16 could not be trimmed to whole rows");
            return false;
        }
    }
    // Rows past the end were lost in a crash; those items are embedded again.
    QSqlQuery reset{database_};
    reset.prepare(QStringLiteral("UPDATE items SET embedding_row = -1 WHERE embedding_row >= :rows"));
    reset.bindValue(QStringLiteral(":rows"), rows_);
    if (!reset.exec()) {
        error = reset.lastError().text();
        return false;
    }
    return true;
}

void Library::syncEmbeddings() {
    if (embeddings_.isOpen() && unsynced_ > 0) {
        sync_to_disk(embeddings_);
        unsynced_ = 0;
    }
}

QList<qint64> Library::upsertItems(const QString& source_key, const QList<ItemRow>& rows, const bool full_sync) {
    if (source_key.isEmpty() || source_key.size() > 256 || !database_.isOpen() || !database_.transaction()) {
        qCWarning(library_log) << "upsert refused for source" << source_key;
        return {};
    }
    const auto abort = [this](const char* why) {
        qCWarning(library_log) << "upsert rolled back:" << why;
        database_.rollback();
        return QList<qint64>{};
    };

    QSqlQuery source{database_};
    source.prepare(QStringLiteral("INSERT INTO sources(key) VALUES(:key) ON CONFLICT(key) DO NOTHING"));
    source.bindValue(QStringLiteral(":key"), source_key);
    if (!run(source, "source")) {
        return abort("source row");
    }
    if (full_sync) {
        QSqlQuery hide{database_};
        hide.prepare(QStringLiteral("UPDATE items SET removed = 1 WHERE source_key = :key"));
        hide.bindValue(QStringLiteral(":key"), source_key);
        if (!run(hide, "full sync")) {
            return abort("full sync");
        }
    }

    const auto changed = QString::fromLatin1(content_changed);
    QSqlQuery upsert{database_};
    upsert.prepare(
        QStringLiteral(
            "INSERT INTO items(source_key, source_id, relative_path, mime, format, kind, bytes, width, height, "
            "duration_s, captured_ms, modified_s, sha256, favorite, seed, unit_id) "
            "VALUES(:source, :sid, :path, :mime, :format, :kind, :bytes, :width, :height, :duration, :captured, "
            ":modified, :sha, :favorite, :seed, :unit) "
            "ON CONFLICT(source_key, source_id) DO UPDATE SET "
            "relative_path = excluded.relative_path, mime = excluded.mime, format = excluded.format, "
            "kind = excluded.kind, width = excluded.width, height = excluded.height, duration_s = excluded.duration_s, "
            "captured_ms = excluded.captured_ms, favorite = excluded.favorite, seed = excluded.seed, "
            "unit_id = excluded.unit_id, removed = 0, bytes = excluded.bytes, modified_s = excluded.modified_s, "
            "sha256 = CASE WHEN %1 OR length(excluded.sha256) > 0 THEN excluded.sha256 ELSE sha256 END, "
            "embedding_row = CASE WHEN %1 THEN -1 ELSE embedding_row END, "
            "sharpness = CASE WHEN %1 THEN 0 ELSE sharpness END, clipped = CASE WHEN %1 THEN 0 ELSE clipped END, "
            "noise = CASE WHEN %1 THEN 0 ELSE noise END, pdq = CASE WHEN %1 THEN x'' ELSE pdq END, "
            "pdq_quality = CASE WHEN %1 THEN 0 ELSE pdq_quality END, error = CASE WHEN %1 THEN '' ELSE error END "
            "RETURNING id")
            .arg(changed));

    QList<qint64> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.source_id.isEmpty() || row.source_id.size() > 1024 || row.relative_path.size() > 4096 ||
            row.mime.size() > 255 || !bind_uint64(upsert, QStringLiteral(":bytes"), row.bytes)) {
            return abort("row outside supported bounds");
        }
        upsert.bindValue(QStringLiteral(":source"), source_key);
        upsert.bindValue(QStringLiteral(":sid"), row.source_id);
        upsert.bindValue(QStringLiteral(":path"), text(row.relative_path));
        upsert.bindValue(QStringLiteral(":mime"), text(row.mime));
        upsert.bindValue(QStringLiteral(":format"), static_cast<int>(row.format));
        upsert.bindValue(QStringLiteral(":kind"), static_cast<int>(row.kind));
        upsert.bindValue(QStringLiteral(":width"), static_cast<qint64>(row.width));
        upsert.bindValue(QStringLiteral(":height"), static_cast<qint64>(row.height));
        upsert.bindValue(QStringLiteral(":duration"), row.duration_s);
        upsert.bindValue(QStringLiteral(":captured"), row.captured_ms);
        upsert.bindValue(QStringLiteral(":modified"), row.modified_s);
        upsert.bindValue(QStringLiteral(":sha"), blob(row.sha256));
        upsert.bindValue(QStringLiteral(":favorite"), row.favorite ? 1 : 0);
        upsert.bindValue(QStringLiteral(":seed"), row.seed ? 1 : 0);
        upsert.bindValue(QStringLiteral(":unit"), row.unit_id);
        if (!run(upsert, "upsert") || !upsert.next()) {
            return abort("item row");
        }
        ids.push_back(upsert.value(0).toLongLong());
        upsert.finish();
    }
    if (!database_.commit()) {
        return abort("commit");
    }
    return ids;
}

void Library::setAnalysis(
    const qint64 id, const float sharpness, const float clipped, const float noise, const pms::PdqHash& pdq) {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "UPDATE items SET sharpness = :sharpness, clipped = :clipped, noise = :noise, pdq = :pdq, "
        "pdq_quality = :quality WHERE id = :id"));
    query.bindValue(QStringLiteral(":sharpness"), sharpness);
    query.bindValue(QStringLiteral(":clipped"), clipped);
    query.bindValue(QStringLiteral(":noise"), noise);
    query.bindValue(QStringLiteral(":pdq"), encode_pdq(pdq.bits));
    query.bindValue(QStringLiteral(":quality"), pdq.quality);
    query.bindValue(QStringLiteral(":id"), id);
    run(query, "analysis");
}

int Library::appendEmbedding(const qint64 id, const std::span<const float> unit_vector) {
    if (!embeddings_.isOpen() || unit_vector.empty() || unit_vector.size() > max_dim ||
        (dim_ != 0 && unit_vector.size() != dim_)) {
        return -1;
    }
    std::vector<std::uint16_t> halves;
    try {
        pms::EmbeddingMatrix one{unit_vector.size()};
        one.append(unit_vector);
        halves.assign(one.raw().begin(), one.raw().end());
    } catch (const std::invalid_argument&) {
        return -1;
    }
    QSqlQuery exists{database_};
    exists.prepare(QStringLiteral("SELECT 1 FROM items WHERE id = :id"));
    exists.bindValue(QStringLiteral(":id"), id);
    if (!run(exists, "embedding item") || !exists.next()) {
        return -1;
    }
    exists.finish();

    if (dim_ == 0) {
        std::array<char, header_bytes> header{};
        std::copy(embeddings_magic.begin(), embeddings_magic.end(), header.begin());
        qToLittleEndian(static_cast<quint32>(unit_vector.size()), header.data() + embeddings_magic.size());
        if (!embeddings_.seek(0) || embeddings_.write(header.data(), header_bytes) != header_bytes) {
            embeddings_.resize(0);
            qCWarning(library_log) << "embeddings.f16 header could not be written";
            return -1;
        }
        dim_ = unit_vector.size();
        ++unsynced_;
    }
    const auto row_bytes = static_cast<qint64>(dim_ * sizeof(std::uint16_t));
    const auto offset = header_bytes + rows_ * row_bytes;
    if (!embeddings_.seek(offset) ||
        embeddings_.write(reinterpret_cast<const char*>(halves.data()), row_bytes) != row_bytes) {
        embeddings_.resize(offset);
        qCWarning(library_log) << "embedding row could not be written:" << embeddings_.errorString();
        return -1;
    }
    const auto row = rows_++;
    if (++unsynced_ >= sync_every) {
        syncEmbeddings();
    }
    // A crash between the write and this update leaves an unreferenced row; the item is embedded again.
    QSqlQuery update{database_};
    update.prepare(QStringLiteral("UPDATE items SET embedding_row = :row WHERE id = :id"));
    update.bindValue(QStringLiteral(":row"), row);
    update.bindValue(QStringLiteral(":id"), id);
    if (!run(update, "embedding row")) {
        return -1;
    }
    return static_cast<int>(row);
}

pms::EmbeddingMatrix Library::loadEmbeddings() const {
    if (dim_ == 0) {
        return pms::EmbeddingMatrix{};
    }
    std::vector<std::uint16_t> halves(static_cast<std::size_t>(rows_) * dim_);
    const auto bytes = static_cast<qint64>(halves.size() * sizeof(std::uint16_t));
    QFile file{embeddings_.fileName()};
    if (!file.open(QIODevice::ReadOnly) || !file.seek(header_bytes) ||
        file.read(reinterpret_cast<char*>(halves.data()), bytes) != bytes) {
        qCWarning(library_log) << "embeddings.f16 could not be read:" << file.errorString();
        return pms::EmbeddingMatrix{dim_};
    }
    try {
        return pms::EmbeddingMatrix::from_raw(dim_, std::move(halves));
    } catch (const std::invalid_argument& failure) {
        qCWarning(library_log) << "embeddings.f16 is damaged:" << failure.what();
        return pms::EmbeddingMatrix{dim_};
    }
}

void Library::setError(const qint64 id, const QString& reason) {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral("UPDATE items SET error = :error WHERE id = :id"));
    query.bindValue(QStringLiteral(":error"), text(reason.left(1024)));
    query.bindValue(QStringLiteral(":id"), id);
    run(query, "error");
}

void Library::setDecision(const qint64 id, const Decision d, const qint64 at_ms) {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral("INSERT INTO decisions(item_id, decision, at_ms) VALUES(:id, :decision, :at)"));
    query.bindValue(QStringLiteral(":id"), id);
    query.bindValue(QStringLiteral(":decision"), static_cast<int>(d));
    query.bindValue(QStringLiteral(":at"), at_ms);
    run(query, "decision");
}

std::optional<std::pair<qint64, Decision>> Library::undoLast() {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "UPDATE decisions SET undone = 1 WHERE seq = (SELECT MAX(seq) FROM decisions WHERE undone = 0) "
        "RETURNING item_id, decision"));
    if (!run(query, "undo") || !query.next()) {
        return std::nullopt;
    }
    const auto decision = to_decision(query.value(1).toInt());
    if (!decision) {
        return std::nullopt;
    }
    return std::pair{query.value(0).toLongLong(), *decision};
}

QList<ItemRow> Library::items(const QString& source_key) const {
    QSqlQuery query{database_};
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT id, source_id, relative_path, mime, format, kind, bytes, width, height, duration_s, captured_ms, "
        "modified_s, sha256, favorite, seed, unit_id, embedding_row, sharpness, clipped, noise, pdq, pdq_quality, "
        "error FROM items WHERE source_key = :key AND removed = 0 ORDER BY id"));
    query.bindValue(QStringLiteral(":key"), source_key);
    QList<ItemRow> items;
    if (!run(query, "items")) {
        return items;
    }
    while (query.next()) {
        ItemRow item;
        item.id = query.value(0).toLongLong();
        item.source_id = query.value(1).toString();
        item.relative_path = query.value(2).toString();
        item.mime = query.value(3).toString();
        item.format = checked_enum(query.value(4).toInt(), pms::MediaFormat::Avi, pms::MediaFormat::Unknown);
        item.kind = checked_enum(query.value(5).toInt(), pms::MediaKind::Unknown, pms::MediaKind::Unknown);
        item.bytes = static_cast<quint64>(query.value(6).toLongLong());
        item.width = static_cast<quint32>(query.value(7).toLongLong());
        item.height = static_cast<quint32>(query.value(8).toLongLong());
        item.duration_s = query.value(9).toDouble();
        item.captured_ms = query.value(10).toLongLong();
        item.modified_s = query.value(11).toLongLong();
        item.sha256 = query.value(12).toByteArray();
        item.favorite = query.value(13).toBool();
        item.seed = query.value(14).toBool();
        item.unit_id = query.value(15).toLongLong();
        item.embedding_row = query.value(16).toInt();
        item.sharpness = query.value(17).toFloat();
        item.clipped = query.value(18).toFloat();
        item.noise = query.value(19).toFloat();
        const auto pdq = query.value(20).toByteArray();
        if (pdq.size() == 32) {
            for (std::size_t index = 0; index < item.pdq.size(); ++index) {
                item.pdq[index] = qFromLittleEndian<quint64>(pdq.constData() + index * 8);
            }
        }
        item.pdq_quality = query.value(21).toInt();
        item.error = query.value(22).toString();
        items.push_back(std::move(item));
    }
    return items;
}

QHash<qint64, Decision> Library::decisions() const {
    QSqlQuery query{database_};
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT item_id, decision FROM decisions WHERE seq IN "
        "(SELECT MAX(seq) FROM decisions WHERE undone = 0 GROUP BY item_id)"));
    QHash<qint64, Decision> decisions;
    if (!run(query, "decisions")) {
        return decisions;
    }
    while (query.next()) {
        if (const auto decision = to_decision(query.value(1).toInt())) {
            decisions.insert(query.value(0).toLongLong(), *decision);
        }
    }
    return decisions;
}

void Library::setSourceCursor(const QString& source_key, const QString& store_version, const quint64 generation) {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "INSERT INTO sources(key, cursor_version, cursor_generation) VALUES(:key, :version, :generation) "
        "ON CONFLICT(key) DO UPDATE SET cursor_version = excluded.cursor_version, "
        "cursor_generation = excluded.cursor_generation"));
    query.bindValue(QStringLiteral(":key"), source_key);
    query.bindValue(QStringLiteral(":version"), text(store_version));
    if (source_key.isEmpty() || !bind_uint64(query, QStringLiteral(":generation"), generation)) {
        qCWarning(library_log) << "source cursor refused for" << source_key;
        return;
    }
    run(query, "source cursor");
}

std::pair<QString, quint64> Library::sourceCursor(const QString& source_key) const {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral("SELECT cursor_version, cursor_generation FROM sources WHERE key = :key"));
    query.bindValue(QStringLiteral(":key"), source_key);
    if (!run(query, "source cursor") || !query.next()) {
        return {QString{}, 0};
    }
    return {query.value(0).toString(), static_cast<quint64>(query.value(1).toLongLong())};
}

}  // namespace pms::desktop
