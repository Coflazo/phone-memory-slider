#include "catalog_store.hpp"

#include <QByteArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTypes>
#include <qfloat16.h>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace pms::desktop {
namespace {

QByteArray encode_embedding(const FixedEmbedding& embedding) {
    std::array<qfloat16, embedding_dimensions> halves{};
    for (std::size_t index = 0; index < halves.size(); ++index) {
        halves[index] = qfloat16{embedding[index]};
    }
    static_assert(sizeof(qfloat16) == 2);
    return {reinterpret_cast<const char*>(halves.data()), static_cast<qsizetype>(sizeof(halves))};
}

FixedEmbedding decode_embedding(const QByteArray& bytes) {
    std::array<qfloat16, embedding_dimensions> halves{};
    if (bytes.size() != static_cast<qsizetype>(sizeof(halves))) {
        return {};
    }
    std::memcpy(halves.data(), bytes.constData(), sizeof(halves));
    std::array<float, embedding_dimensions> values{};
    for (std::size_t index = 0; index < halves.size(); ++index) {
        values[index] = static_cast<float>(halves[index]);
    }
    return FixedEmbedding{values};
}

bool bind_uint64(QSqlQuery& query, const QString& name, const std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    query.bindValue(name, QVariant::fromValue(static_cast<qint64>(value)));
    return true;
}

bool ensure_column(QSqlDatabase& database, const QString& name, const QString& declaration, QString& error) {
    QSqlQuery columns{database};
    if (!columns.exec(QStringLiteral("PRAGMA table_info(assets)"))) {
        error = columns.lastError().text();
        return false;
    }
    while (columns.next()) {
        if (columns.value(1).toString() == name) {
            return true;
        }
    }
    QSqlQuery alter{database};
    if (!alter.exec(QStringLiteral("ALTER TABLE assets ADD COLUMN ") + declaration)) {
        error = alter.lastError().text();
        return false;
    }
    return true;
}

}  // namespace

