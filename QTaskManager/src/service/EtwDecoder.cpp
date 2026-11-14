#include "EtwDecoder.h"
#include <cstring>
#include <tdh.h>
namespace monitoring
{
namespace
{
std::optional<uint64_t> readProperty(const EVENT_RECORD &record, const wchar_t *name)
{
    PROPERTY_DATA_DESCRIPTOR p{};
    p.PropertyName = reinterpret_cast<ULONGLONG>(name);
    p.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    auto *r = const_cast<EVENT_RECORD *>(&record);
    uint64_t value{};
    if (TdhGetPropertySize(r, 0, nullptr, 1, &p, &size) != ERROR_SUCCESS || (size != 4 && size != 8) ||
        TdhGetProperty(r, 0, nullptr, 1, &p, size, reinterpret_cast<PBYTE>(&value)) != ERROR_SUCCESS)
        return {};
    return value;
}
} // namespace
EtwDecoder::EtwDecoder(PropertyReader reader) : reader_(reader ? std::move(reader) : readProperty)
{
}
std::optional<DecodedEvent> EtwDecoder::decode(const EVENT_RECORD &r)
{
    auto guid = r.EventHeader.ProviderId;
    unsigned provider = IsEqualGUID(guid, FileIoGuid)    ? 1
                        : IsEqualGUID(guid, TcpGuid)     ? 2
                        : IsEqualGUID(guid, UdpGuid)     ? 3
                        : IsEqualGUID(guid, ThreadGuid)  ? 4
                        : IsEqualGUID(guid, ProcessGuid) ? 5
                                                         : 0;
    if (!provider)
        return {};
    unsigned op = r.EventHeader.EventDescriptor.Opcode, v = r.EventHeader.EventDescriptor.Version;
    bool x32 = (r.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0;
    if (provider == 1 && op != 67 && op != 68)
        return {};
    if ((provider == 2 || provider == 3) && op != 10 && op != 11 && op != 26 && op != 27)
        return {};
    if (provider >= 4 && (op < 1 || op > 4))
        return {};
    auto fail = [&]() -> std::optional<DecodedEvent> {
        ++unsupported;
        if (provider == 1 || provider >= 4)
            diskStatus = {SourceState::Failed, ERROR_NOT_SUPPORTED, "Unsupported ETW schema"};
        if (provider == 2 || provider == 3 || provider == 5)
            networkStatus = {SourceState::Failed, ERROR_NOT_SUPPORTED, "Unsupported ETW schema"};
        return {};
    };
    size_t a = 0, b = 4;
    const wchar_t *an = L"PID";
    const wchar_t *bn = L"size";
    if (provider == 1)
    {
        if (v != 2 && v != 3)
            return fail();
        // v2 TTID is pointer-sized; v3 moved it after the three object pointers.
        // IoSize is the requested logical byte count, not a physical disk transfer.
        a = v == 2 ? (x32 ? 24 : 40) : (x32 ? 24 : 36);
        b = v == 2 ? (x32 ? 12 : 16) : (x32 ? 20 : 32);
        an = L"IoSize";
        bn = L"TTID";
    }
    if (provider == 4)
    {
        a = v == 0 ? 4 : 0;
        b = v == 0 ? 0 : 4;
        an = L"ProcessId";
        bn = L"TThreadId";
    }
    if (provider == 5)
    {
        if (v < 2)
            return fail();
        a = x32 ? 4 : 8;
        b = a;
        an = bn = L"ProcessId";
    }
    if (!r.UserData || r.UserDataLength < (std::max)(a, b) + 4)
        return fail();
    uint32_t av, bv;
    auto *data = static_cast<const char *>(r.UserData);
    memcpy(&av, data + a, 4);
    memcpy(&bv, data + b, 4);
    auto key = std::tuple(provider, v, op, x32);
    auto it = schemas_.find(key);
    if (it == schemas_.end())
    {
        auto ta = reader_(r, an), tb = reader_(r, bn);
        bool good = ta && tb && *ta == av && *tb == bv;
        it = schemas_.emplace(key, good).first;
    }
    if (!it->second)
        return fail();
    DecodedEvent e{};
    e.qpc = r.EventHeader.TimeStamp.QuadPart;
    if (provider == 1)
    {
        // Use the request's TTID and timestamp, even if the event header is in System's context.
        e.kind = op == 67 ? EventKind::DiskRead : EventKind::DiskWrite;
        e.tid = bv;
        e.bytes = av;
    }
    else if (provider <= 3)
    {
        e.kind = (op == 10 || op == 26) ? EventKind::NetSend : EventKind::NetReceive;
        e.pid = av;
        e.bytes = bv;
    }
    else
    {
        bool start = op == 1 || op == 3;
        e.kind = provider == 4 ? (start ? EventKind::ThreadStart : EventKind::ThreadEnd)
                               : (start ? EventKind::ProcessStart : EventKind::ProcessEnd);
        e.pid = av;
        e.tid = provider == 4 ? bv : 0;
    }
    return e;
}
} // namespace monitoring
