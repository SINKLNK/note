#include "ProcessPage.h"
#include "MonitoringClient.h"

#include "CellButtonDelegate.h"
#include "ListViewStyle.h"
#include "ProcessActions.h"
#include "ProcessDetailsDialog.h"
#include "ProcessFilterProxy.h"
#include "ProcessMonitor.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QTreeView>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <QFileInfo>

#include <Windows.h>
#include <Psapi.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace
{
// ShellExecuteEx does not expand %VAR% in lpFile, but the Run box is expected
// to accept "%windir%\notepad.exe" the way the system one does.
QString expandEnvironmentStrings(const QString &value)
{
    const std::wstring input = value.toStdWString();
    const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }

    std::vector<wchar_t> buffer(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(input.c_str(), buffer.data(), needed);
    if (written == 0 || written > needed) {
        return value;
    }

    // The returned length counts the terminating null.
    return QString::fromWCharArray(buffer.data(), static_cast<int>(written) - 1);
}

// Separate the executable from its arguments. A quoted first token is
// authoritative; otherwise an unquoted string that names an existing file is
// taken whole, so "C:\Program Files\App\x.exe" survives, and only then do we
// fall back to splitting at the first space for the "notepad file.txt" case.
void splitCommand(const QString &command, QString &file, QString &parameters)
{
    file = command;
    parameters.clear();

    if (command.startsWith(QLatin1Char('"'))) {
        const int closing = command.indexOf(QLatin1Char('"'), 1);
        if (closing > 0) {
            file = command.mid(1, closing - 1);
            parameters = command.mid(closing + 1).trimmed();
        } else {
            file = command.mid(1);
        }
        return;
    }

    if (QFileInfo::exists(command)) {
        return;
    }

    const int space = command.indexOf(QLatin1Char(' '));
    if (space > 0) {
        file = command.left(space);
        parameters = command.mid(space + 1).trimmed();
    }
}
} // namespace
#endif

namespace
{
// How long the mouse has to rest on a name before its copy button shows up.
// Names are what the user points at to read or select, so the button waits
// rather than flashing up over every row the mouse passes.
constexpr int kNameCopyButtonDelayMs = 2000;

// The columns without a button of their own -- CPU, PID, memory, GPU -- with
// their text drawn through CellTextCache like the rest. Their figures change
// every second, but a scroll repaints the same ones dozens of times in
// between, and the style would shape each of them twice per paint.
class CachedTextDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem cellOption = option;
        initStyleOption(&cellOption, index);
        if (!m_textCache.paint(painter, cellOption)) {
            QStyledItemDelegate::paint(painter, option, index);
        }
    }

private:
    // Several columns share it: a screenful is a few hundred cells.
    mutable CellTextCache m_textCache{1024};
};

