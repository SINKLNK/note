#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace
{
using Clock = std::chrono::steady_clock;

constexpr wchar_t kSessionName[] = L"EtwIoProbeKernel";

// Private system-logger session GUID. Any stable private GUID is fine on Windows 8+.
constexpr GUID kSessionGuid =
{ 0xa64c9830, 0xaaf2, 0x4bf2, { 0x93, 0xd6, 0x55, 0x72, 0x3d, 0x20, 0xc7, 0x4e } };

// Documented NT Kernel Logger provider GUIDs.
constexpr GUID kDiskIoGuid =
{ 0x3d6fa8d4, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
constexpr GUID kThreadGuid =
{ 0x3d6fa8d1, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
constexpr GUID kTcpIpGuid =
{ 0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2 } };
constexpr GUID kUdpIpGuid =
{ 0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80 } };

constexpr UCHAR kOpStart = 1;
constexpr UCHAR kOpEnd = 2;
constexpr UCHAR kOpDcStart = 3;
constexpr UCHAR kOpDcEnd = 4;
constexpr UCHAR kOpSendV4 = 10;
constexpr UCHAR kOpReceiveV4 = 11;
constexpr UCHAR kOpDiskRead = 10;
constexpr UCHAR kOpDiskWrite = 11;
constexpr UCHAR kOpSendV6 = 26;
constexpr UCHAR kOpReceiveV6 = 27;

bool GuidEqual(const GUID& a, const GUID& b)
{
    return !!InlineIsEqualGUID(a, b);
}

std::string Win32ErrorMessage(DWORD error)
{
    if (error == ERROR_SUCCESS)
        return "ERROR_SUCCESS";

    char* buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);

    std::string result;
    if (size && buffer)
    {
        result.assign(buffer, size);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
            result.pop_back();
        LocalFree(buffer);
    }
    else
    {
        result = "Win32 error " + std::to_string(error);
    }
    return result;
}

std::string Utf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};

    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

uint64_t MiB(uint64_t n)
{
    return n * 1024ull * 1024ull;
}

double ToMiB(uint64_t bytes)
{
    return static_cast<double>(bytes) / 1024.0 / 1024.0;
}

struct IoTotals
{
    uint64_t diskRead = 0;
    uint64_t diskWrite = 0;
    uint64_t tcpSend = 0;
    uint64_t tcpReceive = 0;
    uint64_t udpSend = 0;
    uint64_t udpReceive = 0;

    uint64_t DiskBytes() const { return diskRead + diskWrite; }
    uint64_t NetworkBytes() const { return tcpSend + tcpReceive + udpSend + udpReceive; }
};

IoTotals operator-(const IoTotals& a, const IoTotals& b)
{
    IoTotals r;
    r.diskRead = a.diskRead >= b.diskRead ? a.diskRead - b.diskRead : 0;
    r.diskWrite = a.diskWrite >= b.diskWrite ? a.diskWrite - b.diskWrite : 0;
    r.tcpSend = a.tcpSend >= b.tcpSend ? a.tcpSend - b.tcpSend : 0;
    r.tcpReceive = a.tcpReceive >= b.tcpReceive ? a.tcpReceive - b.tcpReceive : 0;
    r.udpSend = a.udpSend >= b.udpSend ? a.udpSend - b.udpSend : 0;
    r.udpReceive = a.udpReceive >= b.udpReceive ? a.udpReceive - b.udpReceive : 0;
    return r;
}

struct TraceStats
{
    uint64_t diskEvents = 0;
    uint64_t tcpEvents = 0;
    uint64_t udpEvents = 0;
    uint64_t threadEvents = 0;
    uint64_t tidMapHits = 0;
    uint64_t tidFallbackHits = 0;
    uint64_t tidUnresolved = 0;
    uint64_t eventsLost = 0;
    uint64_t logBuffersLost = 0;
    uint64_t realtimeBuffersLost = 0;
    int diskSchemaState = 0;
    int tcpSchemaState = 0;
    int udpSchemaState = 0;
    DWORD diskSchemaError = ERROR_SUCCESS;
    DWORD tcpSchemaError = ERROR_SUCCESS;
    DWORD udpSchemaError = ERROR_SUCCESS;
};

