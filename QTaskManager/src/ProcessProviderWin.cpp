#include "ProcessProviderWin.h"

#ifdef Q_OS_WIN

#include <Windows.h>
#include <winternl.h>
#include <sddl.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cstddef>
#include <vector>
#include <QPair>

namespace
{
struct UnicodeStringRemote
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct RtlUserProcessParametersRemote
{
    BYTE Reserved1[16];
    PVOID Reserved2[5]; // ConsoleHandle, ConsoleFlags, StandardInput/Output/Error.
    UnicodeStringRemote CurrentDirectory; // CURDIR.DosPath; its Handle follows.
    PVOID Reserved3[3]; // CURDIR.Handle and DllPath.
    UnicodeStringRemote ImagePathName;
    UnicodeStringRemote CommandLine;
};

struct PebRemote
{
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    RtlUserProcessParametersRemote* ProcessParameters;
};

QString fromWideBuffer(const wchar_t *buffer, DWORD size)
{
    if (!buffer || size == 0) {
        return {};
    }
    return QString::fromWCharArray(buffer, static_cast<int>(size));
}

quint64 fileTimeToUInt64(const FILETIME &ft)
{
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// Task Manager and Process Explorer both do this on startup: without
// SeDebugPrivilege a number of processes stay closed to us even under an
// elevated token, and their path, memory and CPU come back empty.
bool enableDebugPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    bool enabled = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) {
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
        enabled = GetLastError() == ERROR_SUCCESS;
    }

    CloseHandle(token);
    return enabled;
}

QString queryExecutablePath(HANDLE process)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        return fromWideBuffer(buffer.data(), size);
    }
    return {};
}

// Not implemented yet: nothing calls this, and ProcessEntry::userName stays
// empty until the user column lands. Kept because the lookup itself is done.
QString queryUserName(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return {};
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return {};
    }

    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &needed)) {
        CloseHandle(token);
        return {};
    }

    auto *tokenUser = reinterpret_cast<TOKEN_USER *>(buffer.data());
    DWORD nameLen = 0;
    DWORD domainLen = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, tokenUser->User.Sid, nullptr, &nameLen, nullptr, &domainLen, &use);

    std::vector<wchar_t> name(nameLen + 1, L'\0');
    std::vector<wchar_t> domain(domainLen + 1, L'\0');
    QString result;

    if (LookupAccountSidW(nullptr,
                          tokenUser->User.Sid,
                          name.data(),
                          &nameLen,
                          domain.data(),
                          &domainLen,
                          &use)) {
        if (domainLen > 0) {
            result = QString::fromWCharArray(domain.data(), static_cast<int>(domainLen))
                     + QLatin1Char('\\')
                     + QString::fromWCharArray(name.data(), static_cast<int>(nameLen));
        } else {
            result = QString::fromWCharArray(name.data(), static_cast<int>(nameLen));
        }
    }

    CloseHandle(token);
    return result;
}

QString readRemoteUnicodeString(HANDLE process, const UnicodeStringRemote &remoteString)
{
    if (remoteString.Length == 0 || !remoteString.Buffer) {
        return {};
    }

    std::vector<wchar_t> buffer(remoteString.Length / sizeof(wchar_t) + 1, L'\0');
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, remoteString.Buffer, buffer.data(), remoteString.Length, &read) || read == 0) {
        return {};
    }

    return QString::fromWCharArray(buffer.data(), static_cast<int>(read / sizeof(wchar_t)));
}

// The command line and the working directory sit in the same parameter block,
// so one walk through the PEB serves both.
void queryProcessParameters(HANDLE process, QString &commandLine, QString &currentDirectory)
{
    PROCESS_BASIC_INFORMATION pbi{};
    ULONG returned = 0;
    const NTSTATUS status = NtQueryInformationProcess(process,
                                                      ProcessBasicInformation,
                                                      &pbi,
                                                      sizeof(pbi),
                                                      &returned);
    if (status != 0 || !pbi.PebBaseAddress) {
        return;
    }

    PebRemote peb{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &read) || read != sizeof(peb)) {
        return;
    }

    if (!peb.ProcessParameters) {
        return;
    }

    RtlUserProcessParametersRemote params{};
    if (!ReadProcessMemory(process, peb.ProcessParameters, &params, sizeof(params), &read)
        || read != sizeof(params)) {
        return;
    }

    commandLine = readRemoteUnicodeString(process, params.CommandLine);
    currentDirectory = readRemoteUnicodeString(process, params.CurrentDirectory);
}

