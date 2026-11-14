#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

struct ProcessEntry
{
    enum class IoQuality {Unavailable, Complete, Partial};
    quint32 pid = 0;
    quint32 parentPid = 0;
    quint64 creationTimeTicks = 0;

    QString name;
    QString userName;
    QString commandLine;
    QString executablePath;
    QString currentDirectory; // Working directory when the process was first seen; empty when it refuses to be read.
    QString windowTitle; // Captions of the process's visible top-level windows, one per line; empty when it has none.

    double cpuPercent = 0.0;
    quint64 residentRamBytes = 0; // Full resident working set: private + shared resident pages.
    quint64 committedRamBytes = 0; // Process commit charge: PrivateUsage / Commit size.
    double diskRateMBps = -1.0; // ETW FileIo requested read+write MB/s, including cache; -1 means unavailable.
    double networkRateMBps = -1.0; // Accurate per-process networking needs ETW; -1 means unavailable.
    IoQuality diskQuality = IoQuality::Unavailable;
    IoQuality networkQuality = IoQuality::Unavailable;
    quint64 dedicatedGpuMemoryBytes = 0;
    quint64 sharedGpuMemoryBytes = 0;

    // Lets the model tell an unchanged row from a changed one and emit
    // dataChanged only for the rows that actually moved.
    bool operator==(const ProcessEntry &other) const = default;
};

Q_DECLARE_METATYPE(ProcessEntry)
Q_DECLARE_METATYPE(QVector<ProcessEntry>)