std::unordered_map<DWORD, std::string> SnapshotProcessNames()
{
    std::unordered_map<DWORD, std::string> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe))
    {
        do
        {
            result[pe.th32ProcessID] = Utf8(pe.szExeFile);
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return result;
}

class EtwIoMonitor
{
public:
    ~EtwIoMonitor() { Stop(); }

    bool Start()
    {
        if (started_)
            return true;

        if (!IsElevated())
        {
            startError_ = ERROR_ACCESS_DENIED;
            return false;
        }

        SeedThreadMap();
        StopStaleSession();

        auto properties = MakeProperties();
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());

        TRACEHANDLE session = 0;
        const ULONG status = StartTraceW(&session, kSessionName, p);
        if (status != ERROR_SUCCESS)
        {
            startError_ = status;
            return false;
        }

        sessionHandle_ = session;
        started_ = true;
        stopping_.store(false, std::memory_order_release);
        consumerThread_ = std::thread([this]() { ConsumerMain(); });

        std::unique_lock lock(readyMutex_);
        readyCv_.wait_for(lock, std::chrono::seconds(5), [this]() { return consumerReady_; });
        if (!consumerReady_ || consumerOpenError_ != ERROR_SUCCESS)
        {
            const DWORD error = consumerOpenError_ ? consumerOpenError_ : ERROR_TIMEOUT;
            lock.unlock();
            Stop();
            startError_ = error;
            return false;
        }

        return true;
    }

    void Stop()
    {
        if (!started_ && !consumerThread_.joinable())
            return;

        stopping_.store(true, std::memory_order_release);

        if (sessionHandle_ != 0)
        {
            UpdateSessionLossCounters();
            auto properties = MakeProperties();
            auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());
            ControlTraceW(sessionHandle_, nullptr, p, EVENT_TRACE_CONTROL_STOP);
        }

        if (consumerThread_.joinable())
            consumerThread_.join();

        sessionHandle_ = 0;
        started_ = false;
    }

    void Flush()
    {
        if (!started_ || sessionHandle_ == 0)
            return;
        auto properties = MakeProperties();
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());
        ControlTraceW(sessionHandle_, nullptr, p, EVENT_TRACE_CONTROL_FLUSH);
    }

    DWORD StartError() const { return startError_; }
    DWORD ConsumerOpenError() const { return consumerOpenError_; }
    ULONG ProcessTraceResult() const { return processTraceResult_; }

    IoTotals GetPid(DWORD pid) const
    {
        std::lock_guard lock(dataMutex_);
        const auto it = byPid_.find(pid);
        return it != byPid_.end() ? it->second : IoTotals{};
    }

    std::unordered_map<DWORD, IoTotals> SnapshotAll() const
    {
        std::lock_guard lock(dataMutex_);
        return byPid_;
    }

    TraceStats GetStats()
    {
        UpdateSessionLossCounters();
        TraceStats result;
        result.diskEvents = diskEvents_.load(std::memory_order_relaxed);
        result.tcpEvents = tcpEvents_.load(std::memory_order_relaxed);
        result.udpEvents = udpEvents_.load(std::memory_order_relaxed);
        result.threadEvents = threadEvents_.load(std::memory_order_relaxed);
        result.tidMapHits = tidMapHits_.load(std::memory_order_relaxed);
        result.tidFallbackHits = tidFallbackHits_.load(std::memory_order_relaxed);
        result.tidUnresolved = tidUnresolved_.load(std::memory_order_relaxed);
        result.eventsLost = eventsLost_.load(std::memory_order_relaxed);
        result.logBuffersLost = logBuffersLost_.load(std::memory_order_relaxed);
        result.realtimeBuffersLost = realtimeBuffersLost_.load(std::memory_order_relaxed);
        result.diskSchemaState = diskSchemaState_.load(std::memory_order_relaxed);
        result.tcpSchemaState = tcpSchemaState_.load(std::memory_order_relaxed);
        result.udpSchemaState = udpSchemaState_.load(std::memory_order_relaxed);
        result.diskSchemaError = diskSchemaError_.load(std::memory_order_relaxed);
        result.tcpSchemaError = tcpSchemaError_.load(std::memory_order_relaxed);
        result.udpSchemaError = udpSchemaError_.load(std::memory_order_relaxed);
        return result;
    }

    size_t SeededThreadCount() const { return seededThreadCount_; }