// Scrolling moves the pixels already drawn and paints only the strip that
// comes into view. Stock QTreeView repainted the whole list on every wheel
// notch, for two reasons:
//
// - It weighs dy against the number of rows that fit, to repaint outright
//   any jump longer than a screen; in ScrollPerPixel mode dy is in pixels, so
//   every step over ~38 px counted as one (a notch is 78).
// - Even then, QWidget only moves the pixels of an opaque widget, and the
//   style sheet's background makes the viewport a styled, non-opaque one.
//   So the viewport claims to paint every pixel itself, and does: the
//   background is drawn here, the way Qt would have drawn it.
class ProcessTreeView : public QTreeView
{
public:
    explicit ProcessTreeView(QWidget *parent)
        : QTreeView(parent)
    {
        viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    }

protected:
    void dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                     const QList<int> &roles = {}) override
    {
        QTreeView::dataChanged(topLeft, bottomRight, roles);
        if (roles.isEmpty() || roles.contains(Qt::BackgroundRole)) {
            // Lifecycle colours extend beyond cell rectangles into tree
            // indentation and any unused space after the last column.
            // Endpoint columns may be hidden; obtain vertical bounds from
            // whichever column is actually visible.
            for (int column = 0; column < model()->columnCount(topLeft.parent()); ++column) {
                if (isColumnHidden(column)) {
                    continue;
                }
                QRect rows = visualRect(topLeft.siblingAtColumn(column))
                                 .united(visualRect(bottomRight.siblingAtColumn(column)));
                if (!rows.isEmpty()) {
                    rows.setLeft(0);
                    rows.setRight(viewport()->width() - 1);
                    viewport()->update(rows);
                }
                break;
            }
        }
    }

    void drawRow(QPainter *painter, const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override
    {
        QStyleOptionViewItem rowOption(option);
        const QBrush background = qvariant_cast<QBrush>(index.data(Qt::BackgroundRole));
        if (background.style() != Qt::NoBrush) {
            // Include the tree indentation and keep lifecycle colours visible
            // on the selected row as well as on ordinary cells.
            painter->fillRect(option.rect, background);
            rowOption.palette.setBrush(QPalette::Highlight, background);
            rowOption.palette.setBrush(QPalette::HighlightedText,
                                       rowOption.palette.brush(QPalette::Text));
        }
        QTreeView::drawRow(painter, rowOption, index);

        painter->save();
        painter->setPen(QColor(0xf2, 0xf2, 0xf2));
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }

    void scrollContentsBy(int dx, int dy) override
    {
        if (dy != 0 && verticalScrollMode() == ScrollPerPixel && qAbs(dy) < viewport()->height()) {
            // The horizontal part, and what QTreeView does on any scroll.
            QTreeView::scrollContentsBy(dx, 0);
            // What QTreeView does for a short step.
            scrollDirtyRegion(0, dy);
            viewport()->scroll(0, dy);
            return;
        }
        QTreeView::scrollContentsBy(dx, dy);
    }

    void paintEvent(QPaintEvent *event) override
    {
        {
            QPainter painter(viewport());
            QStyleOption option;
            option.initFrom(viewport());
            style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, viewport());
        }
        QTreeView::paintEvent(event);
    }
};
} // namespace

ProcessPage::ProcessPage(QWidget *parent)
    : QWidget(parent),
      m_view(new ProcessTreeView(this)),
      m_model(new ProcessModel(this)),
      m_proxy(new ProcessFilterProxy(this)),
      m_monitor(new ProcessMonitor())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

    // Filtering lives in a proxy so it stays out of the diff logic in the
    // model: the model keeps describing every process, the proxy decides which
    // of them the view is shown.
    m_proxy->setSourceModel(m_model);
    m_view->setModel(m_proxy);
    configureView();

    m_view->setItemDelegate(new CachedTextDelegate(this));

    auto *nameCopyButton = new CellButtonDelegate(m_view,
                                                  ProcessModel::NameColumn,
                                                  QStringLiteral(":/icons/copy.svg"),
                                                  QStringLiteral("Копировать рабочую папку и командную строку"
                                                                 " (в режиме дерева -- вместе с дочерними)"),
                                                  // The icon and the branch arrow
                                                  // live on the left.
                                                  Qt::RightEdge,
                                                  this);
    nameCopyButton->setShowDelay(kNameCopyButtonDelayMs);
    nameCopyButton->setCachesText(true);
    m_view->setItemDelegateForColumn(ProcessModel::NameColumn, nameCopyButton);
    connect(nameCopyButton, &CellButtonDelegate::clicked, this, &ProcessPage::copyProcessLaunch);

    auto *copyButton = new CellButtonDelegate(m_view,
                                              ProcessModel::CommandLineColumn,
                                              QStringLiteral(":/icons/copy.svg"),
                                              QStringLiteral("Копировать командную строку"),
                                              Qt::RightEdge,
                                              this);
    // Command lines run to a couple of thousand characters and never change
    // for the life of a process; lay each one out once, not on every repaint.
    copyButton->setCachesText(true);
    m_view->setItemDelegateForColumn(ProcessModel::CommandLineColumn, copyButton);
    connect(copyButton, &CellButtonDelegate::clicked, this, [](const QModelIndex &index) {
        const QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            QGuiApplication::clipboard()->setText(text);
        }
    });

    auto *trimButton = new CellButtonDelegate(m_view,
                                              ProcessModel::ResidentRamColumn,
                                              QStringLiteral(":/icons/trim.svg"),
                                              QStringLiteral("Урезать рабочий набор (EmptyWorkingSet)"),
                                              // The figures are right-aligned:
                                              // on the left the button sits in
                                              // empty space and they stay put.
                                              Qt::LeftEdge,
                                              this);
    trimButton->setCachesText(true);
    m_view->setItemDelegateForColumn(ProcessModel::ResidentRamColumn, trimButton);
    connect(trimButton, &CellButtonDelegate::clicked, this, &ProcessPage::trimWorkingSet);

    // Thaw only flips the flag; the re-sort itself waits for the next snapshot,
    // which goes through applySnapshot() and so keeps the newly current row
    // pinned on screen. Re-sorting right here would also pull rows out from
    // under applySnapshot() when it moves the selection itself.
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (m_model->sortFrozen() && keyForIndex(current) != m_sortFrozenFor) {
                    m_model->setSortFrozen(false);
                }
            });

    m_monitor->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_monitor, &ProcessMonitor::start);
    connect(&m_workerThread, &QThread::finished, m_monitor, &QObject::deleteLater);
    connect(m_monitor, &ProcessMonitor::snapshotReady, this, &ProcessPage::applySnapshot);
    connect(m_view, &QTreeView::doubleClicked, this, &ProcessPage::showDetails);

    connect(m_view, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        m_expandedProcesses.insert(keyForIndex(index));
    });
    connect(m_view, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        m_expandedProcesses.remove(keyForIndex(index));
    });

    // While a filter is on, a row the proxy admits is there only because
    // something in it matched, so it must not turn up behind a collapsed arrow.
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (!m_model->treeMode() || m_proxy->filterText().isEmpty()) {
                    return;
                }

                if (parent.isValid()) {
                    m_view->expand(parent);
                }
                for (int row = first; row <= last; ++row) {
                    m_view->expandRecursively(m_proxy->index(row, 0, parent));
                }
            });

    // Esc and Ctrl+Click on the current row are the way out of the pin in
    // applySnapshot(): without a current row the scrollbar is left alone.
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_view);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &ProcessPage::clearCurrentRow);
    m_view->viewport()->installEventFilter(this);

    m_workerThread.start();
}

