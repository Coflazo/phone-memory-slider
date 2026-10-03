#pragma once

#include <QSqlDatabase>
#include <QString>

#include <cstdint>
#include <span>
#include <vector>

#include "pms/media_analysis.hpp"

namespace pms::desktop {

struct CatalogRecord {
    QString assetId;
    QString mimeType;
    std::uint64_t bytes{};
    std::uint64_t modifiedEpochMs{};
    int width{};
    int height{};
    std::uint64_t durationMs{};
    bool favorite{};
    FixedEmbedding embedding;
    std::uint64_t perceptualHash{};
    float blurProblem{};
    float exposureProblem{};
    float screenshotLikelihood{};
    float cleanupScore{};
    float keepScore{0.5F};
    float storageScore{};
    QString reasons;
    QString label;
    QString decision;
};

struct CatalogSyncState {
    std::uint64_t revision{};
    QString cursor;
    bool complete{};
    bool exists{};
};

class CatalogStore final {
public:
    explicit CatalogStore(QString database_path);
    ~CatalogStore();

    CatalogStore(const CatalogStore&) = delete;
    CatalogStore& operator=(const CatalogStore&) = delete;

    [[nodiscard]] bool open();
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] bool beginSync(const QString& device_id, std::uint64_t revision);
    [[nodiscard]] bool upsertPage(
        const QString& device_id,
        std::uint64_t revision,
        std::span<const CatalogRecord> records,
        const QString& next_cursor,
        bool complete);
    [[nodiscard]] CatalogSyncState syncState(const QString& device_id) const;
    [[nodiscard]] std::vector<CatalogRecord> assets(const QString& device_id) const;
    [[nodiscard]] bool updateAnalysis(
        const QString& device_id,
        const QString& asset_id,
        const FixedEmbedding& embedding,
        std::uint64_t perceptual_hash,
        float blur_problem,
        float exposure_problem,
        float screenshot_likelihood);

private:
    [[nodiscard]] bool executeSchema(const QString& sql);
    void setError(const QString& message) const;

    QString database_path_;
    QString connection_name_;
    mutable QString error_;
    QSqlDatabase database_;
};

}  // namespace pms::desktop