private:
    mutable std::mutex dataMutex_;
    std::unordered_map<DWORD, IoTotals> byPid_;
    std::unordered_map<DWORD, DWORD> threadToProcess_;

    std::atomic<uint64_t> diskEvents_{0};
    std::atomic<uint64_t> tcpEvents_{0};
    std::atomic<uint64_t> udpEvents_{0};
    std::atomic<uint64_t> threadEvents_{0};
    std::atomic<uint64_t> tidMapHits_{0};
    std::atomic<uint64_t> tidFallbackHits_{0};
    std::atomic<uint64_t> tidUnresolved_{0};
    std::atomic<uint64_t> eventsLost_{0};
    std::atomic<uint64_t> logBuffersLost_{0};
    std::atomic<uint64_t> realtimeBuffersLost_{0};
    // Schema check states: 0=pending, 1=checking, 2=pass, 3=TDH unavailable, 4=mismatch.
    std::atomic<int> diskSchemaState_{0};
    std::atomic<int> tcpSchemaState_{0};
    std::atomic<int> udpSchemaState_{0};
    std::atomic<DWORD> diskSchemaError_{ERROR_SUCCESS};
    std::atomic<DWORD> tcpSchemaError_{ERROR_SUCCESS};
    std::atomic<DWORD> udpSchemaError_{ERROR_SUCCESS};

    TRACEHANDLE sessionHandle_ = 0;
    TRACEHANDLE consumerHandle_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumerThread_;
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    DWORD startError_ = ERROR_SUCCESS;
    DWORD consumerOpenError_ = ERROR_SUCCESS;
    ULONG processTraceResult_ = ERROR_SUCCESS;
    size_t seededThreadCount_ = 0;

    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool consumerReady_ = false;

    static void AtomicMax(std::atomic<uint64_t>& target, uint64_t value)
    {
        uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {
        }
    }

    std::vector<unsigned char> MakeProperties() const
    {
        const size_t nameBytes = sizeof(kSessionName);
        std::vector<unsigned char> buffer(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes, 0);
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
        p->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
        p->Wnode.Guid = kSessionGuid;
        p->Wnode.ClientContext = 1; // QPC clock.
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->BufferSize = 64;         // KiB per ETW buffer.
        p->MinimumBuffers = 4;
        p->MaximumBuffers = 64;
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->FlushTimer = 1;
        p->EnableFlags = EVENT_TRACE_FLAG_DISK_IO |
                         EVENT_TRACE_FLAG_NETWORK_TCPIP |
                         EVENT_TRACE_FLAG_THREAD;
        p->LogFileNameOffset = 0;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        std::memcpy(buffer.data() + p->LoggerNameOffset, kSessionName, nameBytes);
        return buffer;
    }

    void StopStaleSession()
    {
        auto properties = MakeProperties();
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());
        ControlTraceW(0, kSessionName, p, EVENT_TRACE_CONTROL_STOP);
    }

    void SeedThreadMap()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;

        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        size_t count = 0;
        if (Thread32First(snapshot, &te))
        {
            std::lock_guard lock(dataMutex_);
            do
            {
                threadToProcess_[te.th32ThreadID] = te.th32OwnerProcessID;
                ++count;
            } while (Thread32Next(snapshot, &te));
        }
        seededThreadCount_ = count;
        CloseHandle(snapshot);
    }

    void ConsumerMain()
    {
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = const_cast<LPWSTR>(kSessionName);
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME |
                               PROCESS_TRACE_MODE_EVENT_RECORD |
                               PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        log.EventRecordCallback = &EtwIoMonitor::StaticEventRecordCallback;
        log.BufferCallback = &EtwIoMonitor::StaticBufferCallback;
        log.Context = this;

        consumerHandle_ = OpenTraceW(&log);
        {
            std::lock_guard lock(readyMutex_);
            if (consumerHandle_ == INVALID_PROCESSTRACE_HANDLE)
                consumerOpenError_ = GetLastError();
            else
                consumerOpenError_ = ERROR_SUCCESS;
            consumerReady_ = true;
        }
        readyCv_.notify_all();

        if (consumerHandle_ == INVALID_PROCESSTRACE_HANDLE)
            return;

        processTraceResult_ = ProcessTrace(&consumerHandle_, 1, nullptr, nullptr);
        CloseTrace(consumerHandle_);
        consumerHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    static ULONG WINAPI StaticBufferCallback(PEVENT_TRACE_LOGFILEW log)
    {
        auto* self = static_cast<EtwIoMonitor*>(log->Context);
        return self && !self->stopping_.load(std::memory_order_acquire) ? TRUE : FALSE;
    }

    static void WINAPI StaticEventRecordCallback(PEVENT_RECORD eventRecord)
    {
        auto* self = static_cast<EtwIoMonitor*>(eventRecord->UserContext);
        if (self)
            self->OnEvent(eventRecord);
    }

    static uint32_t ReadU32(const unsigned char* data)
    {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    static bool TdhReadU32(PEVENT_RECORD record, const wchar_t* propertyName,
                           uint32_t& value, DWORD& error)
    {
        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyName);
        descriptor.ArrayIndex = ULONG_MAX;

        ULONG size = 0;
        TDHSTATUS status = TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size);
        if (status != ERROR_SUCCESS)
        {
            error = status;
            return false;
        }
        if (size != sizeof(uint32_t))
        {
            error = ERROR_BAD_LENGTH;
            return false;
        }

        status = TdhGetProperty(record, 0, nullptr, 1, &descriptor,
                                sizeof(value), reinterpret_cast<PBYTE>(&value));
        if (status != ERROR_SUCCESS)
        {
            error = status;
            return false;
        }
        error = ERROR_SUCCESS;
        return true;
    }

    static void CheckPairWithTdh(PEVENT_RECORD record,
                                 const wchar_t* firstName, uint32_t firstRaw,
                                 const wchar_t* secondName, uint32_t secondRaw,
                                 std::atomic<int>& state,
                                 std::atomic<DWORD>& errorOut)
    {
        int expected = 0;
        if (!state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            return;

        uint32_t first = 0;
        uint32_t second = 0;
        DWORD error = ERROR_SUCCESS;
        if (!TdhReadU32(record, firstName, first, error) ||
            !TdhReadU32(record, secondName, second, error))
        {
            errorOut.store(error, std::memory_order_relaxed);
            state.store(3, std::memory_order_release);
            return;
        }

        if (first == firstRaw && second == secondRaw)
        {
            state.store(2, std::memory_order_release);
        }
        else
        {
            errorOut.store(ERROR_INVALID_DATA, std::memory_order_relaxed);
            state.store(4, std::memory_order_release);
        }
    }

    DWORD ResolveThreadToProcess(DWORD tid)
    {
        {
            std::lock_guard lock(dataMutex_);
            const auto it = threadToProcess_.find(tid);
            if (it != threadToProcess_.end())
            {
                tidMapHits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }

        // Safety fallback for a thread that started before our map saw its event, or died quickly.
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (thread)
        {
            const DWORD pid = GetProcessIdOfThread(thread);
            CloseHandle(thread);
            if (pid != 0)
            {
                {
                    std::lock_guard lock(dataMutex_);
                    threadToProcess_[tid] = pid;
                }
                tidFallbackHits_.fetch_add(1, std::memory_order_relaxed);
                return pid;
            }
        }

        tidUnresolved_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    void OnEvent(PEVENT_RECORD record)
    {
        if (stopping_.load(std::memory_order_relaxed))
            return;

        const auto opcode = record->EventHeader.EventDescriptor.Opcode;
        const auto* data = static_cast<const unsigned char*>(record->UserData);
        const size_t length = record->UserDataLength;

        if (GuidEqual(record->EventHeader.ProviderId, kDiskIoGuid))
        {
            if (opcode != kOpDiskRead && opcode != kOpDiskWrite)
                return;

            // DiskIo_TypeGroup1 V3: TransferSize is always field #3 at offset 8.
            // On Windows 8+ IssuingThreadId is field #9. Pointer-sized fields before it
            // make its raw offset 48 bytes for 64-bit kernel events and 40 for 32-bit.
            const bool event32 = (record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0;
            const size_t issuingTidOffset = event32 ? 40u : 48u;
            if (length < issuingTidOffset + sizeof(uint32_t) || length < 12)
                return;

            const uint32_t transferSize = ReadU32(data + 8);
            const uint32_t issuingTid = ReadU32(data + issuingTidOffset);
            CheckPairWithTdh(record, L"TransferSize", transferSize,
                             L"IssuingThreadId", issuingTid,
                             diskSchemaState_, diskSchemaError_);
            DWORD pid = 0;
            if (issuingTid == 0xffffffffu)
                pid = 4; // System.
            else
                pid = ResolveThreadToProcess(issuingTid);

            diskEvents_.fetch_add(1, std::memory_order_relaxed);
            if (pid != 0)
            {
                std::lock_guard lock(dataMutex_);
                auto& totals = byPid_[pid];
                if (opcode == kOpDiskRead)
                    totals.diskRead += transferSize;
                else
                    totals.diskWrite += transferSize;
            }
            return;
        }

        if (GuidEqual(record->EventHeader.ProviderId, kTcpIpGuid))
        {
            if (opcode != kOpSendV4 && opcode != kOpReceiveV4 &&
                opcode != kOpSendV6 && opcode != kOpReceiveV6)
                return;
            if (length < 8)
                return;

            // TcpIp send/receive MOF classes begin with uint32 PID; uint32 size.
            const DWORD pid = ReadU32(data);
            const uint32_t size = ReadU32(data + 4);
            CheckPairWithTdh(record, L"PID", pid, L"size", size,
                             tcpSchemaState_, tcpSchemaError_);
            tcpEvents_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(dataMutex_);
            auto& totals = byPid_[pid];
            if (opcode == kOpSendV4 || opcode == kOpSendV6)
                totals.tcpSend += size;
            else
                totals.tcpReceive += size;
            return;
        }

        if (GuidEqual(record->EventHeader.ProviderId, kUdpIpGuid))
        {
            if (opcode != kOpSendV4 && opcode != kOpReceiveV4 &&
                opcode != kOpSendV6 && opcode != kOpReceiveV6)
                return;
            if (length < 8)
                return;

            // UdpIp send/receive MOF classes also begin with uint32 PID; uint32 size.
            const DWORD pid = ReadU32(data);
            const uint32_t size = ReadU32(data + 4);
            CheckPairWithTdh(record, L"PID", pid, L"size", size,
                             udpSchemaState_, udpSchemaError_);
            udpEvents_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(dataMutex_);
            auto& totals = byPid_[pid];
            if (opcode == kOpSendV4 || opcode == kOpSendV6)
                totals.udpSend += size;
            else
                totals.udpReceive += size;
            return;
        }

        if (GuidEqual(record->EventHeader.ProviderId, kThreadGuid))
        {
            if (opcode != kOpStart && opcode != kOpEnd &&
                opcode != kOpDcStart && opcode != kOpDcEnd)
                return;
            if (length < 8)
                return;

            DWORD pid = 0;
            DWORD tid = 0;
            if (record->EventHeader.EventDescriptor.Version == 0)
            {
                // Thread_V0_TypeGroup1: TThreadId, ProcessId.
                tid = ReadU32(data);
                pid = ReadU32(data + 4);
            }
            else
            {
                // Thread V1+ TypeGroup1: ProcessId, TThreadId.
                pid = ReadU32(data);
                tid = ReadU32(data + 4);
            }

            threadEvents_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(dataMutex_);
            if (opcode == kOpStart || opcode == kOpDcStart)
                threadToProcess_[tid] = pid;
            else
            {
                const auto it = threadToProcess_.find(tid);
                if (it != threadToProcess_.end() && it->second == pid)
                    threadToProcess_.erase(it);
            }
        }
    }

    void UpdateSessionLossCounters()
    {
        if (!started_ || sessionHandle_ == 0)
            return;

        auto properties = MakeProperties();
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());
        const ULONG status = ControlTraceW(sessionHandle_, nullptr, p, EVENT_TRACE_CONTROL_QUERY);
        if (status == ERROR_SUCCESS)
        {
            AtomicMax(eventsLost_, p->EventsLost);
            AtomicMax(logBuffersLost_, p->LogBuffersLost);
            AtomicMax(realtimeBuffersLost_, p->RealTimeBuffersLost);
        }
    }
};


struct DiskTestResult
{
    bool ok = false;
    uint64_t requestedBytes = 0;
    uint64_t writtenBytes = 0;
    double seconds = 0.0;
    DWORD error = ERROR_SUCCESS;
    std::wstring path;
};

DiskTestResult RunDiskTest(uint64_t bytesToWrite)
{
    DiskTestResult result;
    result.requestedBytes = bytesToWrite;

    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
    {
        result.error = GetLastError();
        return result;
    }

    std::wstringstream path;
    path << tempPath << L"EtwIoProbe-" << GetCurrentProcessId() << L".bin";
    result.path = path.str();

    HANDLE file = CreateFileW(result.path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY |
                                  FILE_FLAG_NO_BUFFERING |
                                  FILE_FLAG_WRITE_THROUGH |
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                  FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        result.error = GetLastError();
        return result;
    }

    constexpr DWORD blockSize = 4u * 1024u * 1024u;
    void* memory = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory)
    {
        result.error = GetLastError();
        CloseHandle(file);
        return result;
    }

    // Pseudo-random-ish data avoids testing a storage stack that might compress an all-zero stream.
    auto* words = static_cast<uint32_t*>(memory);
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < blockSize / sizeof(uint32_t); ++i)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        words[i] = x;
    }

    const auto start = Clock::now();
    uint64_t remaining = bytesToWrite;
    while (remaining > 0)
    {
        // Keep all no-buffering transfers sector-friendly.
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(remaining, blockSize));
        DWORD written = 0;
        if (!WriteFile(file, memory, chunk, &written, nullptr))
        {
            result.error = GetLastError();
            break;
        }
        if (written == 0)
        {
            result.error = ERROR_WRITE_FAULT;
            break;
        }
        result.writtenBytes += written;
        remaining -= written;
    }

    FlushFileBuffers(file);
    const auto end = Clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.ok = (result.error == ERROR_SUCCESS && result.writtenBytes == bytesToWrite);

    VirtualFree(memory, 0, MEM_RELEASE);
    CloseHandle(file);
    return result;
}