ProcessPage::~ProcessPage()
{
    // quit() + wait() is the whole shutdown: unwinding the worker's event loop
    // stops the timer, and the deleteLater queued on QThread::finished destroys
    // the monitor before the thread is gone. The blocking invokeMethod that
    // used to precede this bought nothing -- it stalled the GUI thread for the
    // remainder of an in-flight snapshot only to wait again below, and had the
    // worker thread ever failed to start it would have blocked forever.
    m_workerThread.quit();
    m_workerThread.wait();
}

void ProcessPage::clearCurrentRow()
{
    // The selection and the current row are separate things, and it is the
    // current row that applySnapshot() pins, so both have to go.
    m_view->clearSelection();
    m_view->selectionModel()->clearCurrentIndex();
}

bool ProcessPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier)) {
            const QModelIndex index = m_view->indexAt(mouse->position().toPoint());
            if (index.isValid() && index.row() == m_view->currentIndex().row()
                && index.parent() == m_view->currentIndex().parent()) {
                clearCurrentRow();
                return true;
            }
        }

        // The empty part of the viewport under the last row.
        if (mouse->button() == Qt::LeftButton
            && !m_view->indexAt(mouse->position().toPoint()).isValid()) {
            clearCurrentRow();
        }
    }

    return QWidget::eventFilter(watched, event);
}

bool ProcessPage::treeMode() const
{
    return m_model->treeMode();
}

void ProcessPage::setTreeMode(bool enabled)
{
    if (m_model->treeMode() == enabled) {
        return;
    }

    // The row that was at the top of a flat CPU ordering becomes a leaf buried
    // in somebody's subtree, so the selection has to be carried across the
    // switch by hand. Not as a QPersistentModelIndex: re-parenting makes the
    // model announce that every parent changed, the proxy rebuilds its whole
    // mapping in response, and the persistent index does not survive it. The
    // process identity does, so remember that and look the row up again.
    const bool hadSelection = m_view->currentIndex().isValid();
    const ProcessEntry anchor = hadSelection ? currentEntry() : ProcessEntry{};

    m_model->setTreeMode(enabled);
    m_view->setRootIsDecorated(enabled);
    m_view->setItemsExpandable(enabled);
    applyTreeExpansion();

    if (hadSelection) {
        const QModelIndex restored = m_proxy->mapFromSource(m_model->indexForProcess(anchor));
        if (restored.isValid()) {
            // applyTreeExpansion() collapses first, so the row can sit inside a
            // closed branch; open the way to it before asking to show it.
            expandAncestors(restored);
            m_view->setCurrentIndex(restored);
            m_view->scrollTo(restored, QAbstractItemView::PositionAtCenter);
        }
    }

    emit treeModeChanged(enabled);
}

