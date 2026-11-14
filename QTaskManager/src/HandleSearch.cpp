#include "HandleSearch.h"

#ifdef Q_OS_WIN

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <Windows.h>
#include <Psapi.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr NTSTATUS StatusBufferOverflow = static_cast<NTSTATUS>(0x80000005);
constexpr NTSTATUS StatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS StatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);

constexpr auto SystemExtendedHandleInformation = static_cast<SYSTEM_INFORMATION_CLASS>(64);
constexpr auto ObjectNameInformation = static_cast<OBJECT_INFORMATION_CLASS>(1);

// winternl.h only knows the legacy handle table, whose handle values are 16
// bit and whose process ids are 16 bit too.
struct HandleEntryEx
{
    PVOID object;
    ULONG_PTR processId;
    ULONG_PTR handleValue;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG attributes;
    ULONG reserved;
};

struct HandleInformationEx
{
    ULONG_PTR count;
    ULONG_PTR reserved;
    HandleEntryEx handles[1];
};

bool succeeded(NTSTATUS status)
{
    return status >= 0;
}

bool bufferTooSmall(NTSTATUS status)
{
    return status == StatusInfoLengthMismatch || status == StatusBufferOverflow
           || status == StatusBufferTooSmall;
}

// The name NtQueryObject reports; empty for an unnamed object. False when
// the query itself failed.
bool queryObjectName(HANDLE handle, QString &name)
{
    std::vector<BYTE> buffer(1024);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = NtQueryObject(handle, ObjectNameInformation, buffer.data(),
                                              static_cast<ULONG>(buffer.size()), &needed);
        if (succeeded(status)) {
            const auto *text = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
            name = text->Buffer ? QString::fromWCharArray(text->Buffer, text->Length / sizeof(wchar_t))
                                : QString();
            return true;
        }
        if (!bufferTooSmall(status)) {
            return false;
        }
        buffer.resize(std::max<size_t>(needed, buffer.size() * 2));
    }
    return false;
}

bool queryObjectTypeName(HANDLE handle, QString &name)
{
    std::vector<BYTE> buffer(1024);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = NtQueryObject(handle, ObjectTypeInformation, buffer.data(),
                                              static_cast<ULONG>(buffer.size()), &needed);
        if (succeeded(status)) {
            // TypeName is the first member of PUBLIC_OBJECT_TYPE_INFORMATION.
            const auto *text = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
            name = text->Buffer ? QString::fromWCharArray(text->Buffer, text->Length / sizeof(wchar_t))
                                : QString();
            return true;
        }
        if (!bufferTooSmall(status)) {
            return false;
        }
        buffer.resize(std::max<size_t>(needed, buffer.size() * 2));
    }
    return false;
}

// NtQueryObject(ObjectNameInformation) on a file object takes the object's
// lock when it was opened for synchronous I/O. If another thread is parked in
// a synchronous read on that same file object -- the usual case is the server
// end of a named pipe -- the query never returns, and neither does
// GetFileType or any other query on that object. Process Explorer and
// handle.exe both run into this; the cure is to ask from a thread that can be
// given up on.
//
// So file names are fetched by a helper thread. If it does not answer in
// time it is first poked with CancelSynchronousIo, which frees it in the
// cases where the wait is cancellable, and abandoned (it exits on its own if
// the wait ever ends) when it is not.
class GuardedNameQuery
{
public:
    enum class Result { Name, Failed, Hung };

    ~GuardedNameQuery() { retire(); }

    // Takes ownership of `handle` and closes it.
    Result query(HANDLE handle, QString &name)
    {
        ensureWorker();

        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->request = handle;
            m_shared->hasRequest = true;
            m_shared->done = false;
        }
        m_shared->cv.notify_all();

        std::unique_lock lock(m_shared->mutex);
        if (m_shared->cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_shared->done; })) {
            name = std::move(m_shared->name);
            return m_shared->ok ? Result::Name : Result::Failed;
        }

        lock.unlock();
        CancelSynchronousIo(m_thread.native_handle());
        lock.lock();
        if (m_shared->cv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_shared->done; })) {
            // The cancel got it moving again, but the object is still one
            // that traps queries; the caller remembers that.
            return Result::Hung;
        }

        m_shared->quit = true;
        lock.unlock();
        m_thread.detach();
        m_shared.reset();
        ++m_abandoned;
        return Result::Hung;
    }

    int abandoned() const { return m_abandoned; }