DWORD logicalProcessorCount()
{
    static const DWORD count = [] {
        const DWORD value = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return value == 0 ? 1u : value;
    }();
    return count;
}

quint64 preciseNowTicks()
{
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return fileTimeToUInt64(now);
}

// SYSTEM_PROCESS_INFORMATION as the kernel lays it out. winternl.h hides the
// parent PID and the times behind Reserved fields; the asserts below pin this
// layout to its public one.
struct SystemProcessInfo
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    // I/O counters and the per-thread array follow.
};
static_assert(offsetof(SystemProcessInfo, ImageName) == offsetof(SYSTEM_PROCESS_INFORMATION, ImageName));
static_assert(offsetof(SystemProcessInfo, UniqueProcessId) == offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId));
static_assert(offsetof(SystemProcessInfo, InheritedFromUniqueProcessId) == offsetof(SYSTEM_PROCESS_INFORMATION, Reserved2));
static_assert(offsetof(SystemProcessInfo, WorkingSetSize) == offsetof(SYSTEM_PROCESS_INFORMATION, WorkingSetSize));
static_assert(offsetof(SystemProcessInfo, PrivatePageCount) == offsetof(SYSTEM_PROCESS_INFORMATION, PrivatePageCount));

// Every process in the system in one call, without opening any of them --
// so protected processes and those of other users report their times and
// memory too.
bool querySystemProcesses(std::vector<unsigned char> &buffer)
{
    constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
    if (buffer.empty()) {
        buffer.resize(1024 * 1024);
    }

    ULONG needed = 0;
    NTSTATUS status;
    while ((status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(),
                                              static_cast<ULONG>(buffer.size()), &needed))
           == kInfoLengthMismatch) {
        // Processes can start between the two calls; leave some headroom.
        buffer.resize(std::max<size_t>(needed + 64 * 1024, buffer.size() * 2));
    }
    return NT_SUCCESS(status);
}

quint64 creationTimeOf(HANDLE process)
{
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exitTime, &kernel, &user)) {
        return 0;
    }
    return fileTimeToUInt64(creation);
}

// Opens the process behind a snapshot row. The PID came from a list taken a
// moment ago and may belong to another process by now, so the creation time
// has to match as well.
HANDLE openProcess(quint32 pid, quint64 creationTimeTicks, bool &canReadMemory)
{
    // The command line lives in the remote PEB, so reading it needs
    // PROCESS_VM_READ -- a right refused far more often than
    // PROCESS_QUERY_LIMITED_INFORMATION (protected processes, other users,
    // anything running as SYSTEM). Asking for both in one handle means a
    // refusal costs us the path as well, so fall back to the cheaper right
    // and just skip the PEB read.
    canReadMemory = true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        canReadMemory = false;
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (process && creationTimeOf(process) != creationTimeTicks) {
        CloseHandle(process);
        process = nullptr;
    }
    return process;
}

// Path, command line and working directory, once per process. Reading them
// used to take an OpenProcess, a path lookup that converts the NT device path
// to a drive letter, and four reads of the remote PEB for every process every
// second: close to half of what the monitor thread spent, for values that do
// not change.
void readProcessStrings(quint32 pid, quint64 creationTimeTicks, KnownProcess &known)
{
    bool canReadMemory = false;
    const HANDLE process = openProcess(pid, creationTimeTicks, canReadMemory);
    if (!process) {
        return;
    }

    known.executablePath = queryExecutablePath(process);
    if (canReadMemory) {
        queryProcessParameters(process, known.commandLine, known.currentDirectory);
    }
    CloseHandle(process);
}

using GpuMemory = QPair<quint64, quint64>; // dedicated, shared
using GpuMemoryByPid = QHash<quint32, GpuMemory>;

quint32 pidFromGpuInstanceName(const wchar_t *name)
{
    if (!name) {
        return 0;
    }

    const QString instance = QString::fromWCharArray(name);
    if (!instance.startsWith(QStringLiteral("pid_"), Qt::CaseInsensitive)) {
        return 0;
    }

    const int end = instance.indexOf(QLatin1Char('_'), 4);
    bool ok = false;
    const quint32 pid = instance.mid(4, end < 0 ? -1 : end - 4).toUInt(&ok);
    return ok ? pid : 0;
}