struct NetworkTestResult
{
    bool ok = false;
    uint64_t bytesReceived = 0;
    double seconds = 0.0;
    DWORD error = ERROR_SUCCESS;
    DWORD httpStatus = 0;
    std::wstring url;
};

NetworkTestResult RunHttpDownload(const std::wstring& url)
{
    NetworkTestResult result;
    result.url = url;

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        result.error = GetLastError();
        return result;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path;
    if (parts.lpszUrlPath && parts.dwUrlPathLength)
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";

    HINTERNET session = WinHttpOpen(L"EtwIoProbe/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session)
    {
        result.error = GetLastError();
        return result;
    }

    WinHttpSetTimeouts(session, 10000, 10000, 10000, 30000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection)
    {
        result.error = GetLastError();
        WinHttpCloseHandle(session);
        return result;
    }

    const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection,
                                           L"GET",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags);
    if (!request)
    {
        result.error = GetLastError();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    const auto start = Clock::now();
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        result.error = GetLastError();
    }
    else
    {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &status,
                                &statusSize,
                                WINHTTP_NO_HEADER_INDEX))
        {
            result.httpStatus = status;
        }

        std::vector<unsigned char> buffer(128 * 1024);
        while (result.error == ERROR_SUCCESS)
        {
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            {
                result.error = GetLastError();
                break;
            }
            if (read == 0)
                break;
            result.bytesReceived += read;
        }
    }

    const auto end = Clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.ok = result.error == ERROR_SUCCESS &&
                result.httpStatus >= 200 && result.httpStatus < 300 &&
                result.bytesReceived > 0;

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