private:
    struct Shared
    {
        std::mutex mutex;
        std::condition_variable cv;
        HANDLE request = nullptr;
        bool hasRequest = false;
        bool done = false;
        bool ok = false;
        bool quit = false;
        QString name;
    };

    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
    int m_abandoned = 0;

    static void workerMain(std::shared_ptr<Shared> shared)
    {
        std::unique_lock lock(shared->mutex);
        for (;;) {
            shared->cv.wait(lock, [&] { return shared->hasRequest || shared->quit; });
            if (shared->quit) {
                if (shared->hasRequest) {
                    CloseHandle(shared->request);
                }
                return;
            }

            const HANDLE handle = shared->request;
            shared->hasRequest = false;
            lock.unlock();

            QString name;
            const bool ok = queryObjectName(handle, name);
            CloseHandle(handle);

            lock.lock();
            shared->ok = ok;
            shared->name = std::move(name);
            shared->done = true;
            shared->cv.notify_all();
        }
    }

    void ensureWorker()
    {
        if (m_shared) {
            return;
        }
        m_shared = std::make_shared<Shared>();
        m_thread = std::thread(workerMain, m_shared);
    }

    void retire()
    {
        if (!m_shared) {
            return;
        }
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->quit = true;
        }
        m_shared->cv.notify_all();
        m_thread.join();
        m_shared.reset();
    }
};

// What kind of handles carry a name worth matching, and how to read it.
enum class Kind { Skip, Plain, File, Process };

Kind kindForType(const QString &type)
{
    static const QSet<QString> plain = {
        QStringLiteral("Key"),         QStringLiteral("Section"),   QStringLiteral("Mutant"),
        QStringLiteral("Event"),       QStringLiteral("Semaphore"), QStringLiteral("Directory"),
        QStringLiteral("Timer"),       QStringLiteral("Job"),       QStringLiteral("Desktop"),
        QStringLiteral("WindowStation"), QStringLiteral("SymbolicLink"), QStringLiteral("ALPC Port"),
        QStringLiteral("Session"),     QStringLiteral("Partition"), QStringLiteral("KeyedEvent"),
        QStringLiteral("Callback"),
    };

    if (type == QLatin1String("File")) {
        return Kind::File;
    }
    if (type == QLatin1String("Process")) {
        return Kind::Process;
    }
    return plain.contains(type) ? Kind::Plain : Kind::Skip;
}

// "\Device\HarddiskVolume3\dir\x.txt" -> "C:\dir\x.txt", and the registry's
// NT spellings to the HKLM / HKU ones regedit shows.
class NameBeautifier
{
public:
    NameBeautifier()
    {
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
            const wchar_t drive[3] = {letter, L':', L'\0'};
            wchar_t target[1024];
            if (QueryDosDeviceW(drive, target, static_cast<DWORD>(std::size(target))) == 0) {
                continue;
            }
            // A multi-string; the first entry is the live mapping.
            m_drives.append({QString::fromWCharArray(target), QString::fromWCharArray(drive)});
        }
    }

    QString file(const QString &nt) const
    {
        for (const auto &[device, drive] : m_drives) {
            if (nt.startsWith(device, Qt::CaseInsensitive)
                && (nt.size() == device.size() || nt.at(device.size()) == QLatin1Char('\\'))) {
                return drive + nt.mid(device.size());
            }
        }
        if (nt.startsWith(QLatin1String("\\Device\\Mup\\"), Qt::CaseInsensitive)) {
            return QStringLiteral("\\\\") + nt.mid(12);
        }
        return nt;
    }

    QString key(const QString &nt) const
    {
        if (nt.startsWith(QLatin1String("\\REGISTRY\\MACHINE"), Qt::CaseInsensitive)) {
            return QStringLiteral("HKLM") + nt.mid(17);
        }
        if (nt.startsWith(QLatin1String("\\REGISTRY\\USER"), Qt::CaseInsensitive)) {
            return QStringLiteral("HKU") + nt.mid(14);
        }
        return nt;
    }

private:
    QVector<QPair<QString, QString>> m_drives;
};

struct ProcessInfo
{
    QString name;
    QString path;
    quint64 creationTimeTicks = 0;
};

ProcessInfo queryProcessInfo(DWORD pid)
{
    ProcessInfo info;
    if (pid == 0) {
        info.name = QStringLiteral("System Idle Process");
        return info;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[MAX_PATH * 4];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            info.path = QString::fromWCharArray(path, static_cast<int>(size));
            info.name = info.path.mid(info.path.lastIndexOf(QLatin1Char('\\')) + 1);
        }

        FILETIME creation{}, exitTime{}, kernel{}, user{};
        if (GetProcessTimes(process, &creation, &exitTime, &kernel, &user)) {
            ULARGE_INTEGER ticks{};
            ticks.LowPart = creation.dwLowDateTime;
            ticks.HighPart = creation.dwHighDateTime;
            info.creationTimeTicks = ticks.QuadPart;
        }
        CloseHandle(process);
    }

    if (info.name.isEmpty()) {
        info.name = pid == 4 ? QStringLiteral("System") : QStringLiteral("PID %1").arg(pid);
    }
    return info;
}

