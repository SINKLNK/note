#pragma once
#include "monitoring/MonitoringTypes.h"
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <functional>
#include <map>
#include <optional>
#include <windows.h>
namespace monitoring
{
inline constexpr GUID FileIoGuid = {
    0x90cbdc39, 0x4a3e, 0x11d1, {0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3}};
inline constexpr GUID ThreadGuid = {
    0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr GUID ProcessGuid = {
    0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr GUID TcpGuid = {
    0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2}};
inline constexpr GUID UdpGuid = {
    0xbf3a50c5, 0xa9c9, 0x4988, {0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80}};
enum class EventKind
{
    ProcessStart,
    ProcessEnd,
    ThreadStart,
    ThreadEnd,
    DiskRead, // Primary column: FileIo read/write request bytes, including cached I/O.
    DiskWrite,
    NetSend,
    NetReceive
};
struct DecodedEvent
{
    EventKind kind;
    uint64_t qpc{};
    uint32_t pid{}, tid{}, bytes{};
};
class EtwDecoder
{
  public:
    using PropertyReader = std::function<std::optional<uint64_t>(const EVENT_RECORD &, const wchar_t *)>;
    explicit EtwDecoder(PropertyReader reader = {});
    std::optional<DecodedEvent> decode(const EVENT_RECORD &);
    SourceStatus diskStatus{SourceState::Ready}, networkStatus{SourceState::Ready};
    uint64_t unsupported{};

  private:
    PropertyReader reader_;
    std::map<std::tuple<unsigned, unsigned, unsigned, bool>, bool> schemas_;
};
} // namespace monitoring
