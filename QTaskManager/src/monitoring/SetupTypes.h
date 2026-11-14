#pragma once
#include <cstdint>
#include <string>
namespace monitoring
{
inline constexpr uint32_t SetupRollbackIncomplete = 0x20000001;
enum class SetupAction : uint32_t
{
    InstallService,
    EnableService,
    DisableService,
    RemoveService,
    UpdateService,
    EnableTemperatures,
    DisableTemperatures,
    InstallPawnIo,
    RemovePawnIo
};
struct SetupRequest
{
    SetupAction action{};
    bool withTemperatures{};
};
enum class SetupOutcome : uint32_t
{
    Success,
    Cancelled,
    Partial,
    Failed,
    Unsupported
};
struct SetupResult
{
    SetupOutcome outcome = SetupOutcome::Failed;
    uint32_t win32Error{};
    bool rebootRequired{};
    std::string detail;
};
// Reserved for the next phase. Unknown provenance never grants removal rights.
enum class DriverOwnership
{
    Absent,
    QTaskManager,
    External,
    Unknown
};
struct PawnIoState
{
    DriverOwnership ownership = DriverOwnership::Unknown;
    bool installed{}, temperatureEnabled{}, rebootRequired{};
    std::string version;
};
inline bool mayRemovePawnIo(DriverOwnership owner, bool identityMatches)
{
    return owner == DriverOwnership::QTaskManager && identityMatches;
}
} // namespace monitoring
