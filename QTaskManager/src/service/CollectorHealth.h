#pragma once
#include "ProcessIdentity.h"
#include "IoAccumulator.h"
#include <atomic>
namespace monitoring
{
class CollectorHealth
{
    std::atomic<bool> exited_{};
    std::atomic<uint32_t> exitCode_{};
    bool invalid_{};

  public:
    void reset();
    void finished(uint32_t error);
    std::optional<SourceStatus> check(uint64_t lostEvents, uint64_t lostBuffers, ProcessIdentity &,
                                      IoAccumulator &);
};
} // namespace monitoring
