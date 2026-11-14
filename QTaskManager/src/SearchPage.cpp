#include "SearchPage.h"

#include "ListViewStyle.h"
#include "ProcessActions.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
enum Column {
    ProcessColumn = 0,
    PidColumn,
    TypeColumn,
    HandleColumn,
    NameColumn,
    CommandLineColumn,
    ColumnCount
};

constexpr int SortRole = Qt::UserRole;
constexpr int PidRole = Qt::UserRole + 1;
constexpr int CreationRole = Qt::UserRole + 2;

constexpr int DebounceMs = 2000;

// PIDs and handles are numbers, and must sort as such: "9" before "10".
class ResultItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : ProcessColumn;
        if (column == PidColumn || column == HandleColumn) {
            return data(column, SortRole).toULongLong() < other.data(column, SortRole).toULongLong();
        }
        return QString::compare(text(column), other.text(column), Qt::CaseInsensitive) < 0;
    }
};
} // namespace

SearchPage::SearchPage(QWidget *parent)
    : QWidget(parent),
      m_edit(new QLineEdit(this)),
      m_status(new QLabel(this)),
      m_tree(new QTreeWidget(this)),
      m_debounce(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget(this);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(12, 10, 12, 10);
    barLayout->setSpacing(10);

    auto *label = new QLabel(QStringLiteral("Дескриптор или DLL:"), bar);
    m_edit->setObjectName(QStringLiteral("SearchEdit"));
    m_edit->setPlaceholderText(QStringLiteral("Часть имени файла, ключа реестра, мьютекса, DLL…"));
    m_edit->setClearButtonEnabled(true);
    m_edit->addAction(QIcon(QStringLiteral(":/icons/search.svg")), QLineEdit::LeadingPosition);
    m_status->setStyleSheet(QStringLiteral("color: #666;"));

    barLayout->addWidget(label);
    barLayout->addWidget(m_edit, 1);
    barLayout->addWidget(m_status);
    layout->addWidget(bar);

    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({QStringLiteral("Процесс"),
                             QStringLiteral("PID"),
                             QStringLiteral("Тип"),
                             QStringLiteral("Дескриптор"),
                             QStringLiteral("Имя"),
                             QStringLiteral("Командная строка")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setIconSize(QSize(20, 20));
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->verticalScrollBar()->setSingleStep(26);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ProcessColumn, Qt::AscendingOrder);
    m_tree->setStyleSheet(listViewStyleSheet());

    auto *header = m_tree->header();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(40);
    for (int column = 0; column < ColumnCount; ++column) {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    header->setSectionResizeMode(CommandLineColumn, QHeaderView::Stretch);
    m_tree->setColumnWidth(ProcessColumn, 200);
    m_tree->setColumnWidth(PidColumn, 68);
    m_tree->setColumnWidth(TypeColumn, 90);
    m_tree->setColumnWidth(HandleColumn, 90);
    m_tree->setColumnWidth(NameColumn, 520);
    layout->addWidget(m_tree, 1);

    m_debounce->setSingleShot(true);
    m_debounce->setInterval(DebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &SearchPage::startSearch);
    connect(m_edit, &QLineEdit::textChanged, this, &SearchPage::onTextChanged);
    // Enter does not wait out the delay.
    connect(m_edit, &QLineEdit::returnPressed, this, [this] {
        if (!m_edit->text().trimmed().isEmpty()) {
            m_debounce->stop();
            startSearch();
        }
    });

    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        emit processActivated(item->data(ProcessColumn, PidRole).toUInt(),
                              item->data(ProcessColumn, CreationRole).toULongLong());
    });

    updateStatus();
}

SearchPage::~SearchPage()
{
    if (m_thread) {
        m_cancel->store(true);
        // The queued `finished` handler must not run against a dying page.
        m_thread->disconnect(this);
        m_thread->wait();
        delete m_thread;
    }
}

void SearchPage::focusInput()
{
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

void SearchPage::setProcessSnapshot(const QVector<ProcessEntry> &entries)
{
    m_commandLines.clear();
    for (const ProcessEntry &entry : entries) {
        m_commandLines.insert({entry.pid, entry.creationTimeTicks}, entry.commandLine);
    }
}

void SearchPage::killSelectedProcess()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        return;
    }

    ProcessEntry entry;
    entry.pid = item->data(ProcessColumn, PidRole).toUInt();
    entry.creationTimeTicks = item->data(ProcessColumn, CreationRole).toULongLong();
    entry.name = item->text(ProcessColumn);

    if (!ProcessActions::terminateProcess(this, entry)) {
        return;
    }

    // Its handles and modules went with it.
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        const QTreeWidgetItem *other = m_tree->topLevelItem(i);
        if (other->data(ProcessColumn, PidRole).toUInt() == entry.pid
            && other->data(ProcessColumn, CreationRole).toULongLong() == entry.creationTimeTicks) {
            delete m_tree->takeTopLevelItem(i);
            --m_resultCount;
        }
    }
    updateStatus();
}