struct UdpLoopbackResult
{
    bool ok = false;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    double seconds = 0.0;
    int error = 0;
};

UdpLoopbackResult RunUdpLoopback(uint64_t targetBytes)
{
    UdpLoopbackResult result;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        result.error = WSAGetLastError();
        return result;
    }

    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server == INVALID_SOCKET || client == INVALID_SOCKET)
    {
        result.error = WSAGetLastError();
        if (server != INVALID_SOCKET) closesocket(server);
        if (client != INVALID_SOCKET) closesocket(client);
        WSACleanup();
        return result;
    }

    int bufferSize = 4 * 1024 * 1024;
    setsockopt(server, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
    setsockopt(client, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        result.error = WSAGetLastError();
        closesocket(server);
        closesocket(client);
        WSACleanup();
        return result;
    }

    int addrLen = sizeof(address);
    if (getsockname(server, reinterpret_cast<sockaddr*>(&address), &addrLen) == SOCKET_ERROR)
    {
        result.error = WSAGetLastError();
        closesocket(server);
        closesocket(client);
        WSACleanup();
        return result;
    }

    DWORD timeoutMs = 250;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    std::atomic<uint64_t> received{0};
    std::atomic<bool> senderDone{false};
    std::thread receiver([&]()
    {
        std::vector<char> buffer(2048);
        while (!senderDone.load(std::memory_order_acquire) || received.load() < targetBytes)
        {
            const int n = recvfrom(server, buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
            if (n > 0)
            {
                received.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
                if (received.load(std::memory_order_relaxed) >= targetBytes)
                    break;
            }
            else
            {
                const int error = WSAGetLastError();
                if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK)
                    break;
                if (senderDone.load(std::memory_order_acquire))
                    break;
            }
        }
    });

    std::vector<char> payload(1200, static_cast<char>(0x5a));
    const auto start = Clock::now();
    while (result.bytesSent < targetBytes)
    {
        const int toSend = static_cast<int>(std::min<uint64_t>(payload.size(), targetBytes - result.bytesSent));
        const int n = sendto(client,
                             payload.data(),
                             toSend,
                             0,
                             reinterpret_cast<const sockaddr*>(&address),
                             sizeof(address));
        if (n == SOCKET_ERROR)
        {
            result.error = WSAGetLastError();
            break;
        }
        result.bytesSent += static_cast<uint64_t>(n);
    }

    senderDone.store(true, std::memory_order_release);
    receiver.join();
    const auto end = Clock::now();
    result.bytesReceived = received.load(std::memory_order_relaxed);
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.ok = result.error == 0 && result.bytesSent > 0 && result.bytesReceived > 0;

    closesocket(server);
    closesocket(client);
    WSACleanup();
    return result;
}

