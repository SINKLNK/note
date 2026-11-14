#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

// One process holding something that matched: an open handle, or a loaded
// module (type "DLL", no handle).
struct HandleMatch
{
    quint32 pid = 0;
    quint64 creationTimeTicks = 0; // 0 when the process could not be queried.
    QString processName;
    QString imagePath;
    QString type;       // Object type ("File", "Key", "Mutant", ...) or "DLL".
    quint64 handle = 0; // Handle value in the owner; 0 for modules.
    QString name;       // Object name or module path.
};

struct HandleSearchStats
{
    int inaccessibleProcesses = 0; // Processes we were not allowed to look into.
    bool truncated = false;        // Stopped at MaxMatches.
};

namespace HandleSearch
{
constexpr int MaxMatches = 10000;

using BatchSink = std::function<void(QVector<HandleMatch> &&)>;

// Case-insensitive substring search over the names of every open handle in
// the system and the paths of every loaded module -- what Process Explorer's
// "Find Handle or DLL" does. Blocking; meant for a worker thread. Matches go
// to `sink` in batches as they are found. `cancel` is polled throughout.
HandleSearchStats run(const QString &needle, const std::atomic_bool &cancel, const BatchSink &sink);
} // namespace HandleSearch
