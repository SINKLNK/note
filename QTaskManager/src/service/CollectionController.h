#pragma once
#include "monitoring/MonitoringTypes.h"
#include <functional>
namespace monitoring
{
class CollectionController
{
  public:
    CollectionController(std::function<SourceStatus()> start, std::function<void()> stop)
        : start_(std::move(start)), stop_(std::move(stop))
    {
    }
    void tick(bool wanted, uint64_t nowMs);
    void stop();
    void fault(SourceStatus status, uint64_t nowMs);
    bool active() const
    {
        return active_;
    }
    SourceStatus status() const
    {
        return status_;
    }

  private:
    std::function<SourceStatus()> start_;
    std::function<void()> stop_;
    bool active_{};
    uint64_t retryAt_{};
    SourceStatus status_;
};
} // namespace monitoring
