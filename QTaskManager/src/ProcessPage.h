#pragma once

#include "ProcessModel.h"

#include <QPair>
#include <QSet>
#include <QThread>
#include <QWidget>

class QEvent;
class QTreeView;
class ProcessMonitor;
class ProcessFilterProxy;
class MonitoringClient;

class ProcessPage : public QWidget
{
    Q_OBJECT
public:
    explicit ProcessPage(QWidget *parent = nullptr);
    ~ProcessPage() override;

    bool treeMode() const;
    void attachMonitoringClient(MonitoringClient* client);

public slots:
    void setTreeMode(bool enabled);
    void setFilterText(const QString &text);
    void killSelectedProcess();
    void runNewTask();

    // Let go of the current row. applySnapshot() holds that row still on
    // screen by moving the scrollbar; with no row to hold it leaves the
    // scroll position alone, so the view simply shows the live ordering.
    void clearCurrentRow();

    // Select and scroll to a process row, opening the branches above it.
    // False when no such process is listed any more. A filter that hides the
    // row is the caller's to clear first.
    bool revealProcess(quint32 pid, quint64 creationTimeTicks);

signals:
    void treeModeChanged(bool enabled);

    // Every fresh snapshot, for pages that want to describe processes they
    // found by other means (the search page's command line column).
    void snapshotApplied(const QVector<ProcessEntry> &entries);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void applySnapshot(const QVector<ProcessEntry> &entries);
    void showDetails(const QModelIndex &index);

private:
    QTreeView *m_view = nullptr;
    ProcessModel *m_model = nullptr;
    ProcessFilterProxy *m_proxy = nullptr;
    QThread m_workerThread;
    ProcessMonitor *m_monitor = nullptr;
    // Kept in step with ProcessModel's own defaults, so the header's sort
    // indicator and the three-state CPU cycle in sortByHeaderColumn() start
    // out agreeing with what the model is already sorted by.
    int m_sortColumn = ProcessModel::CpuColumn;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    bool m_cpuSmoothed = true; // Third CPU sort state: descending on a moving average.

    // Which branches are open is remembered here, by process identity.
    // QTreeView keeps that state as persistent indexes into the proxy, and
    // those quietly stop matching when the proxy rebuilds its mapping: no
    // collapsed() is ever emitted, the branch simply stops counting as open.
    using ProcessKey = QPair<quint32, quint64>;
    QSet<ProcessKey> m_expandedProcesses;

    // The process whose trim froze the Resident RAM sort. The freeze lasts
    // while it stays the current row; see trimWorkingSet().
    ProcessKey m_sortFrozenFor;

    ProcessKey keyForIndex(const QModelIndex &proxyIndex) const;
    void restoreExpandedState();
    QModelIndex indexForChain(const QList<ProcessEntry> &chain, int column) const;

    ProcessEntry currentEntry() const;
    void trimWorkingSet(const QModelIndex &proxyIndex);

    // Working directory and command line of a process and, in tree mode, of
    // everything below it, one tab deeper per level.
    void copyProcessLaunch(const QModelIndex &proxyIndex);
    void appendLaunchText(QString &out, const QModelIndex &proxyIndex, int depth) const;
    void configureView();
    void applyTreeExpansion();
    void expandAncestors(const QModelIndex &index);
    void showColumnMenu(const QPoint &pos);
    void sortByHeaderColumn(int column);
};
