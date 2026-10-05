#include "review_deck_model.hpp"

#include <QUrl>

#include <utility>
#include <vector>

namespace pms::desktop {

ReviewDeckModel::ReviewDeckModel(QObject* parent)
    : QAbstractListModel(parent), session_(std::vector<ReviewItem>{}) {}

int ReviewDeckModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !session_.current().has_value() ? 0 : 1;
}

QVariant ReviewDeckModel::data(const QModelIndex& index, const int role) const {
    const auto current = session_.current();
    if (!current.has_value() || !index.isValid() || index.row() != 0) {
        return {};
    }

    switch (role) {
    case AssetIdRole:
        return QString::fromStdString(current->asset.asset_id);
    case DisplayNameRole:
        return QString::fromStdString(current->display_name);
    case ReasonRole:
        return current->reasons.empty() ? QStringLiteral("Worth a look") : reasonLabel(current->reasons.front());
    case StorageTextRole:
        return formatBytes(current->bytes);
    case MediaKindRole:
        return current->media_kind == MediaKind::Video ? QStringLiteral("video") : QStringLiteral("photo");
    case PreviewUrlRole:
        if (preview_asset_id_ == QString::fromStdString(current->asset.asset_id) && preview_override_.isValid()) {
            return preview_override_;
        }
        return QUrl{QString::fromStdString(current->preview_uri)};
    default:
        return {};
    }
}

QHash<int, QByteArray> ReviewDeckModel::roleNames() const {
    return {
        {AssetIdRole, "assetId"},
        {DisplayNameRole, "displayName"},
        {ReasonRole, "reason"},
        {StorageTextRole, "storageText"},
        {MediaKindRole, "mediaKind"},
        {PreviewUrlRole, "previewUrl"},
    };
}

int ReviewDeckModel::remaining() const noexcept {
    return static_cast<int>(session_.remaining());
}

QString ReviewDeckModel::pendingBytesText() const {
    return formatBytes(session_.pending_delete_bytes());
}

int ReviewDeckModel::pendingCount() const noexcept {
    return static_cast<int>(session_.pending_delete_count());
}

bool ReviewDeckModel::complete() const noexcept {
    return session_.remaining() == 0;
}

QString ReviewDeckModel::currentAssetId() const {
    const auto current = session_.current();
    return current.has_value() ? QString::fromStdString(current->asset.asset_id) : QString{};
}

TrashBatch ReviewDeckModel::pendingTrashBatch() const {
    return session_.prepare_trash_batch();
}

void ReviewDeckModel::loadItems(std::vector<ReviewItem> items) {
    beginResetModel();
    session_ = ReviewSession{std::move(items)};
    preview_asset_id_.clear();
    preview_override_.clear();
    endResetModel();
    emit summaryChanged();
    emit currentChanged();
}

void ReviewDeckModel::setCurrentPreview(const QString& asset_id, const QUrl& url) {
    if (asset_id != currentAssetId() || !url.isValid()) {
        return;
    }
    preview_asset_id_ = asset_id;
    preview_override_ = url;
    if (rowCount() == 1) {
        const auto model_index = index(0);
        emit dataChanged(model_index, model_index, {PreviewUrlRole});
    }
}

void ReviewDeckModel::clearCurrentPreview() {
    if (!preview_override_.isValid()) {
        return;
    }
    preview_asset_id_.clear();
    preview_override_.clear();
    if (rowCount() == 1) {
        const auto model_index = index(0);
        emit dataChanged(model_index, model_index, {PreviewUrlRole});
    }
}

void ReviewDeckModel::deleteCurrent() {
    applyDecision(Decision::Delete);
}

void ReviewDeckModel::keepCurrent() {
    applyDecision(Decision::Keep);
}

void ReviewDeckModel::skipCurrent() {
    applyDecision(Decision::Skip);
}

void ReviewDeckModel::undo() {
    beginResetModel();
    const auto changed = session_.undo();
    endResetModel();
    if (changed) {
        emit decisionUndone(currentAssetId());
        emit summaryChanged();
        emit currentChanged();
    }
}

void ReviewDeckModel::applyDecision(const Decision decision) {
    const auto asset_id = currentAssetId();
    beginResetModel();
    const auto changed = session_.decide_current(decision);
    endResetModel();
    if (changed) {
        const auto label = decision == Decision::Keep
                               ? QStringLiteral("keep")
                               : decision == Decision::Delete ? QStringLiteral("delete") : QString{};
        emit decisionApplied(asset_id, label);
        preview_asset_id_.clear();
        preview_override_.clear();
        emit summaryChanged();
        emit currentChanged();
    }
}

QString ReviewDeckModel::formatBytes(const std::uint64_t bytes) {
    constexpr auto gib = 1024.0 * 1024.0 * 1024.0;
    constexpr auto mib = 1024.0 * 1024.0;
    if (bytes >= static_cast<std::uint64_t>(gib)) {
        return QString::number(static_cast<double>(bytes) / gib, 'f', 1) + QStringLiteral(" GB");
    }
    return QString::number(static_cast<double>(bytes) / mib, 'f', 1) + QStringLiteral(" MB");
}

QString ReviewDeckModel::reasonLabel(const ReasonCode reason) {
    switch (reason) {
    case ReasonCode::ExactDuplicate:
        return QStringLiteral("Exact duplicate");
    case ReasonCode::NearDuplicate:
        return QStringLiteral("Similar shot kept");
    case ReasonCode::Blurry:
        return QStringLiteral("Very blurry");
    case ReasonCode::ScreenCapture:
        return QStringLiteral("Old screen capture");
    case ReasonCode::LargeVideo:
        return QStringLiteral("Large low-match video");
    case ReasonCode::LowPersonalMatch:
        return QStringLiteral("Unlike your keeps");
    }
    return QStringLiteral("Worth a look");
}

}  // namespace pms::desktop
