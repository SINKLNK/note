#pragma once
#include "monitoring/MonitoringTypes.h"
#include <map>
namespace monitoring
{
enum class IoKind
{
    DiskRead,
    DiskWrite,
    NetSend,
    NetReceive
};
class IoAccumulator
{
    struct Counts
    {
        uint64_t disk{}, network{};
    };
    std::map<ProcessKey, Counts> counts_;
    uint64_t last_{};

  public:
    void add(ProcessKey, IoKind, uint64_t);
    std::vector<IoRate> sample(uint64_t now, uint64_t frequency);
    void reset()
    {
        counts_.clear();
        last_ = 0;
    }
};
} // namespace monitoring