void accumulateGpuCounter(PDH_HCOUNTER counter, bool dedicated, GpuMemoryByPid &result)
{
    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter,
                                                     PDH_FMT_LARGE,
                                                     &bufferSize,
                                                     &itemCount,
                                                     nullptr);
    if (status != PDH_MORE_DATA || bufferSize == 0) {
        return;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto *items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter,
                                          PDH_FMT_LARGE,
                                          &bufferSize,
                                          &itemCount,
                                          items);
    if (status != ERROR_SUCCESS) {
        return;
    }

    for (DWORD i = 0; i < itemCount; ++i) {
        const DWORD counterStatus = items[i].FmtValue.CStatus;
        if (counterStatus != PDH_CSTATUS_VALID_DATA && counterStatus != PDH_CSTATUS_NEW_DATA) {
            continue;
        }

        const quint32 pid = pidFromGpuInstanceName(items[i].szName);
        const LONGLONG value = items[i].FmtValue.largeValue;
        if (pid == 0 || value <= 0) {
            continue;
        }

        GpuMemory &memory = result[pid];
        if (dedicated) {
            memory.first += static_cast<quint64>(value);
        } else {
            memory.second += static_cast<quint64>(value);
        }
    }
}

// Opening the query and expanding the "(*)" instance wildcard is the expensive
// part of reading these counters, and it used to happen on every tick, once a
// second. Open once and keep the handles; only the collect needs repeating.
// The query is deliberately never closed -- it lives as long as the process,
// and snapshot() only ever runs on the single monitor thread.
GpuMemoryByPid queryGpuMemory()
{
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER dedicated = nullptr;
    static PDH_HCOUNTER shared = nullptr;

    static const bool ready = [] {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return false;
        }

        // "Local Usage", not "Dedicated Usage": the latter counts address space
        // a process merely reserved (85 GB for a Chrome utility process), while
        // Local Usage is what is resident in VRAM -- the figure Task Manager shows.
        if (PdhAddEnglishCounterW(query,
                                  L"\\GPU Process Memory(*)\\Local Usage",
                                  0,
                                  &dedicated) != ERROR_SUCCESS) {
            dedicated = nullptr;
        }
        if (PdhAddEnglishCounterW(query,
                                  L"\\GPU Process Memory(*)\\Shared Usage",
                                  0,
                                  &shared) != ERROR_SUCCESS) {
            shared = nullptr;
        }

        return dedicated != nullptr || shared != nullptr;
    }();

    GpuMemoryByPid result;
    if (!ready || PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return result;
    }

    if (dedicated) {
        accumulateGpuCounter(dedicated, true, result);
    }
    if (shared) {
        accumulateGpuCounter(shared, false, result);
    }

    return result;
}

// One pass over the top-level windows per snapshot instead of one per process.
// Only visible, unowned windows with a caption count: owned windows are
// dialogs and popups of some other main window, and invisible ones are the
// message-only helpers nearly every GUI process creates. EnumWindows walks in
// z-order, so the titles of a process come out topmost first. A process can
// own several such windows (every VS Code window belongs to its one main
// process), so all of them are kept, one per line.
BOOL CALLBACK collectWindowTitle(HWND window, LPARAM param)
{
    auto *titles = reinterpret_cast<QHash<quint32, QString> *>(param);

    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return TRUE;
    }

    wchar_t buffer[512];
    const int length = GetWindowTextW(window, buffer, static_cast<int>(std::size(buffer)));
    if (length > 0) {
        QString &joined = (*titles)[pid];
        if (!joined.isEmpty()) {
            joined += QLatin1Char('\n');
        }
        joined += QString::fromWCharArray(buffer, length);
    }
    return TRUE;
}

QHash<quint32, QString> queryWindowTitles()
{
    QHash<quint32, QString> titles;
    EnumWindows(collectWindowTitle, reinterpret_cast<LPARAM>(&titles));
    return titles;
}

} // namespace

