#include "WireCodec.h"
#include <bit>
#include <cmath>
#include <limits>
namespace monitoring
{
namespace
{
struct Writer
{
    std::vector<std::byte> bytes;
    bool ok = true;
    void integer(uint64_t n, size_t width)
    {
        for (size_t i = 0; i < width; ++i)
            bytes.push_back(std::byte((n >> (i * 8)) & 255));
    }
    void string(const std::string &s)
    {
        if (s.size() > 4096)
        {
            ok = false;
            return;
        }
        integer(s.size(), 4);
        for (char c : s)
            bytes.push_back(std::byte(c));
    }
    void real(double d)
    {
        if (!std::isfinite(d))
            ok = false;
        integer(std::bit_cast<uint64_t>(d), 8);
    }
    void source(const SourceStatus &s)
    {
        if (s.state > SourceState::Failed)
            ok = false;
        integer(uint8_t(s.state), 1);
        integer(s.win32Error, 4);
        string(s.detail);
    }
    void attribution(const AttributionCounters &counters)
    {
        integer(counters.diskEvents, 8);
        integer(counters.networkEvents, 8);
        for (const auto &count : counters.issues)
        {
            integer(count.events, 8);
            integer(count.bytes, 8);
            integer(count.lastPid, 4);
            integer(count.lastTid, 4);
            integer(count.lastError, 4);
        }
    }
    std::vector<std::byte> finish()
    {
        return ok && bytes.size() <= MaxPayload ? std::move(bytes) : std::vector<std::byte>{};
    }
};
struct Reader
{
    std::span<const std::byte> bytes;
    size_t pos{};
    bool ok = true;
    uint64_t integer(size_t width)
    {
        if (width > bytes.size() - pos)
        {
            ok = false;
            return 0;
        }
        uint64_t n = 0;
        for (size_t i = 0; i < width; ++i)
            n |= uint64_t(std::to_integer<uint8_t>(bytes[pos++])) << (i * 8);
        return n;
    }
    std::string string()
    {
        auto n = integer(4);
        if (!ok || n > 4096 || n > bytes.size() - pos)
        {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char *>(bytes.data() + pos), size_t(n));
        pos += size_t(n);
        return s;
    }
    bool boolean()
    {
        auto n = integer(1);
        if (n > 1)
            ok = false;
        return n != 0;
    }
    double real()
    {
        double d = std::bit_cast<double>(integer(8));
        if (!std::isfinite(d))
            ok = false;
        return d;
    }
    SourceStatus source()
    {
        SourceStatus s;
        s.state = SourceState(integer(1));
        if (s.state > SourceState::Failed)
            ok = false;
        s.win32Error = uint32_t(integer(4));
        s.detail = string();
        return s;
    }
    AttributionCounters attribution()
    {
        AttributionCounters counters;
        counters.diskEvents = integer(8);
        counters.networkEvents = integer(8);
        for (auto &count : counters.issues)
        {
            count.events = integer(8);
            count.bytes = integer(8);
            count.lastPid = uint32_t(integer(4));
            count.lastTid = uint32_t(integer(4));
            count.lastError = uint32_t(integer(4));
        }
        return counters;
    }
    bool done() const
    {
        return ok && pos == bytes.size();
    }
};
bool validRate(double d)
{
    return std::isfinite(d) && (d >= 0 || d == -1);
}
} // namespace
std::vector<std::byte> encodeFrame(const Frame &f)
{
    if (f.payload.size() > MaxPayload || f.kind < MessageKind::Hello || f.kind > MessageKind::Error)
        return {};
    Writer w;
    w.integer(0x314d5451, 4);
    w.integer(ProtocolVersion, 2);
    w.integer(uint16_t(f.kind), 2);
    w.integer(f.payload.size(), 4);
    w.integer(f.requestId, 8);
    w.bytes.insert(w.bytes.end(), f.payload.begin(), f.payload.end());
    return std::move(w.bytes);
}
DecodeResult decodeFrame(std::span<const std::byte> bytes)
{
    if (bytes.size() < HeaderSize)
        return {DecodeError::NeedMore};
    Reader r{bytes};
    if (r.integer(4) != 0x314d5451)
        return {DecodeError::BadMagic};
    if (r.integer(2) != ProtocolVersion)
        return {DecodeError::UnsupportedVersion};
    auto kind = MessageKind(r.integer(2));
    auto n = r.integer(4);
    auto id = r.integer(8);
    if (n > MaxPayload)
        return {DecodeError::TooLarge};
    if (kind < MessageKind::Hello || kind > MessageKind::Error)
        return {DecodeError::Malformed};
    if (n > bytes.size() - HeaderSize)
        return {DecodeError::NeedMore};
    return {DecodeError::None, HeaderSize + size_t(n),
            Frame{kind, id, {bytes.begin() + HeaderSize, bytes.begin() + HeaderSize + size_t(n)}}};
}
std::vector<std::byte> encodeStatus(const Status &s)
{
    Writer w;
    w.integer(s.protocolVersion, 2);
    w.string(s.serviceVersion);
    w.integer(s.temperatureEnabled, 1);
    w.source(s.disk);
    w.source(s.network);
    w.source(s.temperatures);
    return w.finish();
}
ValueDecodeResult<Status> decodeStatus(std::span<const std::byte> bytes)
{
    if (bytes.size() > MaxPayload)
        return {DecodeError::TooLarge};
    Reader r{bytes};
    Status s;
    s.protocolVersion = uint16_t(r.integer(2));
    s.serviceVersion = r.string();
    s.temperatureEnabled = r.boolean();
    s.disk = r.source();
    s.network = r.source();
    s.temperatures = r.source();
    if (!r.done())
        return {DecodeError::Malformed};
    return {DecodeError::None, std::move(s)};
}
std::vector<std::byte> encodeSnapshot(const Snapshot &s)
{
    if (s.processes.size() > 32768 || s.sensors.size() > 4096)
        return {};
    Writer w;
    w.integer(s.sequence, 8);
    w.integer(s.sampleQpc, 8);
    w.integer(s.qpcFrequency, 8);
    w.integer(s.intervalQpc, 8);
    w.source(s.disk);
    w.source(s.network);
    w.source(s.temperatures);
    w.integer(s.processes.size(), 4);
    for (auto &p : s.processes)
    {
        w.integer(p.key.pid, 4);
        w.integer(p.key.creationTime100ns, 8);
        w.real(p.diskMBps);
        w.real(p.networkMBps);
        if (!validRate(p.diskMBps) || !validRate(p.networkMBps))
            w.ok = false;
    }
    w.integer(s.sensors.size(), 4);
    for (auto &t : s.sensors)
    {
        w.integer(t.sensorId, 4);
        w.integer(uint8_t(t.scope), 1);
        w.integer(t.packageId, 4);
        w.integer(t.coreId, 4);
        w.real(t.celsius);
        w.integer(t.valid, 1);
        if (t.scope > SensorScope::Package)
            w.ok = false;
    }
    w.integer(s.diagnostics.lostEvents, 8);
    w.integer(s.diagnostics.lostBuffers, 8);
    w.integer(s.diagnostics.unresolvedIds, 8);
    w.integer(s.diagnostics.unsupportedSchemas, 8);
    w.integer(s.diagnostics.truncated, 1);
    w.attribution(s.diagnostics.attributionTotal);
    w.attribution(s.diagnostics.attributionInterval);
    return w.finish();
}
ValueDecodeResult<Snapshot> decodeSnapshot(std::span<const std::byte> bytes)
{
    if (bytes.size() > MaxPayload)
        return {DecodeError::TooLarge};
    Reader r{bytes};
    Snapshot s;
    s.sequence = r.integer(8);
    s.sampleQpc = r.integer(8);
    s.qpcFrequency = r.integer(8);
    s.intervalQpc = r.integer(8);
    s.disk = r.source();
    s.network = r.source();
    s.temperatures = r.source();
    auto n = r.integer(4);
    if (!r.ok || n > 32768 || n > (bytes.size() - r.pos) / 28)
        return {DecodeError::Malformed};
    s.processes.reserve(size_t(n));
    for (uint64_t i = 0; i < n; ++i)
    {
        IoRate p;
        p.key.pid = uint32_t(r.integer(4));
        p.key.creationTime100ns = r.integer(8);
        p.diskMBps = r.real();
        p.networkMBps = r.real();
        if (!validRate(p.diskMBps) || !validRate(p.networkMBps))
            r.ok = false;
        s.processes.push_back(p);
    }
    n = r.integer(4);
    if (!r.ok || n > 4096 || n > (bytes.size() - r.pos) / 22)
        return {DecodeError::Malformed};
    s.sensors.reserve(size_t(n));
    for (uint64_t i = 0; i < n; ++i)
    {
        SensorReading t;
        t.sensorId = uint32_t(r.integer(4));
        t.scope = SensorScope(r.integer(1));
        t.packageId = uint32_t(r.integer(4));
        t.coreId = uint32_t(r.integer(4));
        t.celsius = r.real();
        t.valid = r.boolean();
        if (t.scope > SensorScope::Package)
            r.ok = false;
        s.sensors.push_back(t);
    }
    s.diagnostics.lostEvents = r.integer(8);
    s.diagnostics.lostBuffers = r.integer(8);
    s.diagnostics.unresolvedIds = r.integer(8);
    s.diagnostics.unsupportedSchemas = r.integer(8);
    s.diagnostics.truncated = r.boolean();
    s.diagnostics.attributionTotal = r.attribution();
    s.diagnostics.attributionInterval = r.attribution();
    if (!r.done())
        return {DecodeError::Malformed};
    return {DecodeError::None, std::move(s)};
}
} // namespace monitoring
