#include "EtwCollector.h"
#include "ServiceRuntime.h"
#include <cstring>
#include <map>
#include <tlhelp32.h>
namespace monitoring
{
namespace
{
constexpr GUID SessionGuid = {0xf9f88556, 0x8620, 0x43c0, {0x81, 0x85, 0xed, 0x43, 0x26, 0x3c, 0x4a, 0x53}};
uint64_t ft(FILETIME t)
{
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
} // namespace
EtwCollector::EtwCollector()
    : attribution_([this](uint32_t pid, uint64_t at) { return queryPid(pid, at); })
{
}
std::vector<unsigned char> EtwCollector::properties()
{
    std::vector<unsigned char> b(sizeof(EVENT_TRACE_PROPERTIES) +
                                 (sessionName_.size() + 1) * sizeof(wchar_t));
    auto *p = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(b.data());
    p->Wnode.BufferSize = ULONG(b.size());
    p->Wnode.Guid = SessionGuid;
    p->Wnode.ClientContext = 1;
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->BufferSize = 64;
    p->MinimumBuffers = 8;
    p->MaximumBuffers = 128;
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    p->FlushTimer = 1;
    // Request-level file I/O includes buffered activity in the originating process.
    // Do not also enable DiskIo: adding its completions would mix two layers and double-count bytes.
    p->EnableFlags = EVENT_TRACE_FLAG_FILE_IO_INIT | EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_PROCESS |
                     EVENT_TRACE_FLAG_THREAD;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    memcpy(b.data() + p->LoggerNameOffset, sessionName_.c_str(), (sessionName_.size() + 1) * sizeof(wchar_t));
    return b;
}
ProcessLookupResult EtwCollector::queryPid(uint32_t pid, uint64_t at)
{
    WinHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME birth, exit, kernel, user;
    if (!h)
        return {{}, AttributionIssue::ProcessOpenFailed, GetLastError()};
    if (!GetProcessTimes(h.get(), &birth, &exit, &kernel, &user))
        return {{}, AttributionIssue::ProcessTimesFailed, GetLastError()};
    uint64_t created = ft(birth);
    int64_t delta = int64_t(at) - int64_t(anchorQpc_);
    int64_t stamp = int64_t(anchorFiletime_) + int64_t(double(delta) * 10000000.0 / double(frequency_));
    if (stamp < 0 || created > uint64_t(stamp))
        return {{}, AttributionIssue::ProcessNewerThanEvent, 0};
    return {ProcessKey{pid, created}};
}
void EtwCollector::seed()
{
    uint64_t at = qpcNow();
    WinHandle ps(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W pe{sizeof(pe)};
    if (ps && Process32FirstW(ps.get(), &pe))
        do
        {
            attribution_.consume({EventKind::ProcessStart, at, pe.th32ProcessID}, identities_, totals_);
        } while (Process32NextW(ps.get(), &pe));
    WinHandle ts(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    THREADENTRY32 te{sizeof(te)};
    if (ts && Thread32First(ts.get(), &te))
        do
        {
            attribution_.consume({EventKind::ThreadStart, at, te.th32OwnerProcessID, te.th32ThreadID},
                                 identities_, totals_);
        } while (Thread32Next(ts.get(), &te));
}
SourceStatus EtwCollector::start()
{
    if (session_)
    {
        stop();
        if (session_)
            return startStatus_;
    }
    frequency_ = qpcFrequency();
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    anchorFiletime_ = ft(now);
    anchorQpc_ = qpcNow();
    sessionName_ = L"QTaskManager.Monitoring." + std::to_wstring(GetCurrentProcessId());
    auto b = properties();
    auto *p = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(b.data());
    auto error = StartTraceW(&session_, sessionName_.c_str(), p);
    if (error)
    {
        session_ = 0;
        return startStatus_ = {SourceState::Failed, error, "StartTrace failed"};
    }
    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = sessionName_.data();
    log.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = callback;
    log.Context = this;
    consumer_ = OpenTraceW(&log);
    if (consumer_ == INVALID_PROCESSTRACE_HANDLE)
    {
        error = GetLastError();
        stop();
        return startStatus_ = {SourceState::Failed, error, "OpenTrace failed"};
    }
    {
        std::lock_guard lock(mutex_);
        identities_ = {};
        totals_.reset();
        decoder_ = EtwDecoder{};
        attribution_.reset();
        truncated_ = false;
        lastQpc_ = qpcNow();
        totals_.sample(lastQpc_, frequency_);
        seed();
    }
    health_.reset();
    thread_ = std::thread([this, handle = consumer_] {
        auto h = handle;
        health_.finished(ProcessTrace(&h, 1, nullptr, nullptr));
    });
    return startStatus_ = {SourceState::WarmingUp, 0, "Waiting for the first complete interval"};
}
void EtwCollector::stop()
{
    auto closeConsumer = [this] {
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE)
        {
            CloseTrace(consumer_);
            consumer_ = INVALID_PROCESSTRACE_HANDLE;
        }
    };
    auto stopped =
        stopTraceConsumer({[this]() -> uint32_t {
                               if (!session_)
                                   return 0;
                               auto b = properties();
                               auto error = ControlTraceW(
                                   session_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(b.data()),
                                   EVENT_TRACE_CONTROL_STOP);
                               if (error == ERROR_SUCCESS || error == ERROR_WMI_INSTANCE_NOT_FOUND)
                               {
                                   session_ = 0;
                                   return 0;
                               }
                               return error; // Retain ownership so a later retry can stop our session.
                           },
                           closeConsumer,
                           [this](uint32_t timeout) {
                               return !thread_.joinable() ||
                                      WaitForSingleObject(thread_.native_handle(), timeout) == WAIT_OBJECT_0;
                           }});
    if (!stopped.consumerExited)
    {
        // A callback still references this object. Fail the service process instead of detaching it
        // or blocking idle/resume handling forever. No other process is terminated.
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        std::terminate();
    }
    if (thread_.joinable())
        thread_.join();
    closeConsumer();
    if (stopped.error)
        startStatus_ = {SourceState::Failed, stopped.error, "ETW teardown failed"};
}
void WINAPI EtwCollector::callback(EVENT_RECORD *record)
{
    auto *self = static_cast<EtwCollector *>(record->UserContext);
    if (self)
        self->event(*record);
}
void EtwCollector::event(const EVENT_RECORD &record)
{
    // FILE_IO_INIT also emits create/info/directory events. They are not byte rates;
    // reject them before contending with the snapshot thread or touching schema metadata.
    if (IsEqualGUID(record.EventHeader.ProviderId, FileIoGuid) &&
        record.EventHeader.EventDescriptor.Opcode != 67 && record.EventHeader.EventDescriptor.Opcode != 68)
        return;
    std::lock_guard lock(mutex_);
    auto e = decoder_.decode(record);
    if (e)
        attribution_.consume(*e, identities_, totals_);
}
Snapshot EtwCollector::snapshot(uint64_t now)
{
    uint64_t lost = 0, buffers = 0;
    if (session_)
    {
        auto b = properties();
        auto *p = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(b.data());
        if (ControlTraceW(session_, nullptr, p, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS)
        {
            lost = p->EventsLost;
            buffers = uint64_t(p->LogBuffersLost) + p->RealTimeBuffersLost;
        }
    }
    std::lock_guard lock(mutex_);
    Snapshot s;
    s.sequence = ++sequence_;
    s.sampleQpc = now;
    s.qpcFrequency = frequency_;
    s.intervalQpc = now > lastQpc_ ? now - lastQpc_ : 0;
    s.disk = session_ ? decoder_.diskStatus : startStatus_;
    s.network = session_ ? decoder_.networkStatus : startStatus_;
    if (auto failure = health_.check(lost, buffers, identities_, totals_))
    {
        s.disk = s.network = *failure;
        s.diagnostics = {lost, buffers, 0, decoder_.unsupported, truncated_};
        attribution_.finishInterval(s.diagnostics, s.disk, s.network);
        lastQpc_ = now;
        return s; // Never publish rates based on an identity epoch with lost lifecycle events.
    }
    auto active = totals_.sample(now, frequency_);
    std::map<ProcessKey, IoRate> rows;
    for (auto k : identities_.live())
        rows[k] = {k, 0, 0};
    for (auto row : active)
        rows[row.key] = row;
    bool warm = startStatus_.state == SourceState::WarmingUp;
    startStatus_.state = SourceState::Ready;
    for (auto *source : {&s.disk, &s.network})
        if (source->state == SourceState::Ready)
        {
            if (warm)
                source->state = SourceState::WarmingUp;
            else if (truncated_)
                source->state = SourceState::Partial;
        }
    for (auto &[key, row] : rows)
    {
        if (s.processes.size() == 32768)
        {
            truncated_ = true;
            break;
        }
        if (s.disk.state == SourceState::Failed || s.disk.state == SourceState::WarmingUp)
            row.diskMBps = -1;
        if (s.network.state == SourceState::Failed || s.network.state == SourceState::WarmingUp)
            row.networkMBps = -1;
        s.processes.push_back(row);
    }
    truncated_ |= identities_.prune(now > frequency_ * 30 ? now - frequency_ * 30 : 0);
    s.diagnostics = {lost, buffers, 0, decoder_.unsupported, truncated_};
    attribution_.finishInterval(s.diagnostics, s.disk, s.network);
    lastQpc_ = now;
    return s;
}
} // namespace monitoring