void PrintTop(const std::unordered_map<DWORD, IoTotals>& values, bool byDisk, size_t count)
{
    struct Row { DWORD pid; IoTotals totals; };
    std::vector<Row> rows;
    rows.reserve(values.size());
    for (const auto& [pid, totals] : values)
    {
        const uint64_t key = byDisk ? totals.DiskBytes() : totals.NetworkBytes();
        if (key != 0)
            rows.push_back({pid, totals});
    }

    std::sort(rows.begin(), rows.end(), [byDisk](const Row& a, const Row& b)
    {
        const uint64_t ka = byDisk ? a.totals.DiskBytes() : a.totals.NetworkBytes();
        const uint64_t kb = byDisk ? b.totals.DiskBytes() : b.totals.NetworkBytes();
        return ka > kb;
    });

    const auto names = SnapshotProcessNames();
    count = std::min(count, rows.size());
    for (size_t i = 0; i < count; ++i)
    {
        const auto& row = rows[i];
        const auto it = names.find(row.pid);
        const std::string name = it != names.end() ? it->second : "<exited/unknown>";
        std::cout << "  " << std::setw(7) << row.pid << "  "
                  << std::left << std::setw(28) << name.substr(0, 28) << std::right;
        if (byDisk)
        {
            std::cout << "  R=" << std::setw(8) << std::fixed << std::setprecision(2) << ToMiB(row.totals.diskRead)
                      << " MiB  W=" << std::setw(8) << ToMiB(row.totals.diskWrite) << " MiB\n";
        }
        else
        {
            std::cout << "  TCP S=" << std::setw(8) << std::fixed << std::setprecision(2) << ToMiB(row.totals.tcpSend)
                      << " R=" << std::setw(8) << ToMiB(row.totals.tcpReceive)
                      << "  UDP S=" << std::setw(7) << ToMiB(row.totals.udpSend)
                      << " R=" << std::setw(7) << ToMiB(row.totals.udpReceive) << " MiB\n";
        }
    }
}

std::string SchemaStateText(int state, DWORD error)
{
    switch (state)
    {
    case 0: return "not observed";
    case 1: return "checking";
    case 2: return "PASS (raw parser matches TDH metadata)";
    case 3: return "TDH unavailable/inconclusive, error=" + std::to_string(error);
    case 4: return "FAIL (raw parser != TDH metadata)";
    default: return "unknown";
    }
}

void PrintHeader()
{
    std::cout << "EtwIoProbe - kernel ETW disk/network validation\n";
    std::cout << "Architecture: " << (sizeof(void*) == 8 ? "x64" : "x86") << "\n";
    std::cout << "Elevated: " << (IsElevated() ? "yes" : "NO") << "\n";
    std::cout << "PID: " << GetCurrentProcessId() << "\n";
}

struct Options
{
    enum class Mode { Help, SelfTest, Monitor } mode = Mode::Help;
    uint64_t diskMiB = 512;
    uint64_t networkMiB = 64;
    int monitorSeconds = 20;
    bool network = true;
    bool udpLoopback = true;
};

bool ParseUnsigned(const wchar_t* text, uint64_t& value)
{
    if (!text || !*text)
        return false;
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(text, &end, 10);
    if (!end || *end != L'\0')
        return false;
    value = parsed;
    return true;
}

Options ParseOptions(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--selftest")
            o.mode = Options::Mode::SelfTest;
        else if (arg == L"--monitor")
        {
            o.mode = Options::Mode::Monitor;
            if (i + 1 < argc)
            {
                uint64_t seconds = 0;
                if (ParseUnsigned(argv[i + 1], seconds))
                {
                    o.monitorSeconds = static_cast<int>(std::clamp<uint64_t>(seconds, 1, 3600));
                    ++i;
                }
            }
        }
        else if (arg == L"--disk-mib" && i + 1 < argc)
        {
            uint64_t v = 0;
            if (ParseUnsigned(argv[++i], v))
                o.diskMiB = std::clamp<uint64_t>(v, 16, 8192);
        }
        else if (arg == L"--network-mib" && i + 1 < argc)
        {
            uint64_t v = 0;
            if (ParseUnsigned(argv[++i], v))
                o.networkMiB = std::clamp<uint64_t>(v, 1, 1024);
        }
        else if (arg == L"--no-network")
            o.network = false;
        else if (arg == L"--no-udp")
            o.udpLoopback = false;
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            o.mode = Options::Mode::Help;
    }
    return o;
}

void PrintUsage()
{
    std::cout << "Usage:\n"
              << "  EtwIoProbe.exe --selftest [--disk-mib 512] [--network-mib 64] [--no-network] [--no-udp]\n"
              << "  EtwIoProbe.exe --monitor [seconds]\n\n"
              << "Run elevated. verify.cmd does that automatically.\n";
}