// Turns raw finds into batched matches, and knows when to stop.
class Collector
{
public:
    Collector(const QString &needle, const HandleSearch::BatchSink &sink)
        : m_needle(needle), m_sink(sink)
    {
        m_sinceFlush.start();
    }

    bool full() const { return m_count >= HandleSearch::MaxMatches; }
    bool truncated() const { return m_truncated; }

    bool matches(const QString &text) const
    {
        return text.contains(m_needle, Qt::CaseInsensitive);
    }

    void add(DWORD pid, const QString &type, quint64 handle, const QString &name)
    {
        if (full()) {
            m_truncated = true;
            return;
        }

        auto cached = m_processes.find(pid);
        if (cached == m_processes.end()) {
            cached = m_processes.insert(pid, queryProcessInfo(pid));
        }

        HandleMatch match;
        match.pid = pid;
        match.creationTimeTicks = cached->creationTimeTicks;
        match.processName = cached->name;
        match.imagePath = cached->path;
        match.type = type;
        match.handle = handle;
        match.name = name;
        m_batch.append(std::move(match));
        ++m_count;

        if (m_sinceFlush.elapsed() > 150) {
            flush();
        }
    }

    void flush()
    {
        if (!m_batch.isEmpty()) {
            m_sink(std::move(m_batch));
            m_batch.clear();
        }
        m_sinceFlush.restart();
    }

    QString nameOfProcess(DWORD pid)
    {
        auto cached = m_processes.find(pid);
        if (cached == m_processes.end()) {
            cached = m_processes.insert(pid, queryProcessInfo(pid));
        }
        return cached->name;
    }

private:
    QString m_needle;
    const HandleSearch::BatchSink &m_sink;
    QVector<HandleMatch> m_batch;
    QHash<DWORD, ProcessInfo> m_processes;
    QElapsedTimer m_sinceFlush;
    int m_count = 0;
    bool m_truncated = false;
};

void searchModules(Collector &collector, const std::atomic_bool &cancel, QSet<DWORD> &inaccessible)
{
    std::vector<DWORD> pids(2048);
    DWORD bytes = 0;
    for (;;) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes)) {
            return;
        }
        if (bytes < pids.size() * sizeof(DWORD)) {
            break;
        }
        pids.resize(pids.size() * 2);
    }
    pids.resize(bytes / sizeof(DWORD));

    // Not CreateToolhelp32Snapshot(TH32CS_SNAPMODULE): that one plants a thread
    // in every target to read its loader list. Reading the PEB from here is
    // enough and leaves the processes alone.
    std::vector<HMODULE> modules(1024);
    for (const DWORD pid : pids) {
        if (cancel.load(std::memory_order_relaxed) || collector.full()) {
            return;
        }
        if (pid == 0 || pid == 4) {
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process) {
            inaccessible.insert(pid);
            continue;
        }

        DWORD needed = 0;
        bool listed = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!EnumProcessModulesEx(process, modules.data(),
                                      static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                      &needed, LIST_MODULES_ALL)) {
                break;
            }
            if (needed <= modules.size() * sizeof(HMODULE)) {
                listed = true;
                break;
            }
            modules.resize(needed / sizeof(HMODULE) + 64);
        }

        if (!listed) {
            inaccessible.insert(pid);
            CloseHandle(process);
            continue;
        }

        const size_t count = needed / sizeof(HMODULE);
        for (size_t i = 0; i < count && !collector.full(); ++i) {
            wchar_t path[MAX_PATH * 4];
            const DWORD length = GetModuleFileNameExW(process, modules[i], path, static_cast<DWORD>(std::size(path)));
            if (length == 0) {
                continue;
            }

            const QString text = QString::fromWCharArray(path, static_cast<int>(length));
            if (collector.matches(text)) {
                collector.add(pid, QStringLiteral("DLL"), 0, text);
            }
        }
        CloseHandle(process);
    }
}

bool snapshotHandles(std::vector<BYTE> &buffer)
{
    buffer.resize(16 * 1024 * 1024);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(),
                                                         static_cast<ULONG>(buffer.size()), &needed);
        if (succeeded(status)) {
            return true;
        }
        if (status != StatusInfoLengthMismatch) {
            return false;
        }
        // The table keeps growing while we ask; leave room for that.
        buffer.resize(std::max<size_t>(needed + 4 * 1024 * 1024, buffer.size() * 2));
    }
    return false;
}

