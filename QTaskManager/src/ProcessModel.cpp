#include "ProcessModel.h"

#include <QColor>
#include <QDateTime>
#include <QSet>

#include <algorithm>
#include <vector>

namespace
{
// How long a row stays tinted after the process appears or exits.
constexpr qint64 kHighlightMs = 5000;

// The smoothed CPU sort is an exponential moving average fed once per snapshot,
// one a second. The usual N-period weight, 2 / (N + 1), with N = 5 puts the
// average age of what it remembers at about two seconds and lets a spike fade
// to a few percent of its height in roughly five.
constexpr int kCpuSmoothingTicks = 5;
constexpr double kCpuSmoothingAlpha = 2.0 / (kCpuSmoothingTicks + 1);

QVariant alignmentForColumn(int column)
{
    switch (column) {
    case ProcessModel::PidColumn:
    case ProcessModel::CpuColumn:
    case ProcessModel::ResidentRamColumn:
    case ProcessModel::CommittedRamColumn:
    case ProcessModel::DiskRateColumn:
    case ProcessModel::NetworkRateColumn:
    case ProcessModel::DedicatedGpuMemoryColumn:
    case ProcessModel::SharedGpuMemoryColumn:
        return int(Qt::AlignRight | Qt::AlignVCenter);
    default:
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
}
}

ProcessModel::ProcessModel(QObject *parent)
    : QAbstractItemModel(parent),
      m_root(std::make_unique<Node>())
{
}

ProcessModel::~ProcessModel() = default;

bool ProcessModel::isMetricColumn(int column)
{
    switch (column) {
    case CpuColumn:
    case ResidentRamColumn:
    case CommittedRamColumn:
    case DiskRateColumn:
    case NetworkRateColumn:
    case DedicatedGpuMemoryColumn:
    case SharedGpuMemoryColumn:
        return true;
    default:
        return false;
    }
}

ProcessModel::ProcessKey ProcessModel::keyFor(const ProcessEntry &entry)
{
    return {entry.pid, entry.creationTimeTicks};
}

QModelIndex ProcessModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return {};
    }

    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode || row < 0 || row >= static_cast<int>(parentNode->children.size())) {
        return {};
    }

    return createIndex(row, column, parentNode->children[static_cast<size_t>(row)]);
}

QModelIndex ProcessModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return {};
    }

    Node *node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == m_root.get()) {
        return {};
    }

    // Node::row is kept current by reindexChildren(), so this no longer has to
    // scan the grandparent's children to find out where the parent sits.
    return createIndex(node->parent->row, 0, node->parent);
}

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    Node *parentNode = nodeFromIndex(parent);
    return parentNode ? static_cast<int>(parentNode->children.size()) : 0;
}

int ProcessModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    Node *node = nodeFromIndex(index);
    if (!node) {
        return {};
    }

    // Tint the whole row while it is newly arrived or on its way out. This is
    // the one piece of display that depends on the node rather than on the
    // process data, so it is answered here instead of in columnData().
    if (role == Qt::BackgroundRole) {
        if (node->diedAtMs != 0) {
            return QColor(0xff, 0xdf, 0xdf);
        }
        if (node->bornAtMs != 0) {
            return QColor(0xdd, 0xf3, 0xdd);
        }
        return {};
    }

    return columnData(node->entry, index.column(), role);
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) {
        return {};
    }

    if (role == Qt::DisplayRole) {
        const QString title = columnTitle(section);
        // The sort arrow only says ascending or descending, so the third state
        // of the CPU sort is marked in the title: "≈" for "approximately",
        // i.e. averaged rather than instantaneous.
        if (section == CpuColumn && m_cpuSortSmoothed) {
            return QStringLiteral("≈") + title;
        }
        return title;
    }

    if (role == Qt::ToolTipRole && section == CpuColumn) {
        return QStringLiteral("Клик по заголовку переключает сортировку:\n"
                              "▼ по убыванию (мгновенное значение)\n"
                              "▲ по возрастанию\n"
                              "≈▼ по убыванию, сглаженное за последние ~5 с");
    }
    if (role == Qt::ToolTipRole && section == DiskRateColumn) {
        return QStringLiteral("Файловый I/O процесса: сумма размеров запросов чтения и записи, MB/s.\n"
                              "Включает работу через файловый кэш. Не является скоростью физического диска; "
                              "размер запроса не подтверждает успешное завершение операции.");
    }

    if (role == Qt::TextAlignmentRole) {
        return alignmentForColumn(section);
    }

    return {};
}

