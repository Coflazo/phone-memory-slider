#include "review_deck_model.hpp"

#include <QUrl>

#include <utility>
#include <vector>

namespace pms::desktop {
namespace {

std::vector<ReviewItem> fixture_items() {
    return {
        {{"city-duplicate", 0.96F, 0.18F, 0.42F, false, false}, MediaKind::Photo, 4'820'000, "Night walk", {ReasonCode::NearDuplicate}, "qrc:/qt/qml/PhoneMemorySlider/resources/fixture-city.svg"},
        {{"lake-blur", 0.88F, 0.44F, 0.30F, false, false}, MediaKind::Photo, 3'460'000, "Lake at dusk", {ReasonCode::Blurry}, "qrc:/qt/qml/PhoneMemorySlider/resources/fixture-lake.svg"},
        {{"screen-old", 0.78F, 0.12F, 0.08F, false, false}, MediaKind::Photo, 780'000, "Old confirmation", {ReasonCode::ScreenCapture}, "qrc:/qt/qml/PhoneMemorySlider/resources/fixture-screen.svg"},
        {{"clip-large", 0.63F, 0.20F, 1.00F, false, false}, MediaKind::Video, 842'000'000, "Concert clip", {ReasonCode::LargeVideo}, "qrc:/qt/qml/PhoneMemorySlider/resources/fixture-city.svg"},
        {{"favorite", 0.91F, 0.98F, 0.70F, true, false}, MediaKind::Photo, 5'200'000, "Favorite", {ReasonCode::LowPersonalMatch}, "qrc:/qt/qml/PhoneMemorySlider/resources/fixture-lake.svg"},
    };
}

}  // namespace

ReviewDeckModel::ReviewDeckModel(QObject* parent)
    : QAbstractListModel(parent), session_(fixture_items()) {}

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

bool ReviewDeckModel::complete() const noexcept {
    return session_.remaining() == 0;
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
        emit summaryChanged();
    }
}

void ReviewDeckModel::applyDecision(const Decision decision) {
    beginResetModel();
    const auto changed = session_.decide_current(decision);
    endResetModel();
    if (changed) {
        emit summaryChanged();
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

