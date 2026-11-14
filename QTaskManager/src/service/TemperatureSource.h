#pragma once
#include "monitoring/MonitoringTypes.h"
namespace monitoring
{
class TemperatureSource
{
  public:
    virtual ~TemperatureSource() = default;
    virtual SourceStatus start() = 0;
    virtual std::vector<SensorReading> sample() = 0;
    virtual void stop() = 0;
};
class UnavailableTemperatures final : public TemperatureSource
{
  public:
    SourceStatus start() override
    {
        return {SourceState::Unavailable, 0, "Temperature component is not included in this build"};
    }
    std::vector<SensorReading> sample() override
    {
        return {};
    }
    void stop() override
    {
    }
};
} // namespace monitoring