void ProcessPage::expandAncestors(const QModelIndex &index)
{
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        m_view->expand(parent);
    }
}

ProcessPage::ProcessKey ProcessPage::keyForIndex(const QModelIndex &proxyIndex) const
{
    const ProcessEntry entry = m_model->entryForIndex(m_proxy->mapToSource(proxyIndex));
    return {entry.pid, entry.creationTimeTicks};
}

void ProcessPage::restoreExpandedState()
{
    if (!m_model->treeMode() || m_expandedProcesses.isEmpty()) {
        return;
    }

    // Iterate a copy: expand() emits expanded(), which writes to the set.
    const QSet<ProcessKey> remembered = m_expandedProcesses;
    for (const ProcessKey &key : remembered) {
        ProcessEntry probe;
        probe.pid = key.first;
        probe.creationTimeTicks = key.second;

        const QModelIndex source = m_model->indexForProcess(probe);
        if (!source.isValid()) {
            // The process itself is gone, so there is nothing left to reopen.
            m_expandedProcesses.remove(key);
            continue;
        }

        const QModelIndex index = m_proxy->mapFromSource(source);
        if (index.isValid() && !m_view->isExpanded(index)) {
            m_view->expand(index);
        }
    }
}

void ProcessPage::setFilterText(const QString &text)
{
    m_proxy->setFilterText(text);
    applyTreeExpansion();
}

void ProcessPage::applyTreeExpansion()
{
    if (!m_model->treeMode()) {
        m_view->collapseAll();
        return;
    }

    // While filtering, a surviving branch exists only because something in it
    // matched, so expanding everything puts the matches in front of the user
    // instead of leaving them buried behind collapsed arrows.
    if (m_proxy->filterText().isEmpty()) {
        m_view->collapseAll();
        m_view->expandToDepth(1);
    } else {
        m_view->expandAll();
    }
}

void ProcessPage::killSelectedProcess()
{
    const ProcessEntry entry = currentEntry();
    if (entry.pid == 0) {
        return;
    }

    ProcessActions::terminateProcess(this, entry);
}

void ProcessPage::appendLaunchText(QString &out, const QModelIndex &proxyIndex, int depth) const
{
    const ProcessEntry entry = m_model->entryForIndex(m_proxy->mapToSource(proxyIndex));
    const QString indent(depth, QLatin1Char('\t'));

    // Read now rather than taken from the snapshot, which holds the directory
    // from when the process was first seen -- and a shell keeps moving. One
    // that has exited by now keeps that first one.
    QString directory;
#ifdef Q_OS_WIN
    directory = ProcessProviderWin::currentDirectory(entry.pid, entry.creationTimeTicks);
#endif
    if (directory.isEmpty()) {
        directory = entry.currentDirectory;
    }

    if (!directory.isEmpty()) {
        out += indent + directory + QLatin1Char('\n');
    }
    out += indent + entry.commandLine + QLatin1Char('\n');

    // The proxy's children, not the model's: what the view shows is what gets
    // copied, so a filter that trims a branch trims the copy as well. In the
    // flat list nothing has children and this stops at the row itself.
    const QModelIndex parent = proxyIndex.siblingAtColumn(0);
    for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
        appendLaunchText(out, m_proxy->index(row, 0, parent), depth + 1);
    }
}

