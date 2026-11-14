#pragma once

#include "FileIconCache.h"
#include "HandleSearch.h"
#include "ProcessEntry.h"

#include <QHash>
#include <QPair>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;
class QThread;
class QTimer;
class QTreeWidget;

// "Find Handle or DLL": which processes hold an open handle or a loaded module
// whose name contains the typed text.
class SearchPage : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPage(QWidget *parent = nullptr);
    ~SearchPage() override;

    void focusInput();

public slots:
    // The process list of the first tab; adds the command line to results.
    void setProcessSnapshot(const QVector<ProcessEntry> &entries);
    void killSelectedProcess();

signals:
    // A result was double-clicked (or Enter was pressed on it).
    void processActivated(quint32 pid, quint64 creationTimeTicks);

private:
    using ProcessKey = QPair<quint32, quint64>;

    QLineEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_tree = nullptr;

    // Typing restarts this; the search starts when it runs out, so a word
    // typed over a couple of seconds costs one sweep and not one per letter.
    QTimer *m_debounce = nullptr;

    // At most one sweep thread exists. Starting a search while one is still
    // winding down cancels it and waits for its `finished`; results of a sweep
    // that has been superseded carry an old generation and are dropped.
    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    quint64 m_generation = 0;
    bool m_startPending = false;
    bool m_running = false;
    int m_resultCount = 0;

    QHash<ProcessKey, QString> m_commandLines;
    FileIconCache m_icons;

    void onTextChanged();
    void startSearch();
    void cancelRunning();
    void addMatches(quint64 generation, const QVector<HandleMatch> &matches);
    void finishSearch(quint64 generation, const HandleSearchStats &stats);
    void threadFinished();
    void clearResults();
    void updateStatus(const QString &suffix = {});
};