QString ProcessModel::columnTitle(int column) const
{
    switch (column) {
    case NameColumn: return QStringLiteral("Имя");
    case CpuColumn: return QStringLiteral("ЦП");
    case PidColumn: return QStringLiteral("PID");
    case StatusColumn: return QStringLiteral("Состояние");
    case ResidentRamColumn: return QStringLiteral("Resident RAM");
    case CommittedRamColumn: return QStringLiteral("Committed RAM");
    case DiskRateColumn: return QStringLiteral("Файлы, MB/s");
    case NetworkRateColumn: return QStringLiteral("Сеть, MB/s");
    case CommandLineColumn: return QStringLiteral("Командная строка");
    case DedicatedGpuMemoryColumn: return QStringLiteral("GPU D RAM");
    case SharedGpuMemoryColumn: return QStringLiteral("GPU S RAM");
    default: return {};
    }
}

Qt::ItemFlags ProcessModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

void ProcessModel::sort(int column, Qt::SortOrder order)
{
    // No early return for an unchanged column and order: the CPU sort mode can
    // change underneath them, and resort() is already a no-op when every
    // branch is in order.
    m_sortColumn = column;
    m_sortOrder = order;
    m_sortFrozen = false;
    resort();
}

void ProcessModel::setCpuSortSmoothed(bool smoothed)
{
    if (m_cpuSortSmoothed == smoothed) {
        return;
    }

    m_cpuSortSmoothed = smoothed;
    emit headerDataChanged(Qt::Horizontal, CpuColumn, CpuColumn);
}

bool ProcessModel::cpuSortSmoothed() const
{
    return m_cpuSortSmoothed;
}

void ProcessModel::setSortFrozen(bool frozen)
{
    m_sortFrozen = frozen;
}

bool ProcessModel::sortFrozen() const
{
    return m_sortFrozen;
}