void ProcessPage::copyProcessLaunch(const QModelIndex &proxyIndex)
{
    QString text;
    appendLaunchText(text, proxyIndex, 0);
    text.chop(1); // The newline after the last line.

    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void ProcessPage::trimWorkingSet(const QModelIndex &proxyIndex)
{
    const ProcessEntry entry = m_model->entryForIndex(m_proxy->mapToSource(proxyIndex));
    if (entry.pid == 0) {
        return;
    }

#ifdef Q_OS_WIN
    // Same as Perforce\TrimWS.ps1: hand every page of the working set back to
    // the standby/modified lists. Nothing is freed -- the process faults back
    // in whatever it touches next -- so no confirmation, the next tick simply
    // shows what Resident RAM was actually being used.
    HANDLE process = ProcessActions::openSameProcess(this, entry, PROCESS_SET_QUOTA);
    if (!process) {
        return;
    }

    const bool ok = EmptyWorkingSet(process);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!ok) {
        QMessageBox::warning(this,
                             QStringLiteral("Ошибка"),
                             QStringLiteral("Не удалось урезать рабочий набор %1 (PID %2): %3")
                                 .arg(entry.name)
                                 .arg(entry.pid)
                                 .arg(qt_error_string(static_cast<int>(error))));
        return;
    }

    // Sorted by Resident RAM, the trimmed row would drop to the bottom on the
    // next tick and drag the scroll position with it. Hold every row where it
    // is instead -- the sort indicator stays as it was -- for as long as the
    // trimmed process remains the current row. Making it current first is what
    // gives "until the user moves on" a meaning: the button swallows the click,
    // so otherwise the selection could be anywhere.
    if (m_sortColumn == ProcessModel::ResidentRamColumn) {
        m_view->setCurrentIndex(proxyIndex.siblingAtColumn(ProcessModel::NameColumn));
        m_sortFrozenFor = {entry.pid, entry.creationTimeTicks};
        m_model->setSortFrozen(true);
    }
#else
    QMessageBox::information(this, QStringLiteral("Unsupported"), QStringLiteral("Только на Windows."));
#endif
}

void ProcessPage::runNewTask()
{
    const QString text = QInputDialog::getText(this,
                                               QStringLiteral("Запустить новую задачу"),
                                               QStringLiteral("Команда или путь к EXE:"));
    if (text.trimmed().isEmpty()) {
        return;
    }

#ifdef Q_OS_WIN
    // CreateProcessW with a null lpApplicationName makes Win32 guess where the
    // executable name ends by splitting lpCommandLine on spaces, trying each
    // prefix in turn: "C:\Program Files\App\x.exe" first attempts to launch
    // C:\Program.exe. That is both a broken Run box and a well known hijack
    // vector. ShellExecuteEx takes the file and its arguments as separate
    // fields, and additionally resolves App Paths, shortcuts and documents the
    // way the real Run dialog does.
    const QString expanded = expandEnvironmentStrings(text.trimmed());
    QString file;
    QString parameters;
    splitCommand(expanded, file, parameters);

    const std::wstring fileNative = file.toStdWString();
    const std::wstring parametersNative = parameters.toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = reinterpret_cast<HWND>(window()->winId());
    info.lpFile = fileNative.c_str();
    info.lpParameters = parametersNative.empty() ? nullptr : parametersNative.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        QMessageBox::warning(this,
                             QStringLiteral("Ошибка"),
                             QStringLiteral("Не удалось запустить «%1».").arg(file));
        return;
    }

    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
#else
    QMessageBox::information(this, QStringLiteral("Unsupported"), QStringLiteral("Только на Windows."));
#endif
}

