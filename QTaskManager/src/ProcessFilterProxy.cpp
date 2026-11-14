#include "ProcessFilterProxy.h"

#include "ProcessModel.h"

ProcessFilterProxy::ProcessFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);

    // Qt's own recursive filtering keeps a parent whose descendant matches, but
    // it has no notion of keeping the descendants of a match. Both halves are
    // implemented in filterAcceptsRow() instead, so leave it off.
    setRecursiveFilteringEnabled(false);
}

void ProcessFilterProxy::setFilterText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_filter == trimmed) {
        return;
    }

    m_filter = trimmed;
    invalidateFilter();
}

QString ProcessFilterProxy::filterText() const
{
    return m_filter;
}

bool ProcessFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_filter.isEmpty()) {
        return true;
    }

    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!index.isValid()) {
        return false;
    }

    // A row survives for one of three reasons: it matches; an ancestor matches,
    // which is what keeps the whole subtree of a match visible; or a descendant
    // matches, which is what keeps the match reachable from the root. Siblings
    // along that chain lead nowhere and are dropped, so an ancestor shows up
    // without the rest of its children.
    return matchesSelfOrAncestor(index) || hasMatchingDescendant(index);
}

bool ProcessFilterProxy::matches(const QModelIndex &sourceIndex) const
{
    const QAbstractItemModel *model = sourceModel();

    const QString name = model->data(sourceIndex.siblingAtColumn(ProcessModel::NameColumn),
                                     Qt::DisplayRole)
                             .toString();
    if (name.contains(m_filter, Qt::CaseInsensitive)) {
        return true;
    }

    const QString pid = model->data(sourceIndex.siblingAtColumn(ProcessModel::PidColumn),
                                    Qt::DisplayRole)
                            .toString();
    return pid.contains(m_filter);
}

bool ProcessFilterProxy::matchesSelfOrAncestor(const QModelIndex &sourceIndex) const
{
    for (QModelIndex index = sourceIndex; index.isValid(); index = index.parent()) {
        if (matches(index)) {
            return true;
        }
    }
    return false;
}

bool ProcessFilterProxy::hasMatchingDescendant(const QModelIndex &sourceIndex) const
{
    const QAbstractItemModel *model = sourceModel();
    const int rows = model->rowCount(sourceIndex);

    for (int row = 0; row < rows; ++row) {
        const QModelIndex child = model->index(row, 0, sourceIndex);
        if (matches(child) || hasMatchingDescendant(child)) {
            return true;
        }
    }

    return false;
}
