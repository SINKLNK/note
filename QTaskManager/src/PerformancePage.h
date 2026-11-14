#pragma once

#include "AudioDevices.h"
#include "PerformanceMonitor.h"
#include "ProcessEntry.h"

#include <QThread>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <memory>

class QScrollArea;
class PerformanceBoard;

// The "Производительность" tab: CPU, memory, GPU and audio cards laid out like
// the Windows 11 Task Manager.
//
// The board's minimum width follows the window height. The large charts share
// whatever height is left after the fixed rows (captions, numbers, small
// charts) and each keeps its own width-to-height ratio, so the board gets
// wider as the window gets taller. When that is wider than the window a
// horizontal scroll bar appears; when the window is wider, the cards stretch
// sideways to fill it and the charts give up their ratio.
//
// CPU, memory and GPU readings come from a PerformanceMonitor on a worker
// thread and keep accumulating while the tab is hidden, so the charts already
// hold history when it is opened. Audio is polled on the GUI thread, and only
// while the tab is visible.
class PerformancePage : public QWidget
{
    Q_OBJECT
public:
    explicit PerformancePage(QWidget *parent = nullptr);
    ~PerformancePage() override;

public slots:
    // The process list of the first tab. The RAM the compressed store occupies
    // is the working set of the Memory Compression process, and the list has
    // it already; reading it here spares the monitor a second walk over every
    // process in the system each second.
    void setProcessSnapshot(const QVector<ProcessEntry> &entries);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QScrollArea *m_scroll = nullptr;
    PerformanceBoard *m_board = nullptr;

    QThread m_workerThread;
    PerformanceMonitor *m_monitor = nullptr;
    SystemInfo m_info;
    quint64 m_compressedBytes = 0;
    bool m_compressedKnown = false;

    std::unique_ptr<AudioDevices> m_audio;
    QTimer m_audioTimer;
    QVector<AudioDevices::Device> m_outputs;
    QVector<AudioDevices::Device> m_inputs;
    QString m_defaultOutput;
    QString m_defaultInput;

    void updateBoardLayout();
    void applySystemInfo(const SystemInfo &info);
    void applySample(const PerformanceSample &sample);
    void refreshAudio();
};
