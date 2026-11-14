#pragma once

#include "ProcessEntry.h"
#include "ProcessProviderWin.h"

#include <QObject>
#include <QTimer>
#include <QVector>
#include "MonitoringClient.h"
#include <optional>

class ProcessMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ProcessMonitor(QObject *parent = nullptr);

public slots:
    void start();
    void stop();
    void poll();
    void setServiceSnapshot(const monitoring::Snapshot& snapshot);
    void clearServiceSnapshot();

signals:
    void snapshotReady(const QVector<ProcessEntry> &entries);

private:
    QTimer *m_timer = nullptr;
    std::optional<monitoring::Snapshot> m_serviceSnapshot;
#ifdef Q_OS_WIN
    ProcessProviderWin m_provider;
#endif
};