void searchHandles(Collector &collector, const std::atomic_bool &cancel, QSet<DWORD> &inaccessible)
{
    std::vector<BYTE> buffer;
    if (!snapshotHandles(buffer)) {
        return;
    }

    const auto *table = reinterpret_cast<const HandleInformationEx *>(buffer.data());

    // One owner at a time, so each process is opened once.
    QHash<DWORD, QVector<const HandleEntryEx *>> byOwner;
    for (ULONG_PTR i = 0; i < table->count; ++i) {
        const HandleEntryEx &entry = table->handles[i];
        byOwner[static_cast<DWORD>(entry.processId)].append(&entry);
    }

    const NameBeautifier beautifier;
    GuardedNameQuery guarded;
    QHash<USHORT, Kind> kindByTypeIndex;
    QHash<USHORT, QString> typeNameByIndex;
    // File objects whose query hung. Every other handle to the same object
    // would hang just the same, so only the first one costs a timeout.
    QSet<PVOID> hungObjects;
    constexpr int MaxAbandonedThreads = 64;

    const HANDLE self = GetCurrentProcess();

    for (auto owner = byOwner.cbegin(); owner != byOwner.cend(); ++owner) {
        if (cancel.load(std::memory_order_relaxed) || collector.full()) {
            return;
        }

        const DWORD pid = owner.key();
        HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!process) {
            inaccessible.insert(pid);
            continue;
        }

        for (const HandleEntryEx *entry : owner.value()) {
            if (cancel.load(std::memory_order_relaxed) || collector.full()) {
                break;
            }

            const HANDLE source = reinterpret_cast<HANDLE>(entry->handleValue);

            auto kind = kindByTypeIndex.constFind(entry->objectTypeIndex);
            if (kind == kindByTypeIndex.cend()) {
                // Types are told apart by their index, which is the same for
                // every handle in the system, so ask once per index.
                Kind learned = Kind::Skip;
                QString typeName;
                HANDLE copy = nullptr;
                if (DuplicateHandle(process, source, self, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    if (queryObjectTypeName(copy, typeName)) {
                        learned = kindForType(typeName);
                    }
                    CloseHandle(copy);
                } else {
                    // A handle we cannot copy says nothing about its type;
                    // try again with the next one of that index.
                    continue;
                }
                typeNameByIndex.insert(entry->objectTypeIndex, typeName);
                kind = kindByTypeIndex.insert(entry->objectTypeIndex, learned);
            }

            if (kind.value() == Kind::Skip) {
                continue;
            }

            QString raw;
            QString shown;
            if (kind.value() == Kind::Process) {
                HANDLE copy = nullptr;
                if (!DuplicateHandle(process, source, self, &copy, PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
                    continue;
                }
                const DWORD targetPid = GetProcessId(copy);
                CloseHandle(copy);
                if (targetPid == 0) {
                    continue;
                }
                raw = shown = QStringLiteral("%1 (%2)").arg(collector.nameOfProcess(targetPid)).arg(targetPid);
            } else {
                if (kind.value() == Kind::File
                    && (hungObjects.contains(entry->object) || guarded.abandoned() >= MaxAbandonedThreads)) {
                    continue;
                }

                HANDLE copy = nullptr;
                if (!DuplicateHandle(process, source, self, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    continue;
                }

                if (kind.value() == Kind::File) {
                    // Closes `copy` itself.
                    const auto result = guarded.query(copy, raw);
                    if (result == GuardedNameQuery::Result::Hung) {
                        hungObjects.insert(entry->object);
                    }
                    if (result != GuardedNameQuery::Result::Name) {
                        continue;
                    }
                    shown = beautifier.file(raw);
                } else {
                    const bool ok = queryObjectName(copy, raw);
                    CloseHandle(copy);
                    if (!ok) {
                        continue;
                    }
                    shown = typeNameByIndex.value(entry->objectTypeIndex) == QLatin1String("Key")
                                ? beautifier.key(raw)
                                : raw;
                }
            }

            if (shown.isEmpty()) {
                continue;
            }

            if (collector.matches(shown) || (raw != shown && collector.matches(raw))) {
                collector.add(pid, typeNameByIndex.value(entry->objectTypeIndex), entry->handleValue, shown);
            }
        }
        CloseHandle(process);
    }
}
} // namespace

namespace HandleSearch
{
HandleSearchStats run(const QString &needle, const std::atomic_bool &cancel, const BatchSink &sink)
{
    HandleSearchStats stats;
    if (needle.isEmpty()) {
        return stats;
    }

    Collector collector(needle, sink);
    QSet<DWORD> inaccessible;

    // Modules first: they take a moment, and their hits fill the list while
    // the much longer sweep over handles is still running.
    searchModules(collector, cancel, inaccessible);
    searchHandles(collector, cancel, inaccessible);
    collector.flush();

    stats.truncated = collector.truncated();
    // Idle and System are never readable; do not count them against the user.
    inaccessible.remove(0);
    inaccessible.remove(4);
    stats.inaccessibleProcesses = static_cast<int>(inaccessible.size());
    return stats;
}
} // namespace HandleSearch

#else

namespace HandleSearch
{
HandleSearchStats run(const QString &, const std::atomic_bool &, const BatchSink &)
{
    return {};
}
} // namespace HandleSearch

#endif
