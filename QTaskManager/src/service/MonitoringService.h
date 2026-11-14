#pragma once
#include "CollectionController.h"
#include "EtwCollector.h"
#include "PipeServer.h"
#include "TemperatureSource.h"
namespace monitoring
{
class MonitoringService
{
  public:
    MonitoringService();
    uint32_t run(const std::function<void()> &ready);
    void requestStop();
    void resume()
    {
        resume_ = true;
    }

  private:
    WinHandle stop_;
    std::atomic<bool> resume_{};
    ClientLeases leases_;
    PipeServer pipe_;
    EtwCollector etw_;
    UnavailableTemperatures temperatures_;
    std::mutex statusMutex_;
    Status status_;
};
} // namespace monitoring
