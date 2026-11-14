#pragma once
#include "monitoring/MonitoringTypes.h"
#include <functional>
namespace monitoring
{
SourceStatus initializeServiceEndpoint(const std::function<SourceStatus()> &initialize,
                                       const std::function<void()> &ready);
struct TraceStopOperations
{
    std::function<uint32_t()> stopSession;
    std::function<void()> closeConsumer;
    std::function<bool(uint32_t)> waitConsumer;
};
struct TraceStopResult
{
    uint32_t error{};
    bool consumerExited{};
};
TraceStopResult stopTraceConsumer(const TraceStopOperations &);
} // namespace monitoring
