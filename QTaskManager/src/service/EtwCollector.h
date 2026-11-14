#pragma once
#include "EtwDecoder.h"
#include "EventAttribution.h"
#include "CollectorHealth.h"
#include "IoAccumulator.h"
#include "ProcessIdentity.h"
#include "monitoring/win/WinHandle.h"
#include <atomic>
#include <mutex>
#include <thread>
namespace monitoring
{
class EtwCollector
{
  public:
    EtwCollector();
    ~EtwCollector()
    {
        stop();
    }
    SourceStatus start();
    void stop();
    Snapshot snapshot(uint64_t nowQpc);
    bool cleanupPending() const { return session_ != 0 && !thread_.joinable(); }
    SourceStatus teardownStatus() const { return startStatus_; }

  private:
    static void WINAPI callback(EVENT_RECORD *);
    void event(const EVENT_RECORD &);
    void seed();
    ProcessLookupResult queryPid(uint32_t, uint64_t);
    std::vector<unsigned char> properties();
    std::mutex mutex_;
    EtwDecoder decoder_;
    ProcessIdentity identities_;
    IoAccumulator totals_;
    EventAttribution attribution_;
    TRACEHANDLE session_{}, consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    std::wstring sessionName_;
    uint64_t sequence_{}, lastQpc_{}, frequency_{}, anchorQpc_{}, anchorFiletime_{};
    SourceStatus startStatus_;
    bool truncated_{};
    CollectorHealth health_;
};
} // namespace monitoring