CatalogStore::CatalogStore(QString database_path)
    : database_path_(std::move(database_path)),
      connection_name_(QStringLiteral("pms-catalog-") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

CatalogStore::~CatalogStore() {
    if (database_.isValid()) {
        database_.close();
        database_ = {};
    }
    QSqlDatabase::removeDatabase(connection_name_);
}

bool CatalogStore::open() {
    if (database_.isOpen()) {
        return true;
    }
    if (database_path_.isEmpty()) {
        setError(QStringLiteral("No private application-data directory is available"));
        return false;
    }
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
    database_.setDatabaseName(database_path_);
    if (!database_.open()) {
        setError(database_.lastError().text());
        return false;
    }
    if (!executeSchema(QStringLiteral("PRAGMA foreign_keys = ON")) ||
        !executeSchema(QStringLiteral("PRAGMA journal_mode = WAL")) ||
        !executeSchema(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS devices("
               "device_id TEXT PRIMARY KEY, revision INTEGER NOT NULL, cursor TEXT NOT NULL DEFAULT '', "
               "complete INTEGER NOT NULL DEFAULT 0)")) ||
        !executeSchema(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS assets("
               "device_id TEXT NOT NULL, asset_id TEXT NOT NULL, mime_type TEXT NOT NULL, bytes INTEGER NOT NULL, "
               "modified_ms INTEGER NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL, duration_ms INTEGER NOT NULL, "
               "favorite INTEGER NOT NULL, embedding BLOB NOT NULL, perceptual_hash BLOB NOT NULL, "
               "blur_problem REAL NOT NULL, exposure_problem REAL NOT NULL, screenshot_likelihood REAL NOT NULL, "
               "cleanup_score REAL NOT NULL, keep_score REAL NOT NULL, "
               "storage_score REAL NOT NULL, reasons TEXT NOT NULL, label TEXT NOT NULL, decision TEXT NOT NULL, "
               "PRIMARY KEY(device_id, asset_id), FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE)"))) {
        return false;
    }
    return ensure_column(database_, QStringLiteral("perceptual_hash"),
                         QStringLiteral("perceptual_hash BLOB NOT NULL DEFAULT X'0000000000000000'"), error_) &&
           ensure_column(database_, QStringLiteral("blur_problem"),
                         QStringLiteral("blur_problem REAL NOT NULL DEFAULT 0"), error_) &&
           ensure_column(database_, QStringLiteral("exposure_problem"),
                         QStringLiteral("exposure_problem REAL NOT NULL DEFAULT 0"), error_) &&
           ensure_column(database_, QStringLiteral("screenshot_likelihood"),
                         QStringLiteral("screenshot_likelihood REAL NOT NULL DEFAULT 0"), error_) &&
           executeSchema(QStringLiteral("PRAGMA user_version = 2"));
}

QString CatalogStore::errorString() const {
    return error_;
}

bool CatalogStore::beginSync(const QString& device_id, const std::uint64_t revision) {
    if (!database_.isOpen() || device_id.isEmpty() ||
        revision > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) || !database_.transaction()) {
        setError(QStringLiteral("Cannot begin catalog synchronization"));
        return false;
    }

    QSqlQuery lookup{database_};
    lookup.prepare(QStringLiteral("SELECT revision FROM devices WHERE device_id = :device"));
    lookup.bindValue(QStringLiteral(":device"), device_id);
    if (!lookup.exec()) {
        setError(lookup.lastError().text());
        database_.rollback();
        return false;
    }
    const auto exists = lookup.next();
    const auto same_revision = exists && lookup.value(0).toLongLong() == static_cast<qint64>(revision);
    if (same_revision) {
        return database_.commit();
    }

    QSqlQuery replace{database_};
    replace.prepare(QStringLiteral(
        "INSERT INTO devices(device_id, revision, cursor, complete) VALUES(:device, :revision, '', 0) "
        "ON CONFLICT(device_id) DO UPDATE SET revision=excluded.revision, cursor='', complete=0"));
    replace.bindValue(QStringLiteral(":device"), device_id);
    replace.bindValue(QStringLiteral(":revision"), QVariant::fromValue(static_cast<qint64>(revision)));
    if (!replace.exec()) {
        setError(replace.lastError().text());
        database_.rollback();
        return false;
    }
    QSqlQuery clear{database_};
    clear.prepare(QStringLiteral("DELETE FROM assets WHERE device_id = :device"));
    clear.bindValue(QStringLiteral(":device"), device_id);
    if (!clear.exec() || !database_.commit()) {
        setError(clear.lastError().text());
        database_.rollback();
        return false;
    }
    return true;
}