void ProcessPage::applySnapshot(const QVector<ProcessEntry> &entries)
{
    emit snapshotApplied(entries);

    // The model applies this as a difference, so selection and expanded
    // branches survive on their own. What does not survive is where the
    // selected row sits on screen: processes appear, exit and reorder above it
    // every second, and each of those slides it up or down under the cursor.
    //
    // So pin it. Measure where the row is in the viewport now, and put it back
    // at the same place afterwards by moving the scrollbar instead. The
    // scrollbar jumping around is the point -- it is what keeps the row still.
    // Manual scrolling keeps working for the same reason: the next refresh
    // measures from wherever the user has just put the row, so their offset is
    // carried forward rather than fought over, even off-screen.
    const QModelIndex current = m_view->currentIndex();
    const int anchorColumn = current.isValid() ? current.column() : 0;
    const QRect anchorRect = current.isValid() ? m_view->visualRect(current) : QRect();

    // The current row and then its ancestors, by identity. The ancestors are
    // the fallback for the moment the selected process finally disappears: the
    // selection climbs to its parent, which is where the user was looking,
    // rather than being dropped wherever the view happens to leave it.
    QList<ProcessEntry> anchorChain;
    for (QModelIndex index = current; index.isValid(); index = index.parent()) {
        anchorChain.append(m_model->entryForIndex(m_proxy->mapToSource(index)));
    }

    m_model->setEntries(entries);

    // Reopen anything that quietly stopped counting as expanded, before
    // measuring where the anchor row landed: a row inside a branch the view no
    // longer considers open has no position on screen to restore.
    restoreExpandedState();

    if (anchorChain.isEmpty()) {
        return;
    }

    const QModelIndex restored = indexForChain(anchorChain, anchorColumn);
    if (!restored.isValid()) {
        return;
    }

    if (restored != m_view->currentIndex()) {
        m_view->setCurrentIndex(restored);
    }

    // Null means the row is inside a collapsed branch and has no place on
    // screen to hold; a negative or oversized top is fine and expected, since
    // the anchor is allowed to be scrolled out of sight.
    const QRect rect = m_view->visualRect(restored);
    if (anchorRect.isNull() || rect.isNull()) {
        return;
    }

    QScrollBar *bar = m_view->verticalScrollBar();
    bar->setValue(bar->value() + rect.top() - anchorRect.top());
}

QModelIndex ProcessPage::indexForChain(const QList<ProcessEntry> &chain, int column) const
{
    // The selection model holds the current row as a persistent index into the
    // proxy, and those stop describing the row they were made for once the
    // proxy rebuilds its mapping: the address survives while the process at
    // that address does not. So resolve it from identity instead, taking the
    // first member of the chain that still exists.
    for (const ProcessEntry &entry : chain) {
        const QModelIndex source = m_model->indexForProcess(entry);
        if (!source.isValid()) {
            continue;
        }

        const QModelIndex index = m_proxy->mapFromSource(source).siblingAtColumn(column);
        if (index.isValid()) {
            return index;
        }
    }

    return {};
}

bool ProcessPage::revealProcess(quint32 pid, quint64 creationTimeTicks)
{
    ProcessEntry probe;
    probe.pid = pid;
    probe.creationTimeTicks = creationTimeTicks;

    const QModelIndex index = m_proxy->mapFromSource(m_model->indexForProcess(probe));
    if (!index.isValid()) {
        return false;
    }

    // In tree mode the row can sit inside a closed branch.
    expandAncestors(index);
    m_view->setCurrentIndex(index.siblingAtColumn(ProcessModel::NameColumn));
    m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    m_view->setFocus();
    return true;
}

void ProcessPage::showDetails(const QModelIndex &index)
{
    ProcessDetailsDialog dialog(m_model->entryForIndex(m_proxy->mapToSource(index)), this);
    dialog.exec();
}

ProcessEntry ProcessPage::currentEntry() const
{
    // The view speaks proxy indexes; the entry lives in the source model.
    return m_model->entryForIndex(m_proxy->mapToSource(m_view->currentIndex()));
}

void ProcessPage::configureView()
{
    m_view->setAlternatingRowColors(false);
    m_view->setUniformRowHeights(true);
    m_view->setSortingEnabled(false);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setAllColumnsShowFocus(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setIndentation(18);
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setExpandsOnDoubleClick(false);
    m_view->setAnimated(false);
    m_view->setIconSize(QSize(20, 20));
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->verticalScrollBar()->setSingleStep(26);

    auto *header = m_view->header();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(40);
    header->setSectionsClickable(true);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(m_sortColumn, m_sortOrder);
    connect(header, &QHeaderView::customContextMenuRequested,
            this, &ProcessPage::showColumnMenu);
    connect(header, &QHeaderView::sectionClicked,
            this, &ProcessPage::sortByHeaderColumn);

    for (int column = 0; column < ProcessModel::ColumnCount; ++column) {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    header->setSectionResizeMode(ProcessModel::CommandLineColumn, QHeaderView::Stretch);

    m_view->setColumnWidth(ProcessModel::NameColumn, 220);
    m_view->setColumnWidth(ProcessModel::CpuColumn, 58);
    m_view->setColumnWidth(ProcessModel::PidColumn, 68);
    m_view->setColumnWidth(ProcessModel::StatusColumn, 92);
    m_view->setColumnWidth(ProcessModel::ResidentRamColumn, 100);
    m_view->setColumnWidth(ProcessModel::CommittedRamColumn, 108);
    m_view->setColumnWidth(ProcessModel::DiskRateColumn, 82);
    m_view->setColumnWidth(ProcessModel::NetworkRateColumn, 82);
    m_view->setColumnWidth(ProcessModel::DedicatedGpuMemoryColumn, 76);
    m_view->setColumnWidth(ProcessModel::SharedGpuMemoryColumn, 76);
    m_view->setColumnHidden(ProcessModel::StatusColumn, true);

    // A QSS item border bypasses Qt's BackgroundRole painting. Draw the
    // separators in ProcessTreeView instead, leaving cell backgrounds native.
    m_view->setStyleSheet(listViewStyleSheet(false));
}

void ProcessPage::showColumnMenu(const QPoint &pos)
{
    QMenu menu(this);

    for (int column = 0; column < ProcessModel::ColumnCount; ++column) {
        const QString title = m_model->columnTitle(column);
        QAction *action = menu.addAction(title);
        action->setCheckable(true);
        action->setChecked(!m_view->isColumnHidden(column));

        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            m_view->setColumnHidden(column, !visible);
        });
    }

    menu.exec(m_view->header()->mapToGlobal(pos));
}