int RunMonitorMode(const Options& options)
{
    PrintHeader();
    EtwIoMonitor monitor;
    if (!monitor.Start())
    {
        std::cout << "ETW START: FAIL: " << monitor.StartError() << " ("
                  << Win32ErrorMessage(monitor.StartError()) << ")\n";
        return 3;
    }

    std::cout << "ETW START: PASS\n";
    std::cout << "Seeded TID->PID entries: " << monitor.SeededThreadCount() << "\n";
    std::cout << "Watching for " << options.monitorSeconds << " seconds. Generate disk/network traffic now.\n\n";

    auto previous = monitor.SnapshotAll();
    for (int second = 1; second <= options.monitorSeconds; ++second)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        monitor.Flush();
        auto current = monitor.SnapshotAll();
        std::unordered_map<DWORD, IoTotals> delta;
        for (const auto& [pid, now] : current)
        {
            const auto it = previous.find(pid);
            delta[pid] = it != previous.end() ? now - it->second : now;
        }
        previous = std::move(current);

        std::cout << "[" << second << "s] top disk (MiB/s)\n";
        PrintTop(delta, true, 5);
        std::cout << "[" << second << "s] top network (MiB/s)\n";
        PrintTop(delta, false, 5);
        std::cout << "\n";
    }

    const TraceStats stats = monitor.GetStats();
    std::cout << "Final events: disk=" << stats.diskEvents
              << " tcp=" << stats.tcpEvents
              << " udp=" << stats.udpEvents
              << " thread=" << stats.threadEvents << "\n";
    std::cout << "TID mapping: map=" << stats.tidMapHits
              << " fallback=" << stats.tidFallbackHits
              << " unresolved=" << stats.tidUnresolved << "\n";
    std::cout << "Lost: events=" << stats.eventsLost
              << " logBuffers=" << stats.logBuffersLost
              << " realtimeBuffers=" << stats.realtimeBuffersLost << "\n";

    monitor.Stop();
    return 0;
}