bool CatalogStore::upsertPage(
    const QString& device_id,
    const std::uint64_t revision,
    const std::span<const CatalogRecord> records,
    const QString& next_cursor,
    const bool complete) {
    const auto state = syncState(device_id);
    if (!state.exists || state.revision != revision || next_cursor.size() > 512 || !database_.transaction()) {
        setError(QStringLiteral("Catalog page does not match the active revision"));
        return false;
    }

    QSqlQuery upsert{database_};
    upsert.prepare(QStringLiteral(
        "INSERT INTO assets(device_id, asset_id, mime_type, bytes, modified_ms, width, height, duration_ms, favorite, "
        "embedding, perceptual_hash, blur_problem, exposure_problem, screenshot_likelihood, cleanup_score, keep_score, "
        "storage_score, reasons, label, decision) "
        "VALUES(:device, :asset, :mime, :bytes, :modified, :width, :height, :duration, :favorite, :embedding, "
        ":phash, :blur, :exposure, :screenshot, :cleanup, :keep, :storage, :reasons, :label, :decision) "
        "ON CONFLICT(device_id, asset_id) DO UPDATE SET mime_type=excluded.mime_type, bytes=excluded.bytes, "
        "modified_ms=excluded.modified_ms, width=excluded.width, height=excluded.height, duration_ms=excluded.duration_ms, "
        "favorite=excluded.favorite, embedding=excluded.embedding, perceptual_hash=excluded.perceptual_hash, "
        "blur_problem=excluded.blur_problem, exposure_problem=excluded.exposure_problem, "
        "screenshot_likelihood=excluded.screenshot_likelihood, cleanup_score=excluded.cleanup_score, "
        "keep_score=excluded.keep_score, storage_score=excluded.storage_score, reasons=excluded.reasons, "
        "label=excluded.label, decision=excluded.decision"));
    for (const auto& record : records) {
        if (record.assetId.isEmpty() || record.assetId.size() > 512 || record.mimeType.size() > 128 ||
            !bind_uint64(upsert, QStringLiteral(":bytes"), record.bytes) ||
            !bind_uint64(upsert, QStringLiteral(":modified"), record.modifiedEpochMs) ||
            !bind_uint64(upsert, QStringLiteral(":duration"), record.durationMs)) {
            setError(QStringLiteral("Catalog record is outside supported bounds"));
            database_.rollback();
            return false;
        }
        upsert.bindValue(QStringLiteral(":device"), device_id);
        upsert.bindValue(QStringLiteral(":asset"), record.assetId);
        upsert.bindValue(QStringLiteral(":mime"), record.mimeType);
        upsert.bindValue(QStringLiteral(":width"), record.width);
        upsert.bindValue(QStringLiteral(":height"), record.height);
        upsert.bindValue(QStringLiteral(":favorite"), record.favorite ? 1 : 0);
        upsert.bindValue(QStringLiteral(":embedding"), encode_embedding(record.embedding));
        QByteArray perceptual_hash(sizeof(record.perceptualHash), Qt::Uninitialized);
        std::memcpy(perceptual_hash.data(), &record.perceptualHash, sizeof(record.perceptualHash));
        upsert.bindValue(QStringLiteral(":phash"), perceptual_hash);
        upsert.bindValue(QStringLiteral(":blur"), record.blurProblem);
        upsert.bindValue(QStringLiteral(":exposure"), record.exposureProblem);
        upsert.bindValue(QStringLiteral(":screenshot"), record.screenshotLikelihood);
        upsert.bindValue(QStringLiteral(":cleanup"), record.cleanupScore);
        upsert.bindValue(QStringLiteral(":keep"), record.keepScore);
        upsert.bindValue(QStringLiteral(":storage"), record.storageScore);
        upsert.bindValue(QStringLiteral(":reasons"), record.reasons.isNull() ? QStringLiteral("") : record.reasons);
        upsert.bindValue(QStringLiteral(":label"), record.label.isNull() ? QStringLiteral("") : record.label);
        upsert.bindValue(QStringLiteral(":decision"), record.decision.isNull() ? QStringLiteral("") : record.decision);
        if (!upsert.exec()) {
            setError(upsert.lastError().text());
            database_.rollback();
            return false;
        }
        upsert.finish();
    }

    QSqlQuery update{database_};
    update.prepare(QStringLiteral("UPDATE devices SET cursor=:cursor, complete=:complete WHERE device_id=:device"));
    update.bindValue(QStringLiteral(":cursor"), next_cursor);
    update.bindValue(QStringLiteral(":complete"), complete ? 1 : 0);
    update.bindValue(QStringLiteral(":device"), device_id);
    if (!update.exec() || update.numRowsAffected() != 1 || !database_.commit()) {
        setError(update.lastError().text());
        database_.rollback();
        return false;
    }
    return true;
}

CatalogSyncState CatalogStore::syncState(const QString& device_id) const {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral("SELECT revision, cursor, complete FROM devices WHERE device_id=:device"));
    query.bindValue(QStringLiteral(":device"), device_id);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return {
        static_cast<std::uint64_t>(query.value(0).toLongLong()),
        query.value(1).toString(),
        query.value(2).toBool(),
        true,
    };
}

