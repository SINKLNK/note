#pragma once
#include "monitoring/MonitoringTypes.h"
#include <map>
#include <optional>
namespace monitoring
{
class ProcessIdentity
{
    struct Life
    {
        uint64_t begin{}, end = UINT64_MAX;
        ProcessKey key;
    };
    std::multimap<uint32_t, Life> pids_, tids_;
    static std::optional<ProcessKey> resolve(const std::multimap<uint32_t, Life> &, uint32_t, uint64_t);
    static void start(std::multimap<uint32_t, Life> &, uint32_t, ProcessKey, uint64_t);
    static void end(std::multimap<uint32_t, Life> &, uint32_t, uint64_t);

  public:
    void processStart(ProcessKey key, uint64_t qpc);
    void processEnd(uint32_t pid, uint64_t qpc);
    void threadStart(uint32_t tid, ProcessKey key, uint64_t qpc);
    void threadEnd(uint32_t tid, uint64_t qpc);
    std::optional<ProcessKey> resolvePid(uint32_t pid, uint64_t qpc) const;
    std::optional<ProcessKey> resolveTid(uint32_t tid, uint64_t qpc) const;
    std::vector<ProcessKey> live() const;
    bool prune(uint64_t before);
};
} // namespace monitoring
