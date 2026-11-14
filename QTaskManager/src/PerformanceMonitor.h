#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QTimer;

// What the machine is, read once at start-up.
struct SystemInfo
{
    QString cpuName;
    int logicalProcessors = 0;

    quint64 installedMemory = 0; // What the DIMMs add up to, before firmware reservations.
    QString memoryType;          // "DDR4" and the like, from SMBIOS; empty when it does not say.

    // The hardware adapter with the most dedicated memory. Empty name when the
    // machine only has the software renderer.
    QString gpuName;
    quint64 gpuDedicatedTotal = 0;
    quint64 gpuSharedTotal = 0;
    bool gpuTemperatureAvailable = false;
};

// One tick's worth of readings. Percentages are 0..100, sizes are bytes.
struct PerformanceSample
{
    // Task Manager's figures: "% Processor Utility", which accounts for the
    // clock running above or below base, not plain busy time.
    QVector<double> coreUsage;
    QVector<double> coreKernel; // The privileged share of coreUsage.
    double cpuUsage = 0.0;
    double cpuSpeedMHz = 0.0;
    quint32 processes = 0;
    quint32 threads = 0;
    quint32 handles = 0;
    quint64 uptimeMs = 0;

    quint64 physicalTotal = 0;
    quint64 physicalAvailable = 0;
    quint64 commitTotal = 0;
    quint64 commitLimit = 0;
    quint64 modified = 0;
    quint64 standby = 0;
    quint64 free = 0;
    quint64 pagedPool = 0;
    quint64 nonPagedPool = 0;

    double gpuUsage = 0.0; // The busiest engine, as Task Manager reports it.
    double gpu3d = 0.0;
    double gpuVideoDecode = 0.0;
    quint64 gpuDedicatedUsed = 0;
    quint64 gpuSharedUsed = 0;
    double gpuTemperature = -1.0; // °C; negative when the driver does not report it.
};

Q_DECLARE_METATYPE(SystemInfo)
Q_DECLARE_METATYPE(PerformanceSample)

// Polls the system once a second on whatever thread it is moved to. start()
// reads the static description first and announces it, then samples follow.
class PerformanceMonitor : public QObject
{
    Q_OBJECT
public:
    explicit PerformanceMonitor(QObject *parent = nullptr);
    ~PerformanceMonitor() override;

public slots:
    void start();

signals:
    void systemInfoReady(const SystemInfo &info);
    void sampleReady(const PerformanceSample &sample);

private:
    struct Sources;
    std::unique_ptr<Sources> m_sources;
    QTimer *m_timer = nullptr;

    void poll();
};
