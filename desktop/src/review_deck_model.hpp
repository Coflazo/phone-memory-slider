#pragma once

#include <QAbstractListModel>
#include <QString>

#include "pms/review_session.hpp"

namespace pms::desktop {

class ReviewDeckModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int remaining READ remaining NOTIFY summaryChanged)
    Q_PROPERTY(QString pendingBytesText READ pendingBytesText NOTIFY summaryChanged)
    Q_PROPERTY(bool complete READ complete NOTIFY summaryChanged)

public:
    enum Role {
        AssetIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        ReasonRole,
        StorageTextRole,
        MediaKindRole,
        PreviewUrlRole,
    };

    explicit ReviewDeckModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int remaining() const noexcept;
    [[nodiscard]] QString pendingBytesText() const;
    [[nodiscard]] bool complete() const noexcept;

    Q_INVOKABLE void deleteCurrent();
    Q_INVOKABLE void keepCurrent();
    Q_INVOKABLE void skipCurrent();
    Q_INVOKABLE void undo();

signals:
    void summaryChanged();

private:
    void applyDecision(Decision decision);
    [[nodiscard]] static QString formatBytes(std::uint64_t bytes);
    [[nodiscard]] static QString reasonLabel(ReasonCode reason);

    ReviewSession session_;
};

}  // namespace pms::desktop