std::vector<CatalogRecord> CatalogStore::assets(const QString& device_id) const {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "SELECT asset_id, mime_type, bytes, modified_ms, width, height, duration_ms, favorite, embedding, "
        "perceptual_hash, blur_problem, exposure_problem, screenshot_likelihood, cleanup_score, keep_score, "
        "storage_score, reasons, label, decision FROM assets "
        "WHERE device_id=:device ORDER BY asset_id"));
    query.bindValue(QStringLiteral(":device"), device_id);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    std::vector<CatalogRecord> records;
    while (query.next()) {
        std::uint64_t perceptual_hash{};
        const auto hash_bytes = query.value(9).toByteArray();
        if (hash_bytes.size() == static_cast<qsizetype>(sizeof(perceptual_hash))) {
            std::memcpy(&perceptual_hash, hash_bytes.constData(), sizeof(perceptual_hash));
        }
        records.push_back({
            query.value(0).toString(),
            query.value(1).toString(),
            static_cast<std::uint64_t>(query.value(2).toLongLong()),
            static_cast<std::uint64_t>(query.value(3).toLongLong()),
            query.value(4).toInt(),
            query.value(5).toInt(),
            static_cast<std::uint64_t>(query.value(6).toLongLong()),
            query.value(7).toBool(),
            decode_embedding(query.value(8).toByteArray()),
            perceptual_hash,
            query.value(10).toFloat(),
            query.value(11).toFloat(),
            query.value(12).toFloat(),
            query.value(13).toFloat(),
            query.value(14).toFloat(),
            query.value(15).toFloat(),
            query.value(16).toString(),
            query.value(17).toString(),
            query.value(18).toString(),
        });
    }
    return records;
}

bool CatalogStore::updateAnalysis(
    const QString& device_id,
    const QString& asset_id,
    const FixedEmbedding& embedding,
    const std::uint64_t perceptual_hash,
    const float blur_problem,
    const float exposure_problem,
    const float screenshot_likelihood) {
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "UPDATE assets SET embedding=:embedding, perceptual_hash=:phash, blur_problem=:blur, "
        "exposure_problem=:exposure, screenshot_likelihood=:screenshot "
        "WHERE device_id=:device AND asset_id=:asset"));
    query.bindValue(QStringLiteral(":embedding"), encode_embedding(embedding));
    QByteArray hash_bytes(sizeof(perceptual_hash), Qt::Uninitialized);
    std::memcpy(hash_bytes.data(), &perceptual_hash, sizeof(perceptual_hash));
    query.bindValue(QStringLiteral(":phash"), hash_bytes);
    query.bindValue(QStringLiteral(":blur"), blur_problem);
    query.bindValue(QStringLiteral(":exposure"), exposure_problem);
    query.bindValue(QStringLiteral(":screenshot"), screenshot_likelihood);
    query.bindValue(QStringLiteral(":device"), device_id);
    query.bindValue(QStringLiteral(":asset"), asset_id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

bool CatalogStore::updateLabel(
    const QString& device_id,
    const QString& asset_id,
    const QString& label) {
    if (device_id.isEmpty() || asset_id.isEmpty() ||
        (label != QStringLiteral("keep") && label != QStringLiteral("delete") && !label.isEmpty())) {
        setError(QStringLiteral("Invalid local preference label"));
        return false;
    }
    QSqlQuery query{database_};
    query.prepare(QStringLiteral(
        "UPDATE assets SET label=:label WHERE device_id=:device AND asset_id=:asset"));
    query.bindValue(QStringLiteral(":label"), label);
    query.bindValue(QStringLiteral(":device"), device_id);
    query.bindValue(QStringLiteral(":asset"), asset_id);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

bool CatalogStore::executeSchema(const QString& sql) {
    QSqlQuery query{database_};
    if (query.exec(sql)) {
        return true;
    }
    setError(query.lastError().text());
    return false;
}

void CatalogStore::setError(const QString& message) const {
    error_ = message;
}

}  // namespace pms::desktop
