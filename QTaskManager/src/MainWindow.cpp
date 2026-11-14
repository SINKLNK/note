#include "MainWindow.h"

#include "PerformancePage.h"
#include "ProcessPage.h"
#include "SearchPage.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSettings>
#include <QMessageBox>
#include <QCloseEvent>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("QTaskManager"));
    resize(1500, 860);
    setMinimumSize(1100, 680);
    setWindowIcon(QIcon(QStringLiteral(":/icons/details.svg")));

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("CentralPanel"));
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(createSidebar());

    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("ContentPanel"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_header = createHeader();
    contentLayout->addWidget(m_header);

    m_stack = new QStackedWidget(this);
    m_processPage = new ProcessPage(this);
    m_stack->addWidget(m_processPage);
    auto *performancePage = new PerformancePage(this);
    m_stack->addWidget(performancePage);
    m_searchPage = new SearchPage(this);
    m_stack->addWidget(m_searchPage);
    m_settingsPage = new SettingsPage(this);
    m_stack->addWidget(m_settingsPage);
    m_monitoringClient = new MonitoringClient(this);
    m_setupLauncher = new SetupLauncher(this);
    m_processPage->attachMonitoringClient(m_monitoringClient);
    m_settingsState.startupOfferDismissed = QSettings().value(QStringLiteral("monitoring/startupOfferDismissed"), false).toBool();
    connect(m_monitoringClient, &MonitoringClient::serviceChanged, this, [this](const monitoring::ServiceInfo& info) {
        m_settingsState.service = info;
        refreshSettings();
        if (info.state == monitoring::ServiceState::Missing && !m_settingsState.startupOfferDismissed) {
            m_settingsState.startupOfferDismissed = true;
            QSettings().setValue(QStringLiteral("monitoring/startupOfferDismissed"), true);
            auto* offer = new QMessageBox(QMessageBox::Information, QStringLiteral("Диск и сеть по процессам"),
                QStringLiteral("Для этих показателей нужна служба мониторинга. Её можно установить в настройках. Основное приложение продолжит работать с обычными правами."),
                QMessageBox::Open | QMessageBox::Cancel, this);
            offer->setButtonText(QMessageBox::Open, QStringLiteral("Открыть настройки"));
            offer->setButtonText(QMessageBox::Cancel, QStringLiteral("Позже"));
            offer->setAttribute(Qt::WA_DeleteOnClose);
            connect(offer, &QMessageBox::finished, this, [this](int result) { if (result == QMessageBox::Open) setCurrentPage(3); });
            offer->open();
        }
    });
    connect(m_monitoringClient, &MonitoringClient::statusChanged, this, [this](const monitoring::Status& status) { m_settingsState.live = status; refreshSettings(); });
    connect(m_monitoringClient, &MonitoringClient::snapshotReady, this, [this](const monitoring::Snapshot& snapshot) { m_settingsState.diagnostics = snapshot.diagnostics; refreshSettings(); });
    connect(m_monitoringClient, &MonitoringClient::connectionChanged, this, [this](bool connected) { m_settingsState.connected = connected; refreshSettings(); });
    connect(m_monitoringClient, &MonitoringClient::connectionError, this, [this](const QString& error) { m_settingsState.connectionError = error; refreshSettings(); });
    connect(m_settingsPage, &SettingsPage::actionRequested, this, [this](monitoring::SetupRequest request) {
        m_settingsState.operationPending = true; refreshSettings(); m_setupLauncher->run(request);
    });
    connect(m_setupLauncher, &SetupLauncher::finished, this, [this](const monitoring::SetupResult& result) {
        m_settingsState.operationPending = false;
        m_settingsState.lastResult = QString::fromStdString(result.detail);
        if (result.win32Error) m_settingsState.lastResult += QStringLiteral(" (код %1)").arg(result.win32Error);
        refreshSettings();
    });
    m_monitoringClient->start();

    contentLayout->addWidget(m_stack, 1);
    rootLayout->addWidget(content, 1);

    setCentralWidget(central);

    connect(m_toggleTreeButton, &QPushButton::toggled, m_processPage, &ProcessPage::setTreeMode);
    connect(m_processPage, &ProcessPage::treeModeChanged, this, [this](bool enabled) {
        m_toggleTreeButton->setText(enabled ? QStringLiteral("Список") : QStringLiteral("Дерево"));
        m_toggleTreeButton->setChecked(enabled);
    });

    connect(m_searchEdit, &QLineEdit::textChanged, m_processPage, &ProcessPage::setFilterText);

    connect(m_processPage, &ProcessPage::snapshotApplied, m_searchPage, &SearchPage::setProcessSnapshot);
    connect(m_processPage, &ProcessPage::snapshotApplied, performancePage, &PerformancePage::setProcessSnapshot);
    connect(m_searchPage, &SearchPage::processActivated, this, [this](quint32 pid, quint64 creationTicks) {
        // A filter on the first tab could be hiding the very row we are going to.
        m_searchEdit->clear();
        setCurrentPage(0);
        m_processPage->revealProcess(pid, creationTicks);
    });

    auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+T")), this);
    connect(shortcut, &QShortcut::activated, this, [this] {
        if (m_stack->currentIndex() == 0) {
            m_toggleTreeButton->toggle();
        }
    });

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        if (m_stack->currentIndex() == 0) {
            m_searchEdit->setFocus(Qt::ShortcutFocusReason);
            m_searchEdit->selectAll();
        } else if (m_stack->currentIndex() == 2) {
            m_searchPage->focusInput();
        }
    });

    // A click on a surface that does nothing -- the window background, the
    // title, the empty part of the header or the sidebar -- is the user
    // pointing away from the process list, so the row they had picked is let
    // go. Buttons and the search box are deliberately not on this list: the
    // task buttons act on the selected row.
    const QList<QWidget *> surfaces{central, content, m_header, m_titleLabel,
                                    central->findChild<QFrame *>(QStringLiteral("Sidebar"))};
    for (QWidget *surface : surfaces) {
        surface->installEventFilter(this);
    }

    setCurrentPage(0);
    // A bare "QWidget" selector matches literally every widget in the process,
    // including the internal subwidgets of combo boxes, menus, scroll bars,
    // tooltips and the native dialogs -- the standard way to wreck the platform
    // style. Name the containers we actually want painted and target those.
    qApp->setStyleSheet(R"(
        QMainWindow,
        QWidget#CentralPanel,
        QWidget#ContentPanel,
        QWidget#HeaderPanel,
        QStackedWidget {
            background: #ffffff;
            color: #111111;
        }
        QFrame#Sidebar {
            background: #fbfbfb;
            border-right: 1px solid #e5e5e5;
        }
        QLabel#HeaderTitle {
            font-size: 15px;
            font-weight: 600;
            padding-left: 4px;
        }
        QWidget#HeaderPanel {
            border-bottom: 1px solid #e6e6e6;
        }
        QLineEdit#SearchEdit {
            background: #f4f5f7;
            border: 1px solid #e3e5e9;
            border-radius: 14px;
            padding: 5px 10px;
            color: #111111;
            selection-background-color: #d6e9ff;
            selection-color: #111111;
        }
        QLineEdit#SearchEdit:hover {
            background: #eff1f4;
        }
        QLineEdit#SearchEdit:focus {
            background: #ffffff;
            border-color: #a8c7fa;
        }
        QPushButton#HeaderButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 6px 12px;
        }
        QPushButton#HeaderButton:hover {
            background: #f2f2f2;
            border-color: #ececec;
        }
        QPushButton#HeaderButton:pressed,
        QPushButton#HeaderButton:checked {
            background: #e9f2ff;
            border-color: #bcd8ff;
        }
        QToolButton#SidebarButton {
            background: transparent;
            border: none;
            border-radius: 10px;
            padding: 0px;
            margin: 2px 4px;
        }
        QToolButton#SidebarButton:hover {
            background: #f0f0f0;
        }
        QToolButton#SidebarButton:checked {
            background: #efefef;
            border-left: 3px solid #1a73e8;
        }
    )");
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        m_processPage->clearCurrentRow();
    }

    return QMainWindow::eventFilter(watched, event);
}

