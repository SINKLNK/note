#pragma once
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <string>
#include <vector>
namespace monitoring
{
inline constexpr uint16_t ProtocolVersion = 3;
inline constexpr char ServiceVersion[] = "1.2.0";
inline constexpr wchar_t ServiceName[] = L"QTaskManagerService";
// Keep the discovery endpoint stable; the frame header rejects incompatible protocols.
inline constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\QTaskManager.Monitoring.v1";
struct ProcessKey
{
    uint32_t pid{};
    uint64_t creationTime100ns{};
    auto operator<=>(const ProcessKey &) const = default;
};
struct LogicalCpuId
{
    uint16_t group{}, number{};
    auto operator<=>(const LogicalCpuId &) const = default;
};
enum class SourceState : uint8_t
{
    Unavailable,
    WarmingUp,
    Ready,
    Partial,
    Failed
};
struct SourceStatus
{
    SourceState state = SourceState::Unavailable;
    uint32_t win32Error{};
    std::string detail;
};
struct IoRate
{
    ProcessKey key;
    // diskMBps is the primary disk-related column: logical FileIo request bytes since protocol 3.
    double diskMBps = -1, networkMBps = -1;
};
enum class SensorScope : uint8_t
{
    Core,
    Group,
    Package
};
struct SensorReading
{
    uint32_t sensorId{};
    SensorScope scope{};
    uint32_t packageId{}, coreId{};
    double celsius{};
    bool valid{};
};
enum class AttributionIssue : uint8_t
{
    DiskMissingThreadId,
    DiskUnknownThread,
    NetworkMissingProcessId,
    NetworkUnknownProcess,
    ProcessOpenFailed,
    ProcessTimesFailed,
    ProcessNewerThanEvent,
    ThreadUnknownProcess,
    Count
};
struct AttributionCount
{
    uint64_t events{}, bytes{};
    uint32_t lastPid{}, lastTid{}, lastError{};
    auto operator<=>(const AttributionCount &) const = default;
};
struct AttributionCounters
{
    uint64_t diskEvents{}, networkEvents{};
    std::array<AttributionCount, size_t(AttributionIssue::Count)> issues{};
    auto operator<=>(const AttributionCounters &) const = default;
};
struct Diagnostics
{
    uint64_t lostEvents{}, lostBuffers{}, unresolvedIds{}, unsupportedSchemas{};
    bool truncated{};
    // Totals are reset when collection restarts; interval counters describe one service sample.
    AttributionCounters attributionTotal, attributionInterval;
};
struct Snapshot
{
    uint64_t sequence{}, sampleQpc{}, qpcFrequency{}, intervalQpc{};
    SourceStatus disk, network, temperatures;
    std::vector<IoRate> processes;
    std::vector<SensorReading> sensors;
    Diagnostics diagnostics;
};
struct Status
{
    uint16_t protocolVersion = ProtocolVersion;
    std::string serviceVersion = ServiceVersion;
    bool temperatureEnabled = false;
    SourceStatus disk, network, temperatures;
};
} // namespace monitoring
