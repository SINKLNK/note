#include "ProcessMonitor.h"
#include "ServiceSnapshotMerge.h"

ProcessMonitor::ProcessMonitor(QObject *parent)
    : QObject(parent),
      m_timer(new QTimer(this))
{
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &ProcessMonitor::poll);
}

void ProcessMonitor::start()
{
    if (!m_timer->isActive()) {
        m_timer->start();
    }
    poll();
}

void ProcessMonitor::stop()
{
    m_timer->stop();
}

void ProcessMonitor::poll()
{
#ifdef Q_OS_WIN
    auto rows = m_provider.snapshot();
    mergeServiceSnapshot(rows, m_serviceSnapshot ? &*m_serviceSnapshot : nullptr,
                         monitoring::qpcNow(), monitoring::qpcFrequency());
    emit snapshotReady(rows);
#else
    emit snapshotReady({});
#endif
}

void ProcessMonitor::setServiceSnapshot(const monitoring::Snapshot& snapshot) { m_serviceSnapshot = snapshot; }
void ProcessMonitor::clearServiceSnapshot() { m_serviceSnapshot.reset(); }