void ProcessModel::setEntries(const QVector<ProcessEntry> &entries)
{
    // The first snapshot has nothing to diff against, so it goes in as a reset.
    // Every snapshot after that is applied as a difference: the rows that
    // vanished are removed, the rows that stayed are updated in place, the rows
    // that appeared are inserted, and the reordering is announced separately.
    // The view therefore keeps its selection, its expanded branches and its
    // scroll position on its own, which a full reset makes impossible.
    if (m_nodes.empty()) {
        resetTo(entries);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QHash<ProcessKey, const ProcessEntry *> incoming;
    incoming.reserve(entries.size());
    for (const ProcessEntry &entry : entries) {
        incoming.insert(keyFor(entry), &entry);
    }

    // Order matters: retire the ghosts whose time is up before marking this
    // tick's departures, so a process cannot be marked and reaped at once.
    expireGhosts(now);
    markVanished(incoming, now);
    updateSurvivors(incoming);
    insertArrived(entries, now);
    fadeHighlights(now);
    if (!m_sortFrozen) {
        resort();
    }
}

void ProcessModel::resetTo(const QVector<ProcessEntry> &entries)
{
    beginResetModel();

    m_nodeByKey.clear();
    m_nodeByPid.clear();
    m_nodes.clear();
    m_root = std::make_unique<Node>();

    m_nodes.reserve(static_cast<size_t>(entries.size()));
    for (const ProcessEntry &entry : entries) {
        auto owned = std::make_unique<Node>();
        owned->entry = entry;
        owned->smoothedCpu = entry.cpuPercent;
        Node *node = owned.get();
        m_nodes.push_back(std::move(owned));
        m_nodeByKey.insert(keyFor(entry), node);
        m_nodeByPid.insert(entry.pid, node);
    }

    relinkTree();
    sortChildrenRecursive(m_root.get());

    endResetModel();
}

void ProcessModel::markVanished(const QHash<ProcessKey, const ProcessEntry *> &incoming,
                               qint64 now)
{
    for (const auto &owned : m_nodes) {
        Node *node = owned.get();
        if (node->diedAtMs != 0 || incoming.contains(keyFor(node->entry))) {
            continue;
        }

        node->diedAtMs = now;
        node->bornAtMs = 0;

        // Stop offering a dead process as a parent, so a newly started process
        // that inherits its PID is not attached to the ghost.
        if (m_nodeByPid.value(node->entry.pid, nullptr) == node) {
            m_nodeByPid.remove(node->entry.pid);
        }

        emitRowChanged(node);
    }
}

void ProcessModel::fadeHighlights(qint64 now)
{
    for (const auto &owned : m_nodes) {
        Node *node = owned.get();
        if (node->bornAtMs != 0 && now - node->bornAtMs >= kHighlightMs) {
            node->bornAtMs = 0;
            emitRowChanged(node);
        }
    }
}

void ProcessModel::emitRowChanged(Node *node)
{
    const QModelIndex left = indexForNode(node);
    if (left.isValid()) {
        emit dataChanged(left, left.siblingAtColumn(ColumnCount - 1));
    }
}

void ProcessModel::expireGhosts(qint64 now)
{
    m_carriedBirth.clear();

    const auto expired = [now](const Node *node) {
        return node->diedAtMs != 0 && now - node->diedAtMs >= kHighlightMs;
    };

    // Only the top-most expiring node of a chain is removed explicitly; anything
    // below it leaves with its subtree. Live descendants of an expiring parent
    // go too, and insertArrived() puts them back at the root afterwards -- that
    // costs those few rows their expanded state, but only when their parent
    // actually exits, and it avoids moving subtrees around mid-update.
    std::vector<Node *> dead;
    for (const auto &owned : m_nodes) {
        Node *node = owned.get();
        if (!expired(node)) {
            continue;
        }

        bool ancestorExpired = false;
        for (Node *ancestor = node->parent; ancestor && ancestor != m_root.get();
             ancestor = ancestor->parent) {
            if (expired(ancestor)) {
                ancestorExpired = true;
                break;
            }
        }

        if (!ancestorExpired) {
            dead.push_back(node);
        }
    }

    for (Node *node : dead) {
        Node *parentNode = node->parent;
        if (!parentNode) {
            continue;
        }

        std::vector<Node *> subtree;
        collectSubtree(node, subtree);

        // Everything under an expiring ghost goes with it, including processes
        // that are still running. Those come straight back in insertArrived(),
        // so carry their arrival time across the gap: they are being rebuilt,
        // not started, and marking them green would say the opposite.
        for (Node *doomed : subtree) {
            if (doomed->diedAtMs == 0) {
                m_carriedBirth.insert(keyFor(doomed->entry), doomed->bornAtMs);
            }
        }

        beginRemoveRows(indexForNode(parentNode), node->row, node->row);

        for (Node *doomed : subtree) {
            m_nodeByKey.remove(keyFor(doomed->entry));
            if (m_nodeByPid.value(doomed->entry.pid, nullptr) == doomed) {
                m_nodeByPid.remove(doomed->entry.pid);
            }
        }

        parentNode->children.erase(parentNode->children.begin() + node->row);
        reindexChildren(parentNode);

        const QSet<Node *> doomed(subtree.cbegin(), subtree.cend());
        m_nodes.erase(std::remove_if(m_nodes.begin(),
                                     m_nodes.end(),
                                     [&doomed](const std::unique_ptr<Node> &owned) {
                                         return doomed.contains(owned.get());
                                     }),
                      m_nodes.end());

        endRemoveRows();
    }
}

void ProcessModel::updateSurvivors(const QHash<ProcessKey, const ProcessEntry *> &incoming)
{
    for (const auto &owned : m_nodes) {
        Node *node = owned.get();
        const auto it = incoming.constFind(keyFor(node->entry));
        if (it == incoming.cend()) {
            continue;
        }

        // Fed even when the row is otherwise unchanged: an idle process must
        // keep decaying. The average is not on screen, so it needs no signal.
        node->smoothedCpu += kCpuSmoothingAlpha * (it.value()->cpuPercent - node->smoothedCpu);

        if (node->entry == *it.value()) {
            continue;
        }

        node->entry = *it.value();

        const QModelIndex left = indexForNode(node);
        emit dataChanged(left, left.sibling(left.row(), ColumnCount - 1));
    }
}

void ProcessModel::insertArrived(const QVector<ProcessEntry> &entries, qint64 now)
{
    std::vector<const ProcessEntry *> arrivals;
    for (const ProcessEntry &entry : entries) {
        if (!m_nodeByKey.contains(keyFor(entry))) {
            arrivals.push_back(&entry);
        }
    }

    if (arrivals.empty()) {
        return;
    }

    // A parent is always older than its child, so inserting in creation order
    // guarantees the parent is already in the tree when the child looks it up.
    std::sort(arrivals.begin(),
              arrivals.end(),
              [](const ProcessEntry *a, const ProcessEntry *b) {
                  return a->creationTimeTicks < b->creationTimeTicks;
              });

    for (const ProcessEntry *entry : arrivals) {
        auto owned = std::make_unique<Node>();
        owned->entry = *entry;
        owned->smoothedCpu = entry->cpuPercent;
        owned->bornAtMs = m_carriedBirth.value(keyFor(*entry), now);
        Node *node = owned.get();
        m_nodes.push_back(std::move(owned));
        m_nodeByKey.insert(keyFor(*entry), node);
        m_nodeByPid.insert(entry->pid, node);

        Node *parentNode = m_treeMode ? parentFor(node) : m_root.get();
        const int row = static_cast<int>(parentNode->children.size());

        // Appended at the end and left there: resort() moves it into place in
        // the same update, as a reorder rather than another insert -- or, while
        // the sort is frozen, on the first update after it thaws.
        beginInsertRows(indexForNode(parentNode), row, row);
        node->parent = parentNode;
        node->row = row;
        parentNode->children.push_back(node);
        endInsertRows();
    }
}

void ProcessModel::setTreeMode(bool enabled)
{
    if (m_treeMode == enabled) {
        return;
    }

    m_treeMode = enabled;
    // Re-parenting re-sorts every branch below, so a freeze cannot outlive it.
    m_sortFrozen = false;

    if (m_nodes.empty()) {
        return;
    }

    // Switching between list and tree gives every node a different parent but
    // does not change the set of rows, which is precisely what layoutChanged
    // expresses, and a reset would be far heavier.
    //
    // The parent list is deliberately empty here: re-parenting really does
    // affect every parent, unlike a re-sort. The cost is that a proxy on top
    // rebuilds its whole mapping and persistent indexes do not reliably come
    // through, so callers must re-find a row by process identity afterwards
    // (indexForProcess) rather than holding a QPersistentModelIndex across it.
    emit layoutAboutToBeChanged();
    // Only after the signal: see resort().
    const QModelIndexList oldIndexes = persistentIndexList();

    relinkTree();
    sortChildrenRecursive(m_root.get());
    remapPersistentIndexes(oldIndexes);

    emit layoutChanged();
}

bool ProcessModel::treeMode() const
{
    return m_treeMode;
}

ProcessEntry ProcessModel::entryForIndex(const QModelIndex &index) const
{
    Node *node = index.isValid() ? nodeFromIndex(index) : nullptr;
    return node ? node->entry : ProcessEntry{};
}

QModelIndex ProcessModel::indexForProcess(const ProcessEntry &entry) const
{
    Node *node = m_nodeByKey.value(keyFor(entry), nullptr);
    return node ? indexForNode(node) : QModelIndex();
}

void ProcessModel::relinkTree()
{
    m_root->children.clear();
    for (const auto &owned : m_nodes) {
        owned->parent = nullptr;
        owned->children.clear();
    }

    if (!m_treeMode) {
        for (const auto &owned : m_nodes) {
            owned->parent = m_root.get();
            m_root->children.push_back(owned.get());
        }
        reindexChildren(m_root.get());
        return;
    }

    // Resolve every intended parent before a single link is made, so that a
    // PPID cycle can be broken while the graph is still nothing but pointers.
    QHash<Node *, Node *> intended;
    intended.reserve(static_cast<int>(m_nodes.size()));
    for (const auto &owned : m_nodes) {
        Node *candidate = m_nodeByPid.value(owned->entry.parentPid, nullptr);
        if (isPlausibleParent(candidate, owned.get())) {
            intended.insert(owned.get(), candidate);
        }
    }

    for (const auto &owned : m_nodes) {
        Node *walker = intended.value(owned.get(), nullptr);
        for (size_t step = 0; walker && step <= m_nodes.size(); ++step) {
            if (walker == owned.get()) {
                intended.remove(owned.get());
                break;
            }
            walker = intended.value(walker, nullptr);
        }
    }

    for (const auto &owned : m_nodes) {
        Node *parentNode = intended.value(owned.get(), m_root.get());
        owned->parent = parentNode;
        parentNode->children.push_back(owned.get());
    }

    reindexChildren(m_root.get());
    for (const auto &owned : m_nodes) {
        reindexChildren(owned.get());
    }
}

ProcessModel::Node *ProcessModel::parentFor(Node *node) const
{
    Node *candidate = m_nodeByPid.value(node->entry.parentPid, nullptr);
    if (!isPlausibleParent(candidate, node)) {
        return m_root.get();
    }

    for (Node *walker = candidate; walker; walker = walker->parent) {
        if (walker == node) {
            return m_root.get();
        }
    }

    return candidate;
}

bool ProcessModel::isPlausibleParent(const Node *parent, const Node *child)
{
    // Windows recycles PIDs aggressively and Toolhelp keeps reporting the
    // parent PID long after that parent exited, so a live process often carries
    // the PID of a stranger that inherited it. Accept the link only when the
    // candidate is genuinely older. When either timestamp is unknown -- the
    // process refused to open -- there is no evidence against the link, so keep
    // it rather than flattening the system subtrees we can never read.
    if (!parent || parent == child) {
        return false;
    }
    if (parent->entry.creationTimeTicks == 0 || child->entry.creationTimeTicks == 0) {
        return true;
    }
    return parent->entry.creationTimeTicks <= child->entry.creationTimeTicks;
}

void ProcessModel::resort()
{
    QList<QPersistentModelIndex> staleParents;
    collectStaleParents(m_root.get(), staleParents);
    if (staleParents.isEmpty()) {
        return;
    }

    // A reorder is not a change of contents, so it is announced with
    // layoutChanged plus a persistent-index remap instead of a reset. That is
    // what lets the view keep selection and expanded branches across it.
    //
    // Naming the parents that actually reordered is not an optimisation, it is
    // the difference between working and not: layoutChanged with an empty
    // parent list means "everything changed", and a QSortFilterProxyModel
    // answers that by throwing away and rebuilding its entire mapping. Any
    // persistent index that does not survive the rebuild is dropped, including
    // the ones QTreeView keeps to remember which branches are expanded -- so
    // once a second, a filtered tree snapped shut on its own.
    emit layoutAboutToBeChanged(staleParents);

    // Collected only after the signal, never before: listeners create
    // persistent indexes while handling it -- QSortFilterProxyModel saves one
    // into this model for every persistent index of its own -- and expect them
    // remapped. One left out keeps its old row and parent, the proxy can no
    // longer map it back, and the persistent index table ends up corrupted.
    const QModelIndexList oldIndexes = persistentIndexList();

    sortChildrenRecursive(m_root.get());
    remapPersistentIndexes(oldIndexes);

    emit layoutChanged(staleParents);
}

void ProcessModel::collectStaleParents(Node *node, QList<QPersistentModelIndex> &out) const
{
    const auto comparator = [this](const Node *left, const Node *right) {
        return nodeLessThan(left, right);
    };

    if (!std::is_sorted(node->children.begin(), node->children.end(), comparator)) {
        // An invalid index here is not a failure: it is how "the top level
        // reordered" is spelled in a layoutChanged parent list.
        out.append(QPersistentModelIndex(indexForNode(node)));
    }

    for (Node *child : node->children) {
        collectStaleParents(child, out);
    }
}

void ProcessModel::sortChildrenRecursive(Node *node)
{
    if (!node) {
        return;
    }

    // Stable, so that rows tied on the sort column -- and at idle almost every
    // row is tied on CPU -- keep their relative order instead of shuffling and
    // forcing a pointless relayout every second.
    std::stable_sort(node->children.begin(),
                     node->children.end(),
                     [this](const Node *left, const Node *right) {
                         return nodeLessThan(left, right);
                     });
    reindexChildren(node);

    for (Node *child : node->children) {
        sortChildrenRecursive(child);
    }
}

bool ProcessModel::nodeLessThan(const Node *left, const Node *right) const
{
    if (m_sortColumn == CpuColumn && m_cpuSortSmoothed) {
        return m_sortOrder == Qt::AscendingOrder ? left->smoothedCpu < right->smoothedCpu
                                                 : left->smoothedCpu > right->smoothedCpu;
    }
    return lessThan(left->entry, right->entry, m_sortColumn, m_sortOrder);
}

void ProcessModel::remapPersistentIndexes(const QModelIndexList &oldIndexes)
{
    QModelIndexList newIndexes;
    newIndexes.reserve(oldIndexes.size());

    for (const QModelIndex &old : oldIndexes) {
        auto *node = static_cast<Node *>(old.internalPointer());
        newIndexes.append(node ? createIndex(node->row, old.column(), node) : QModelIndex());
    }

    changePersistentIndexList(oldIndexes, newIndexes);
}

void ProcessModel::collectSubtree(Node *node, std::vector<Node *> &out)
{
    out.push_back(node);
    for (Node *child : node->children) {
        collectSubtree(child, out);
    }
}

void ProcessModel::reindexChildren(Node *node)
{
    for (size_t i = 0; i < node->children.size(); ++i) {
        node->children[i]->row = static_cast<int>(i);
    }
}

ProcessModel::Node *ProcessModel::nodeFromIndex(const QModelIndex &index) const
{
    if (index.isValid()) {
        return static_cast<Node *>(index.internalPointer());
    }
    return m_root.get();
}

QModelIndex ProcessModel::indexForNode(Node *node) const
{
    if (!node || node == m_root.get()) {
        return {};
    }
    return createIndex(node->row, 0, node);
}

QVariant ProcessModel::columnData(const ProcessEntry &entry, int column, int role) const
{
    if (role == Qt::DisplayRole) {
        switch (column) {
        case NameColumn: return entry.name;
        case CpuColumn: return QString::number(entry.cpuPercent, 'f', entry.cpuPercent >= 10.0 ? 0 : 1) + QStringLiteral("%");
        case PidColumn: return entry.pid;
        case StatusColumn: return QStringLiteral("Выполняется");
        case ResidentRamColumn: return formatBytes(entry.residentRamBytes);
        case CommittedRamColumn: return formatBytes(entry.committedRamBytes);
        case DiskRateColumn: return entry.diskRateMBps < 0.0 ? QStringLiteral("—") : formatRate(entry.diskRateMBps);
        case NetworkRateColumn: return entry.networkRateMBps < 0.0 ? QStringLiteral("—") : formatRate(entry.networkRateMBps);
        case CommandLineColumn: return entry.commandLine;
        case DedicatedGpuMemoryColumn: return formatBytes(entry.dedicatedGpuMemoryBytes);
        case SharedGpuMemoryColumn: return formatBytes(entry.sharedGpuMemoryBytes);
        default: return {};
        }
    }

    if (role == Qt::DecorationRole && column == NameColumn) {
        // data() runs for every visible row on every repaint, so the key must
        // be cheap: the image path from the snapshot is already absolute and
        // stable. It used to go through QFileInfo::canonicalFilePath(), which
        // resolves symlinks and junctions on the filesystem -- a syscall per
        // row per frame, and a real stall on network paths.
        return m_icons.icon(entry.executablePath);
    }

    if (role == Qt::TextAlignmentRole) {
        return alignmentForColumn(column);
    }

    if (role == Qt::ToolTipRole) {
        if (column == CommandLineColumn) {
            return entry.commandLine;
        }
        if (column == NameColumn) {
            if (!entry.windowTitle.isEmpty()) {
                return entry.windowTitle;
            }
            if (!entry.executablePath.isEmpty()) {
                return entry.executablePath;
            }
        }
        if (column == DiskRateColumn && entry.diskRateMBps < 0.0) {
            return QStringLiteral("Данные файлового I/O недоступны. Проверьте службу мониторинга в настройках.");
        }
        if (column == NetworkRateColumn && entry.networkRateMBps < 0.0) {
            return QStringLiteral("Данные сети недоступны. Проверьте службу мониторинга в настройках.");
        }
        if ((column == DiskRateColumn && entry.diskQuality == ProcessEntry::IoQuality::Partial)
            || (column == NetworkRateColumn && entry.networkQuality == ProcessEntry::IoQuality::Partial)) {
            return QStringLiteral("Неполные данные ETW: часть событий потеряна или не сопоставлена с процессом.");
        }
    }

    return {};
}

QString ProcessModel::formatBytes(quint64 bytes) const
{
    const double kb = static_cast<double>(bytes) / 1024.0;
    const double mb = kb / 1024.0;
    const double gb = mb / 1024.0;

    if (gb >= 1.0) {
        return QString::number(gb, 'f', 2) + QStringLiteral(" ГБ");
    }
    if (mb >= 1.0) {
        return QString::number(mb, 'f', 1) + QStringLiteral(" МБ");
    }
    return QString::number(kb, 'f', 0) + QStringLiteral(" КБ");
}

QString ProcessModel::formatRate(double rateMBps) const
{
    return QString::number(rateMBps, 'f', rateMBps >= 10.0 ? 1 : 2);
}

bool ProcessModel::lessThan(const ProcessEntry &a, const ProcessEntry &b, int column, Qt::SortOrder order)
{
    auto compareValue = [order](auto lhs, auto rhs) {
        return order == Qt::AscendingOrder ? lhs < rhs : lhs > rhs;
    };
    auto compareString = [order](const QString &lhs, const QString &rhs) {
        const int cmp = QString::compare(lhs, rhs, Qt::CaseInsensitive);
        return order == Qt::AscendingOrder ? (cmp < 0) : (cmp > 0);
    };

    switch (column) {
    case NameColumn: return compareString(a.name, b.name);
    case CpuColumn: return compareValue(a.cpuPercent, b.cpuPercent);
    case PidColumn: return compareValue(a.pid, b.pid);
    case StatusColumn: return false;
    case ResidentRamColumn: return compareValue(a.residentRamBytes, b.residentRamBytes);
    case CommittedRamColumn: return compareValue(a.committedRamBytes, b.committedRamBytes);
    case DiskRateColumn: return compareValue(a.diskRateMBps, b.diskRateMBps);
    case NetworkRateColumn: return compareValue(a.networkRateMBps, b.networkRateMBps);
    case CommandLineColumn: return compareString(a.commandLine, b.commandLine);
    case DedicatedGpuMemoryColumn: return compareValue(a.dedicatedGpuMemoryBytes, b.dedicatedGpuMemoryBytes);
    case SharedGpuMemoryColumn: return compareValue(a.sharedGpuMemoryBytes, b.sharedGpuMemoryBytes);
    default: return compareString(a.name, b.name);
    }
}