QVector<ProcessEntry> ProcessProviderWin::snapshot()
{
    static const bool debugPrivilege = enableDebugPrivilege();
    Q_UNUSED(debugPrivilege);

    QVector<ProcessEntry> result;
    if (!querySystemProcesses(m_buffer)) {
        return result;
    }
    // One timestamp serves every process: the kernel fills in all of their
    // times within the one call above. Back when each process was opened and
    // read in turn, its times were read some way into a walk whose length
    // changed from tick to tick, and the interval had to be measured per
    // process -- with one shared timestamp, the cl.exe children of one
    // dotnet.exe added up to 157% of the machine under a full build.
    const quint64 sampleTimeTicks = preciseNowTicks();
    const quint64 elapsedTicks = sampleTimeTicks > m_sampleTimeTicks ? sampleTimeTicks - m_sampleTimeTicks : 0;

    const GpuMemoryByPid gpuMemory = queryGpuMemory();
    const QHash<quint32, QString> windowTitles = queryWindowTitles();

    QHash<quint64, KnownProcess> known;
    known.reserve(m_known.size());

    for (const unsigned char *at = m_buffer.data();;) {
        const auto *info = reinterpret_cast<const SystemProcessInfo *>(at);

        ProcessEntry entry;
        entry.pid = static_cast<quint32>(reinterpret_cast<quintptr>(info->UniqueProcessId));
        entry.parentPid = static_cast<quint32>(reinterpret_cast<quintptr>(info->InheritedFromUniqueProcessId));
        entry.creationTimeTicks = static_cast<quint64>(info->CreateTime.QuadPart);
        // The idle process comes without a name; Toolhelp, which this list
        // used to come from, calls it this.
        entry.name = entry.pid == 0
                         ? QStringLiteral("[System Process]")
                         : QString::fromWCharArray(info->ImageName.Buffer,
                                                   info->ImageName.Length / sizeof(wchar_t));
        // WorkingSetSize is the full resident set: the process's private
        // resident pages plus its share of resident shared pages. Subtracting
        // the private working set from it, as this once did, left only the
        // shared half. PrivatePageCount is the commit charge, what
        // GetProcessMemoryInfo calls PrivateUsage.
        entry.residentRamBytes = info->WorkingSetSize;
        entry.committedRamBytes = info->PrivatePageCount;

        const quint64 key = (static_cast<quint64>(entry.pid) << 32) ^ entry.creationTimeTicks;
        const quint64 cpuTotalTicks = static_cast<quint64>(info->KernelTime.QuadPart)
                                      + static_cast<quint64>(info->UserTime.QuadPart);

        KnownProcess process;
        const auto previous = m_known.constFind(key);
        if (previous != m_known.cend()) {
            process = previous.value();
            // CPU is per core, top-style: one core kept busy for the whole
            // interval is 100%, two spinning threads are 200%, the ceiling is
            // 100% x logical CPUs. The idle process is left out: its "CPU
            // time" is the idle time of every core.
            if (entry.pid != 0 && elapsedTicks > 0 && cpuTotalTicks >= process.cpuTotalTicks) {
                entry.cpuPercent = static_cast<double>(cpuTotalTicks - process.cpuTotalTicks)
                                   / static_cast<double>(elapsedTicks) * 100.0;
                // Thread times are charged at clock-tick granularity, so a short
                // window can still overshoot slightly; never past every core busy.
                entry.cpuPercent = std::clamp(entry.cpuPercent,
                                              0.0,
                                              100.0 * static_cast<double>(logicalProcessorCount()));
            }
        } else {
            readProcessStrings(entry.pid, entry.creationTimeTicks, process);
        }
        process.cpuTotalTicks = cpuTotalTicks;

        entry.executablePath = process.executablePath.isEmpty() ? entry.name : process.executablePath;
        entry.commandLine = process.commandLine.isEmpty() ? entry.executablePath : process.commandLine;
        entry.currentDirectory = process.currentDirectory;
        entry.windowTitle = windowTitles.value(entry.pid);

        const auto gpuIt = gpuMemory.constFind(entry.pid);
        if (gpuIt != gpuMemory.cend()) {
            entry.dedicatedGpuMemoryBytes = gpuIt.value().first;
            entry.sharedGpuMemoryBytes = gpuIt.value().second;
        }

        known.insert(key, std::move(process));
        result.push_back(std::move(entry));

        if (info->NextEntryOffset == 0) {
            break;
        }
        at += info->NextEntryOffset;
    }

    // Only the processes of this snapshot carry over; the ones that are gone
    // drop out with the old table.
    m_known.swap(known);
    m_sampleTimeTicks = sampleTimeTicks;

    // No sorting here: ProcessModel re-sorts every rebuild by whichever column
    // the header is currently set to, so ordering the snapshot by name was a
    // full case-insensitive sort of several hundred rows thrown away per tick.
    return result;
}

QString ProcessProviderWin::currentDirectory(quint32 pid, quint64 creationTimeTicks)
{
    bool canReadMemory = false;
    const HANDLE process = openProcess(pid, creationTimeTicks, canReadMemory);
    if (!process) {
        return {};
    }

    QString commandLine;
    QString directory;
    if (canReadMemory) {
        queryProcessParameters(process, commandLine, directory);
    }
    CloseHandle(process);
    return directory;
}

#endif