int RunSelfTest(const Options& options)
{
    PrintHeader();
    std::cout << "\n=== ETW session ===\n";

    if (!IsElevated())
    {
        std::cout << "VERDICT: FAIL - not elevated. Run verify.cmd.\n";
        return 3;
    }

    EtwIoMonitor monitor;
    if (!monitor.Start())
    {
        std::cout << "ETW START: FAIL: " << monitor.StartError() << " ("
                  << Win32ErrorMessage(monitor.StartError()) << ")\n";
        std::cout << "VERDICT: FAIL\n";
        return 3;
    }

    std::cout << "ETW START: PASS\n";
    std::cout << "Seeded TID->PID entries: " << monitor.SeededThreadCount() << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const DWORD selfPid = GetCurrentProcessId();
    bool hardFail = false;
    bool partial = false;

    std::cout << "\n=== Disk self-test ===\n";
    const IoTotals diskBefore = monitor.GetPid(selfPid);
    const TraceStats statsBeforeDisk = monitor.GetStats();
    const DiskTestResult disk = RunDiskTest(MiB(options.diskMiB));
    monitor.Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const IoTotals diskAfter = monitor.GetPid(selfPid);
    const IoTotals diskDelta = diskAfter - diskBefore;
    const TraceStats statsAfterDisk = monitor.GetStats();

    if (!disk.ok)
    {
        std::cout << "Disk generator: FAIL: " << disk.error << " ("
                  << Win32ErrorMessage(disk.error) << ")\n";
        hardFail = true;
    }
    else
    {
        const double generatedMiB = ToMiB(disk.writtenBytes);
        const double etwWriteMiB = ToMiB(diskDelta.diskWrite);
        const double ratio = disk.writtenBytes ? static_cast<double>(diskDelta.diskWrite) / disk.writtenBytes : 0.0;
        const double mbps = disk.seconds > 0 ? generatedMiB / disk.seconds : 0.0;

        std::cout << "Generated unbuffered/write-through: " << std::fixed << std::setprecision(2)
                  << generatedMiB << " MiB in " << disk.seconds << " s (" << mbps << " MiB/s)\n";
        std::cout << "ETW attributed to this PID: write=" << etwWriteMiB
                  << " MiB, read=" << ToMiB(diskDelta.diskRead) << " MiB\n";
        std::cout << "ETW/generated write ratio: " << std::setprecision(3) << ratio << "\n";
        std::cout << "Disk events during test: "
                  << (statsAfterDisk.diskEvents - statsBeforeDisk.diskEvents) << "\n";

        const bool diskPass = ratio >= 0.80 && ratio <= 1.25 &&
                              statsAfterDisk.diskEvents > statsBeforeDisk.diskEvents;
        std::cout << "Disk attribution: " << (diskPass ? "PASS" : "FAIL") << "\n";
        if (!diskPass)
            hardFail = true;
    }

    if (options.network)
    {
        std::cout << "\n=== TCP network self-test ===\n";
        const uint64_t requestedBytes = MiB(options.networkMiB);
        std::wstringstream url;
        url << L"https://speed.cloudflare.com/__down?bytes=" << requestedBytes;

        const IoTotals netBefore = monitor.GetPid(selfPid);
        const TraceStats statsBeforeNet = monitor.GetStats();
        const NetworkTestResult network = RunHttpDownload(url.str());
        monitor.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const IoTotals netAfter = monitor.GetPid(selfPid);
        const IoTotals netDelta = netAfter - netBefore;
        const TraceStats statsAfterNet = monitor.GetStats();

        if (!network.ok)
        {
            std::cout << "HTTP generator: UNAVAILABLE: error=" << network.error
                      << " (" << Win32ErrorMessage(network.error) << ")"
                      << " status=" << network.httpStatus << "\n";
            std::cout << "TCP validation: INCONCLUSIVE (internet test failed)\n";
            partial = true;
        }
        else
        {
            const double payloadMiB = ToMiB(network.bytesReceived);
            const double etwRecvMiB = ToMiB(netDelta.tcpReceive);
            const double ratio = network.bytesReceived ?
                static_cast<double>(netDelta.tcpReceive) / network.bytesReceived : 0.0;
            const double mbps = network.seconds > 0 ? payloadMiB / network.seconds : 0.0;

            std::cout << "Downloaded payload: " << std::fixed << std::setprecision(2)
                      << payloadMiB << " MiB in " << network.seconds << " s (" << mbps << " MiB/s)\n";
            std::cout << "ETW attributed to this PID: TCP recv=" << etwRecvMiB
                      << " MiB, send=" << ToMiB(netDelta.tcpSend) << " MiB\n";
            std::cout << "ETW/payload receive ratio: " << std::setprecision(3) << ratio << "\n";
            std::cout << "TCP events during test: "
                      << (statsAfterNet.tcpEvents - statsBeforeNet.tcpEvents) << "\n";

            const bool tcpPass = ratio >= 0.80 && ratio <= 1.30 &&
                                 statsAfterNet.tcpEvents > statsBeforeNet.tcpEvents;
            std::cout << "TCP attribution: " << (tcpPass ? "PASS" : "FAIL") << "\n";
            if (!tcpPass)
                hardFail = true;
        }
    }

    if (options.udpLoopback)
    {
        std::cout << "\n=== UDP loopback parser test ===\n";
        const IoTotals before = monitor.GetPid(selfPid);
        const TraceStats statsBefore = monitor.GetStats();
        const UdpLoopbackResult udp = RunUdpLoopback(MiB(4));
        monitor.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const IoTotals after = monitor.GetPid(selfPid);
        const IoTotals delta = after - before;
        const TraceStats statsAfter = monitor.GetStats();

        std::cout << "Loopback payload: sent=" << std::fixed << std::setprecision(2)
                  << ToMiB(udp.bytesSent) << " MiB, received=" << ToMiB(udp.bytesReceived) << " MiB\n";
        std::cout << "ETW this PID: UDP send=" << ToMiB(delta.udpSend)
                  << " MiB, receive=" << ToMiB(delta.udpReceive) << " MiB\n";
        std::cout << "UDP events during test: " << (statsAfter.udpEvents - statsBefore.udpEvents) << "\n";

        if (!udp.ok)
        {
            std::cout << "UDP generator: INCONCLUSIVE (Winsock error " << udp.error << ")\n";
            partial = true;
        }
        else if (statsAfter.udpEvents == statsBefore.udpEvents)
        {
            // Loopback can be optimized differently across Windows/network stacks; do not fail the
            // whole ETW design solely on this test. Any normal background UDP events are still counted.
            std::cout << "UDP parser: INCONCLUSIVE (no ETW UDP events for loopback)\n";
            partial = true;
        }
        else
        {
            std::cout << "UDP parser: PASS (provider/events observed)\n";
        }
    }

    std::cout << "\n=== Trace integrity ===\n";
    const TraceStats stats = monitor.GetStats();
    const uint64_t lossTotal = stats.eventsLost + stats.logBuffersLost + stats.realtimeBuffersLost;
    const double unresolvedPct = stats.diskEvents ?
        100.0 * static_cast<double>(stats.tidUnresolved) / static_cast<double>(stats.diskEvents) : 0.0;

    std::cout << "Events: disk=" << stats.diskEvents
              << " tcp=" << stats.tcpEvents
              << " udp=" << stats.udpEvents
              << " thread=" << stats.threadEvents << "\n";
    std::cout << "Disk TID->PID: map hits=" << stats.tidMapHits
              << " fallback hits=" << stats.tidFallbackHits
              << " unresolved=" << stats.tidUnresolved
              << " (" << std::fixed << std::setprecision(4) << unresolvedPct << "%)\n";
    std::cout << "Lost: events=" << stats.eventsLost
              << " logBuffers=" << stats.logBuffersLost
              << " realtimeBuffers=" << stats.realtimeBuffersLost << "\n";
    std::cout << "Schema cross-check: Disk=" << SchemaStateText(stats.diskSchemaState, stats.diskSchemaError) << "\n";
    std::cout << "Schema cross-check: TCP =" << SchemaStateText(stats.tcpSchemaState, stats.tcpSchemaError) << "\n";
    std::cout << "Schema cross-check: UDP =" << SchemaStateText(stats.udpSchemaState, stats.udpSchemaError) << "\n";

    const bool schemaMismatch = stats.diskSchemaState == 4 || stats.tcpSchemaState == 4 || stats.udpSchemaState == 4;
    const bool integrityPass = lossTotal == 0 && unresolvedPct < 1.0 && !schemaMismatch;
    std::cout << "Trace integrity: " << (integrityPass ? "PASS" : "FAIL") << "\n";
    if (!integrityPass)
        hardFail = true;

    std::cout << "\nTop cumulative disk processes:\n";
    PrintTop(monitor.SnapshotAll(), true, 8);
    std::cout << "\nTop cumulative network processes:\n";
    PrintTop(monitor.SnapshotAll(), false, 8);

    monitor.Stop();

    std::cout << "\n=== FINAL ===\n";
    if (hardFail)
    {
        std::cout << "VERDICT: FAIL\n";
        return 3;
    }
    if (partial)
    {
        std::cout << "VERDICT: PARTIAL - core ETW ran, but one optional validation was inconclusive.\n";
        return 2;
    }

    std::cout << "VERDICT: PASS\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const Options options = ParseOptions(argc, argv);
    switch (options.mode)
    {
    case Options::Mode::SelfTest:
        return RunSelfTest(options);
    case Options::Mode::Monitor:
        return RunMonitorMode(options);
    case Options::Mode::Help:
    default:
        PrintUsage();
        return 0;
    }
}
