#pragma once
#include <cstdint>
namespace monitoring
{
inline constexpr wchar_t ConfigKey[] = L"Software\\QTaskManager\\Monitoring";
struct ServiceConfig
{
    bool temperatureEnabled = false;
    bool intentionallyDisabled = false;
    uint32_t schemaVersion = 1;
};
ServiceConfig readServiceConfig();
uint32_t writeServiceConfig(const ServiceConfig &);
} // namespace monitoring
