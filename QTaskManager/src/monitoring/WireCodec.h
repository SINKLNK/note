#pragma once
#include "MonitoringTypes.h"
#include <cstddef>
#include <optional>
#include <span>
namespace monitoring
{
inline constexpr size_t MaxPayload = 4 * 1024 * 1024, HeaderSize = 20;
enum class MessageKind : uint16_t
{
    Hello = 1,
    HelloReply,
    Heartbeat,
    GetStatus,
    StatusReply,
    GetSnapshot,
    SnapshotReply,
    Error
};
struct Frame
{
    MessageKind kind{};
    uint64_t requestId{};
    std::vector<std::byte> payload;
};
enum class DecodeError
{
    None,
    NeedMore,
    TooLarge,
    BadMagic,
    UnsupportedVersion,
    Malformed
};
struct DecodeResult
{
    DecodeError error{};
    size_t consumed{};
    std::optional<Frame> frame;
};
template <class T> struct ValueDecodeResult
{
    DecodeError error{};
    std::optional<T> value;
};
std::vector<std::byte> encodeFrame(const Frame &);
DecodeResult decodeFrame(std::span<const std::byte>);
std::vector<std::byte> encodeStatus(const Status &);
std::vector<std::byte> encodeSnapshot(const Snapshot &);
ValueDecodeResult<Status> decodeStatus(std::span<const std::byte>);
ValueDecodeResult<Snapshot> decodeSnapshot(std::span<const std::byte>);
} // namespace monitoring