void ProcessPage::sortByHeaderColumn(int column)
{
    const bool metric = ProcessModel::isMetricColumn(column);

    if (column == m_sortColumn && column == ProcessModel::CpuColumn) {
        // CPU has three states instead of two: instantaneous descending,
        // ascending, then descending on the average of the last few seconds.
        // The first click on the column lands on the first of them, so the
        // cycle is entered at "instantaneous descending".
        if (m_cpuSmoothed) {
            m_cpuSmoothed = false;
            m_sortOrder = Qt::DescendingOrder;
        } else if (m_sortOrder == Qt::DescendingOrder) {
            m_sortOrder = Qt::AscendingOrder;
        } else {
            m_cpuSmoothed = true;
            m_sortOrder = Qt::DescendingOrder;
        }
    } else if (column == m_sortColumn) {
        m_sortOrder = m_sortOrder == Qt::AscendingOrder
                          ? Qt::DescendingOrder
                          : Qt::AscendingOrder;
    } else {
        m_sortColumn = column;
        m_cpuSmoothed = false;
        // A first click on a metric asks "what is using the most of this", so
        // it starts at the largest. A first click on a name, a PID or a command
        // line asks for a list to read, so it starts at the beginning.
        m_sortOrder = metric ? Qt::DescendingOrder : Qt::AscendingOrder;
    }

    // Name, PID and command line describe one flat ordering of all processes.
    // A tree contradicts that: it sorts only within each branch, so the
    // alphabetical run keeps restarting at every parent. Drop to the list.
    if (!metric) {
        setTreeMode(false);
    }

    m_model->setCpuSortSmoothed(m_cpuSmoothed);
    m_model->sort(m_sortColumn, m_sortOrder);
    m_view->header()->setSortIndicator(m_sortColumn, m_sortOrder);

    if (metric) {
        // The top row is the answer to the question a metric sort asks, so go
        // there and select it.
        const QModelIndex first = m_proxy->index(0, ProcessModel::NameColumn);
        if (first.isValid()) {
            m_view->setCurrentIndex(first);
            m_view->scrollTo(first, QAbstractItemView::PositionAtTop);
        }
        return;
    }

    // An identity sort is a change of viewpoint, not a question: the process
    // the user was looking at stays selected -- the model reports the reorder
    // as a layout change, so that happens by itself -- and the view is scrolled
    // to put it back in the middle, where it is easy to pick up again.
    const QModelIndex current = m_view->currentIndex();
    if (current.isValid()) {
        m_view->scrollTo(current, QAbstractItemView::PositionAtCenter);
    }
}
void ProcessPage::attachMonitoringClient(MonitoringClient* client)
{
    connect(client, &MonitoringClient::snapshotReady, m_monitor, &ProcessMonitor::setServiceSnapshot, Qt::QueuedConnection);
    connect(client, &MonitoringClient::connectionChanged, m_monitor, [this](bool connected) {
        if (!connected) m_monitor->clearServiceSnapshot();
    }, Qt::QueuedConnection);
}
