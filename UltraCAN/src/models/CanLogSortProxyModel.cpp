#include "CanLogSortProxyModel.h"

#include "models/CanLogModel.h"

CanLogSortProxyModel::CanLogSortProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    // THE line that makes this usable. With it true, every dataChanged from a
    // 1 kHz bus would re-evaluate the row's position and issue a move; Count
    // alone changes on every frame. See the class note.
    setDynamicSortFilter(false);
}

void CanLogSortProxyModel::setLogModel(CanLogModel *model)
{
    if (m_log == model)
        return;

    if (m_log)
        disconnect(m_log, nullptr, this, nullptr);

    m_log = model;
    setSourceModel(model);
    if (!m_log)
        return;

    connect(m_log, &CanLogModel::overwriteModeChanged, this, [this] {
        // Leaving overwrite mode must drop the sort, not merely hide the
        // headers: a chronological trace whose rows are still ordered by id is
        // a trace that has lost the only thing it was for.
        if (!m_log->overwriteMode())
            clearSort();
        emit sortStateChanged();
    });
    connect(m_log, &CanLogModel::countChanged, this, &CanLogSortProxyModel::countChanged);
    emit sortStateChanged();
}

bool CanLogSortProxyModel::sortingAvailable() const
{
    return m_log && m_log->overwriteMode();
}

bool CanLogSortProxyModel::overwriteMode() const
{
    return m_log && m_log->overwriteMode();
}

int CanLogSortProxyModel::roleFor(const QString &roleName) const
{
    if (roleName == QLatin1String("canId") || roleName == QLatin1String("idText"))
        return CanLogModel::CanIdRole;       // always the NUMBER, never the text
    if (roleName == QLatin1String("count"))
        return CanLogModel::CountRole;
    if (roleName == QLatin1String("frequency"))
        return CanLogModel::FrequencyRole;
    if (roleName == QLatin1String("type") || roleName == QLatin1String("extended"))
        return CanLogModel::ExtendedRole;
    return -1;
}

void CanLogSortProxyModel::toggleSort(const QString &roleName)
{
    if (!sortingAvailable())
        return;

    const int role = roleFor(roleName);
    if (role < 0) {
        clearSort();
        return;
    }

    if (m_sortRoleName == roleName) {
        m_descending = !m_descending;
    } else {
        m_sortRoleName = roleName;
        // First click on a numeric column shows the biggest first, because
        // "which id is busiest" is the question being asked. Type is a label,
        // so it starts A-Z.
        m_descending = (role != CanLogModel::ExtendedRole);
    }

    setSortRole(role);
    sort(0, m_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    emit sortStateChanged();
}

void CanLogSortProxyModel::resort()
{
    if (m_sortRoleName.isEmpty() || !sortingAvailable())
        return;
    // invalidate() first: with dynamicSortFilter off the proxy will otherwise
    // reuse the ordering it already computed and the call does nothing.
    invalidate();
    sort(0, m_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void CanLogSortProxyModel::clearSort()
{
    if (m_sortRoleName.isEmpty())
        return;
    m_sortRoleName.clear();
    m_descending = false;
    // -1 is "no sort": the proxy returns to source order rather than keeping
    // the last ordering frozen.
    sort(-1);
    emit sortStateChanged();
}

int CanLogSortProxyModel::sourceRow(int proxyRow) const
{
    if (proxyRow < 0 || proxyRow >= rowCount())
        return -1;
    return mapToSource(index(proxyRow, 0)).row();
}

bool CanLogSortProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const int role = sortRole();
    const QVariant a = sourceModel()->data(left, role);
    const QVariant b = sourceModel()->data(right, role);

    switch (role) {
    case CanLogModel::CanIdRole:
        // toUInt, not the default QVariant comparison via string: "0x1F" sorts
        // before "0x9" lexically, which is the classic way an id column comes
        // out looking sorted and is not.
        return a.toUInt() < b.toUInt();

    case CanLogModel::CountRole:
        return a.toLongLong() < b.toLongLong();

    case CanLogModel::FrequencyRole: {
        const double x = a.toDouble();
        const double y = b.toDouble();
        // A stale identity reads 0 Hz. Ties on 0 would otherwise fall back to
        // whatever order the ring happens to hold, and the dead rows would
        // shuffle among themselves on every re-sort; breaking the tie on the
        // id keeps them still.
        if (qFuzzyCompare(x + 1.0, y + 1.0))
            return sourceModel()->data(left, CanLogModel::CanIdRole).toUInt()
                 < sourceModel()->data(right, CanLogModel::CanIdRole).toUInt();
        return x < y;
    }

    case CanLogModel::ExtendedRole: {
        // The column reads "Std"/"Ext", and the user asked for it
        // alphabetically - so compare the LABELS, not the bool. Ext < Std.
        const QString x = a.toBool() ? QStringLiteral("Ext") : QStringLiteral("Std");
        const QString y = b.toBool() ? QStringLiteral("Ext") : QStringLiteral("Std");
        if (x == y)
            return sourceModel()->data(left, CanLogModel::CanIdRole).toUInt()
                 < sourceModel()->data(right, CanLogModel::CanIdRole).toUInt();
        return x < y;
    }

    default:
        return QSortFilterProxyModel::lessThan(left, right);
    }
}
