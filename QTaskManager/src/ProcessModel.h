#pragma once

#include "FileIconCache.h"
#include "ProcessEntry.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QPair>
#include <memory>
#include <vector>

class ProcessModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Column {
        NameColumn = 0,
        CpuColumn,
        PidColumn,
        StatusColumn,
        ResidentRamColumn,
        CommittedRamColumn,
        DiskRateColumn,
        NetworkRateColumn,
        CommandLineColumn,
        DedicatedGpuMemoryColumn,
        SharedGpuMemoryColumn,
        ColumnCount
    };

    explicit ProcessModel(QObject *parent = nullptr);
    ~ProcessModel() override;

    // A metric column holds a measured quantity (CPU, memory, throughput); the
    // rest identify the process (name, PID, command line). The two want
    // opposite defaults almost everywhere: a metric is interesting largest
    // first and its top row is the answer, while an identity is read in
    // ascending order and the row you already had selected is the answer.
    static bool isMetricColumn(int column);

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // The plain column title, without the marker headerData() adds while the
    // CPU sort is smoothed. Menus that list the columns want this one.
    QString columnTitle(int column) const;

    // Makes a sort on the CPU column compare a moving average of the last few
    // seconds instead of the instantaneous value, so a row that spikes for one
    // tick does not jump to the top and fall off again. It only sets the mode;
    // the rows move on the next sort() or snapshot. The header title carries a
    // marker while it is on.
    void setCpuSortSmoothed(bool smoothed);
    bool cpuSortSmoothed() const;

    void setEntries(const QVector<ProcessEntry> &entries);

    // While frozen, setEntries() still updates every value but leaves the rows
    // where they are: no reorder, new processes stay appended at the end of
    // their parent. The sort column and order are untouched, so thawing simply
    // lets the next setEntries() put everything back in order. sort() and
    // setTreeMode() thaw on their own, since both re-sort explicitly.
    void setSortFrozen(bool frozen);
    bool sortFrozen() const;

    void setTreeMode(bool enabled);
    bool treeMode() const;
    ProcessEntry entryForIndex(const QModelIndex &index) const;

    // Find a process again after the tree has been restructured. Identity is
    // the PID plus the creation time, never the row -- the row is precisely
    // what moves, and a QPersistentModelIndex is no help either once a proxy
    // has rebuilt its mapping in response to the restructuring.
    QModelIndex indexForProcess(const ProcessEntry &entry) const;

private:
    // A process is identified by its PID together with its creation time. PIDs
    // on their own are recycled, and a recycled PID must not be taken for the
    // same row surviving into the next snapshot.
    using ProcessKey = QPair<quint32, quint64>;

    // Ownership of every node is flat and lives in m_nodes; the tree itself is
    // nothing but non-owning pointers. That keeps node identity stable across
    // the restructuring that tree mode and parent death cause, and it makes an
    // ownership cycle structurally impossible rather than merely unlikely.
    struct Node
    {
        ProcessEntry entry;
        Node *parent = nullptr;
        std::vector<Node *> children;
        int row = 0;

        // A row outlives its process by a few seconds so that its departure is
        // visible. Until diedAtMs expires the node is a ghost: still a row,
        // still sortable, but no longer updated and no longer a candidate
        // parent. bornAtMs marks the opposite, a row that has just appeared.
        qint64 bornAtMs = 0;
        qint64 diedAtMs = 0;

        // Exponential moving average of entry.cpuPercent, fed once per
        // snapshot. It lives on the node, not in the entry, so that it does not
        // take part in the change detection that drives dataChanged.
        double smoothedCpu = 0.0;
    };

    std::unique_ptr<Node> m_root;
    std::vector<std::unique_ptr<Node>> m_nodes;
    QHash<ProcessKey, Node *> m_nodeByKey;
    QHash<quint32, Node *> m_nodeByPid;

    // Live processes torn down along with an expiring ghost ancestor are
    // re-inserted in the same update. They are not new, so their original
    // arrival time is carried across the gap instead of being reset.
    QHash<ProcessKey, qint64> m_carriedBirth;

    bool m_treeMode = false;
    bool m_sortFrozen = false;
    // CPU descending, smoothed, is the default view: "what is using the most
    // CPU right now" is the question people open the task manager to answer,
    // and the average keeps a one-tick spike from reshuffling the top rows.
    bool m_cpuSortSmoothed = true;
    int m_sortColumn = CpuColumn;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    mutable FileIconCache m_icons;

    static ProcessKey keyFor(const ProcessEntry &entry);
    static bool isPlausibleParent(const Node *parent, const Node *child);
    static void collectSubtree(Node *node, std::vector<Node *> &out);
    static void reindexChildren(Node *node);

    Node *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexForNode(Node *node) const;
    Node *parentFor(Node *node) const;

    void resetTo(const QVector<ProcessEntry> &entries);
    void expireGhosts(qint64 now);
    void markVanished(const QHash<ProcessKey, const ProcessEntry *> &incoming, qint64 now);
    void updateSurvivors(const QHash<ProcessKey, const ProcessEntry *> &incoming);
    void insertArrived(const QVector<ProcessEntry> &entries, qint64 now);
    void fadeHighlights(qint64 now);
    void emitRowChanged(Node *node);
    void relinkTree();
    void resort();
    void collectStaleParents(Node *node, QList<QPersistentModelIndex> &out) const;
    void sortChildrenRecursive(Node *node);
    bool nodeLessThan(const Node *left, const Node *right) const;
    void remapPersistentIndexes(const QModelIndexList &oldIndexes);

    QVariant columnData(const ProcessEntry &entry, int column, int role) const;
    QString formatBytes(quint64 bytes) const;
    QString formatRate(double rateMBps) const;
    static bool lessThan(const ProcessEntry &a, const ProcessEntry &b, int column, Qt::SortOrder order);
};
