#pragma once

#include "ProcessEntry.h"

#include <QHash>
#include <QVector>
// Q_OS_WIN gates everything below, so pull in the header that defines it
// instead of relying on QHash to drag qglobal.h along: reorder the includes
// and the whole provider would silently compile away to nothing.
#include <QtGlobal>

#include <vector>

#ifdef Q_OS_WIN

// What the provider remembers about a process from one snapshot to the next.
struct KnownProcess
{
    quint64 cpuTotalTicks = 0;

    // Read once, when the process is first seen. None of them changes for the
    // life of a process -- except the working directory, which is why the
    // copy action reads that one afresh (ProcessProviderWin::currentDirectory).
    QString executablePath;
    QString commandLine;
    QString currentDirectory;
};

class ProcessProviderWin
{
public:
    QVector<ProcessEntry> snapshot();

    // The working directory of a live process, read now. Empty when the
    // process is gone, its PID belongs to another process by now, or it
    // refuses to be read.
    static QString currentDirectory(quint32 pid, quint64 creationTimeTicks);

private:
    // Keyed like the rows: PID and creation time, since PIDs get recycled.
    QHash<quint64, KnownProcess> m_known;
    // When the previous snapshot was taken (100 ns, FILETIME scale).
    quint64 m_sampleTimeTicks = 0;
    // The system process list, kept between snapshots so that a megabyte or so
    // is not reallocated every second.
    std::vector<unsigned char> m_buffer;
};

#endif