void SearchPage::onTextChanged()
{
    // Whatever is running answers a question that is no longer being asked.
    cancelRunning();

    if (m_edit->text().trimmed().isEmpty()) {
        m_debounce->stop();
        clearResults();
        m_startPending = false;
        updateStatus();
        return;
    }

    m_debounce->start();
    m_status->setText(QStringLiteral("Ожидание ввода…"));
}

void SearchPage::cancelRunning()
{
    ++m_generation;
    m_startPending = false;
    if (m_thread && m_cancel) {
        m_cancel->store(true);
    }
    m_running = false;
}

void SearchPage::startSearch()
{
    const QString needle = m_edit->text().trimmed();
    if (needle.isEmpty()) {
        return;
    }

    cancelRunning();
    if (m_thread) {
        // The superseded sweep is still unwinding; threadFinished() resumes.
        m_startPending = true;
        m_status->setText(QStringLiteral("Поиск…"));
        return;
    }

    clearResults();
    m_running = true;
    updateStatus();

    const quint64 generation = m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;

    m_thread = QThread::create([this, needle, cancel, generation] {
        const HandleSearchStats stats = HandleSearch::run(
            needle, *cancel, [this, generation](QVector<HandleMatch> &&batch) {
                QMetaObject::invokeMethod(
                    this, [this, generation, batch] { addMatches(generation, batch); },
                    Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(
            this, [this, generation, stats] { finishSearch(generation, stats); },
            Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, this, &SearchPage::threadFinished);
    m_thread->start();
}

void SearchPage::threadFinished()
{
    m_thread->deleteLater();
    m_thread = nullptr;

    if (m_startPending) {
        m_startPending = false;
        startSearch();
    }
}

void SearchPage::clearResults()
{
    m_tree->clear();
    m_resultCount = 0;
}

void SearchPage::addMatches(quint64 generation, const QVector<HandleMatch> &matches)
{
    if (generation != m_generation) {
        return;
    }

    QList<QTreeWidgetItem *> items;
    items.reserve(matches.size());
    for (const HandleMatch &match : matches) {
        auto *item = new ResultItem();
        item->setText(ProcessColumn, match.processName);
        item->setIcon(ProcessColumn, m_icons.icon(match.imagePath));
        item->setData(ProcessColumn, PidRole, match.pid);
        item->setData(ProcessColumn, CreationRole, match.creationTimeTicks);

        item->setText(PidColumn, QString::number(match.pid));
        item->setData(PidColumn, SortRole, match.pid);
        item->setTextAlignment(PidColumn, Qt::AlignRight | Qt::AlignVCenter);

        item->setText(TypeColumn, match.type);

        if (match.handle != 0) {
            item->setText(HandleColumn, QStringLiteral("0x") + QString::number(match.handle, 16).toUpper());
        }
        item->setData(HandleColumn, SortRole, match.handle);

        item->setText(NameColumn, match.name);
        item->setToolTip(NameColumn, match.name);
        item->setText(CommandLineColumn, m_commandLines.value({match.pid, match.creationTimeTicks}));
        item->setToolTip(ProcessColumn, match.imagePath);
        items.append(item);
    }

    m_tree->setUpdatesEnabled(false);
    m_tree->addTopLevelItems(items);
    m_tree->setUpdatesEnabled(true);

    m_resultCount += static_cast<int>(items.size());
    updateStatus();
}

void SearchPage::finishSearch(quint64 generation, const HandleSearchStats &stats)
{
    if (generation != m_generation) {
        return;
    }

    m_running = false;

    QStringList notes;
    if (stats.truncated) {
        notes << QStringLiteral("показаны первые %1").arg(HandleSearch::MaxMatches);
    }
    if (stats.inaccessibleProcesses > 0) {
        notes << QStringLiteral("нет доступа к процессам: %1").arg(stats.inaccessibleProcesses);
    }
    updateStatus(notes.join(QStringLiteral(" · ")));
}

void SearchPage::updateStatus(const QString &suffix)
{
    if (m_running) {
        m_status->setText(QStringLiteral("Поиск… найдено: %1").arg(m_resultCount));
        return;
    }

    if (m_edit->text().trimmed().isEmpty()) {
        m_status->setText(QString());
        return;
    }

    QString text = QStringLiteral("Найдено: %1").arg(m_resultCount);
    if (!suffix.isEmpty()) {
        text += QStringLiteral(" · ") + suffix;
    }
    m_status->setText(text);
}
