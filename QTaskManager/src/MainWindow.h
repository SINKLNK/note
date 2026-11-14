#pragma once

#include <QMainWindow>
#include "SettingsPage.h"

class QButtonGroup;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class ProcessPage;
class SearchPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QStackedWidget *m_stack = nullptr;
    QWidget *m_header = nullptr;
    QLabel *m_titleLabel = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_toggleTreeButton = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    ProcessPage *m_processPage = nullptr;
    SearchPage *m_searchPage = nullptr;
    QButtonGroup *m_sidebarGroup = nullptr;
    SettingsPage* m_settingsPage = nullptr;
    MonitoringClient* m_monitoringClient = nullptr;
    SetupLauncher* m_setupLauncher = nullptr;
    SettingsState m_settingsState;
    void refreshSettings();

    QWidget *createSidebar();
    QWidget *createHeader();
    QToolButton *createSidebarButton(const QString &iconPath, const QString &tooltip, int index);
    void setCurrentPage(int index);
};