QWidget *MainWindow::createSidebar()
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Sidebar"));
    frame->setFixedWidth(54);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignTop);

    m_sidebarGroup = new QButtonGroup(this);
    m_sidebarGroup->setExclusive(true);

    auto *details = createSidebarButton(QStringLiteral(":/icons/details.svg"), QStringLiteral("Сведения"), 0);
    auto *apps = createSidebarButton(QStringLiteral(":/icons/apps.svg"), QStringLiteral("Вкладка 2"), 1);
    auto *search = createSidebarButton(QStringLiteral(":/icons/search.svg"), QStringLiteral("Поиск"), 2);

    layout->addWidget(details);
    layout->addWidget(apps);
    layout->addWidget(search);
    layout->addStretch(1);

    auto* settings = createSidebarButton(QStringLiteral(":/icons/settings.svg"), QStringLiteral("Настройки"), 3);
    settings->setAccessibleName(QStringLiteral("Настройки"));
    layout->addWidget(settings);

    connect(m_sidebarGroup, &QButtonGroup::idClicked, this, &MainWindow::setCurrentPage);
    details->setChecked(true);

    return frame;
}

MainWindow::~MainWindow()
{
    m_monitoringClient->stop();
}

void MainWindow::refreshSettings()
{
    m_settingsPage->setState(m_settingsState);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_settingsState.operationPending) {
        setCurrentPage(3);
        m_settingsState.lastResult = QStringLiteral("Дождитесь установщика или отмените запрос UAC перед закрытием.");
        refreshSettings();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

QWidget *MainWindow::createHeader()
{
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("HeaderPanel"));

    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    m_titleLabel = new QLabel(QStringLiteral("Сведения"), header);
    m_titleLabel->setObjectName(QStringLiteral("HeaderTitle"));
    layout->addWidget(m_titleLabel);

    m_searchEdit = new QLineEdit(header);
    m_searchEdit->setObjectName(QStringLiteral("SearchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("Поиск по имени или PID"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->addAction(QIcon(QStringLiteral(":/icons/search.svg")),
                            QLineEdit::LeadingPosition);
    m_searchEdit->setFixedWidth(320);

    // Centred in the empty stretch between the title and the buttons.
    layout->addStretch(1);
    layout->addWidget(m_searchEdit);
    layout->addStretch(1);

    m_runButton = new QPushButton(QIcon(QStringLiteral(":/icons/run.svg")), QStringLiteral("Запустить новую задачу"), header);
    auto *killButton = new QPushButton(QIcon(QStringLiteral(":/icons/kill.svg")), QStringLiteral("Завершить задачу"), header);
    m_toggleTreeButton = new QPushButton(QStringLiteral("Дерево"), header);

    m_runButton->setObjectName(QStringLiteral("HeaderButton"));
    killButton->setObjectName(QStringLiteral("HeaderButton"));
    m_toggleTreeButton->setObjectName(QStringLiteral("HeaderButton"));
    m_toggleTreeButton->setCheckable(true);

    connect(m_runButton, &QPushButton::clicked, this, [this] {
        if (m_stack->currentIndex() == 0) {
            m_processPage->runNewTask();
        }
    });
    connect(killButton, &QPushButton::clicked, this, [this] {
        if (m_stack->currentIndex() == 0) {
            m_processPage->killSelectedProcess();
        } else if (m_stack->currentIndex() == 2) {
            m_searchPage->killSelectedProcess();
        }
    });

    layout->addWidget(m_runButton);
    layout->addWidget(killButton);
    layout->addWidget(m_toggleTreeButton);

    return header;
}

QToolButton *MainWindow::createSidebarButton(const QString &iconPath, const QString &tooltip, int index)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("SidebarButton"));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIcon(QIcon(iconPath));
    button->setIconSize(QSize(24, 24));
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFixedSize(44, 42);
    button->setToolTip(tooltip);

    m_sidebarGroup->addButton(button, index);
    return button;
}

void MainWindow::setCurrentPage(int index)
{
    m_stack->setCurrentIndex(index);
    // The sidebar is only told about clicks; a jump made from code (opening a
    // search result on the first tab) has to move its highlight itself.
    if (QAbstractButton *button = m_sidebarGroup->button(index)) {
        button->setChecked(true);
    }
    // The performance cards carry their own titles, and the header's title
    // and task buttons have nothing to act on there.
    m_header->setVisible(index != 1 && index != 3);

    switch (index) {
    case 0:
        m_titleLabel->setText(QStringLiteral("Сведения"));
        m_runButton->setVisible(true);
        m_toggleTreeButton->setVisible(true);
        m_searchEdit->setVisible(true);
        break;
    case 2:
        m_titleLabel->setText(QStringLiteral("Поиск"));
        m_runButton->setVisible(false);
        m_toggleTreeButton->setVisible(false);
        m_searchEdit->setVisible(false);
        m_searchPage->focusInput();
        break;
    default:
        break;
    }
}
