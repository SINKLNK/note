#pragma once

#include <QSortFilterProxyModel>
#include <QString>

// Filters processes by name or PID. In tree mode a match drags its whole
// ancestor chain and its whole subtree along with it, while branches that lead
// to no match disappear entirely.
class ProcessFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit ProcessFilterProxy(QObject *parent = nullptr);

    void setFilterText(const QString &text);
    QString filterText() const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool matches(const QModelIndex &sourceIndex) const;
    bool matchesSelfOrAncestor(const QModelIndex &sourceIndex) const;
    bool hasMatchingDescendant(const QModelIndex &sourceIndex) const;

    QString m_filter;
};
