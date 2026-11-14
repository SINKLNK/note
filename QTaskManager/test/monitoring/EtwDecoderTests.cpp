#include "TestSupport.h"
#include "service/EtwDecoder.h"
#include "service/EventAttribution.h"
#include <cstring>
using namespace monitoring;
int main()
{
    for (auto guid : {TcpGuid, UdpGuid})
        for (unsigned op : {10, 11, 26, 27})
            for (unsigned flags : {EVENT_HEADER_FLAG_32_BIT_HEADER, EVENT_HEADER_FLAG_64_BIT_HEADER})
            {
                EVENT_RECORD r{};
                uint32_t data[]{77, 12345};
                r.EventHeader.ProviderId = guid;
                r.EventHeader.EventDescriptor.Opcode = UCHAR(op);
                r.EventHeader.EventDescriptor.Version = 2;
                r.EventHeader.Flags = USHORT(flags);
                r.UserData = data;
                r.UserDataLength = sizeof(data);
                EtwDecoder d([](const EVENT_RECORD &, const wchar_t *n) -> std::optional<uint32_t> {
                    return wcscmp(n, L"PID") == 0 ? 77 : 12345;
                });
                auto e = d.decode(r);
                CHECK(e);
                CHECK(e->pid == 77);
                CHECK(e->bytes == 12345);
                r.UserDataLength = 4;
                CHECK(!d.decode(r));
            }
    // FileIo v2 and v3 order their pointer-sized fields differently.
    // Use independently specified layouts, including header IDs from a different process.
    struct FileLayout { unsigned flags, version, tidOffset, sizeOffset; };
    for (auto layout : {FileLayout{EVENT_HEADER_FLAG_32_BIT_HEADER, 2, 12, 24},
                        FileLayout{EVENT_HEADER_FLAG_64_BIT_HEADER, 2, 16, 40},
                        FileLayout{EVENT_HEADER_FLAG_32_BIT_HEADER, 3, 20, 24},
                        FileLayout{EVENT_HEADER_FLAG_64_BIT_HEADER, 3, 32, 36}})
        for (unsigned op : {67, 68})
        {
            unsigned char data[64]{};
            uint32_t size = 500, tid = 90;
            memcpy(data + layout.sizeOffset, &size, 4);
            memcpy(data + layout.tidOffset, &tid, 4);
            EVENT_RECORD r{};
            r.EventHeader.ProviderId = {0x90cbdc39, 0x4a3e, 0x11d1,
                                       {0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3}};
            r.EventHeader.EventDescriptor.Opcode = UCHAR(op);
            r.EventHeader.EventDescriptor.Version = UCHAR(layout.version);
            r.EventHeader.Flags = USHORT(layout.flags);
            r.EventHeader.ProcessId = 4;
            r.EventHeader.ThreadId = 7;
            r.UserData = data;
            r.UserDataLength = 64;
            r.EventHeader.TimeStamp.QuadPart = 1000;
            EtwDecoder d([](const EVENT_RECORD &, const wchar_t *n) -> std::optional<uint32_t> {
                if (wcscmp(n, L"IoSize") == 0) return 500;
                if (wcscmp(n, L"TTID") == 0) return 90;
                return {};
            });
            auto decoded = d.decode(r);
            CHECK(decoded);
            CHECK(decoded->tid == 90);
            CHECK(decoded->qpc == 1000);
            CHECK(decoded->bytes == 500);
            CHECK(decoded->kind == (op == 67 ? EventKind::DiskRead : EventKind::DiskWrite));
            ProcessIdentity ids;
            ids.processStart({4, 10}, 800);
            ids.threadStart(7, {4, 10}, 800);
            ids.processStart({42, 20}, 800);
            ids.threadStart(90, {42, 20}, 800);
            IoAccumulator totals;
            totals.sample(900, 1000);
            EventAttribution attribution([](uint32_t, uint64_t) { return ProcessLookupResult{}; });
            attribution.consume(*decoded, ids, totals);
            auto rates = totals.sample(1900, 1000);
            CHECK(rates.size() == 1);
            CHECK(rates[0].key.pid == 42); // TTID owns the request, not the header's System context.
            CHECK_NEAR(rates[0].diskMBps, 0.0005, 1e-12);
            auto fileProvider = r.EventHeader.ProviderId;
            r.EventHeader.ProviderId = {0x3d6fa8d4, 0xfe05, 0x11d0,
                                       {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
            r.EventHeader.EventDescriptor.Opcode = 10;
            CHECK(!d.decode(r)); // Never add physical completions to logical file request bytes.
            CHECK(d.diskStatus.state == SourceState::Ready);
            r.EventHeader.ProviderId = fileProvider;
            r.EventHeader.EventDescriptor.Opcode = UCHAR(op);
            EtwDecoder bad(
                [](const EVENT_RECORD &, const wchar_t *) -> std::optional<uint32_t> { return 999; });
            CHECK(!bad.decode(r));
            CHECK(bad.diskStatus.state == SourceState::Failed);
            CHECK(bad.networkStatus.state == SourceState::Ready);
            r.UserDataLength = USHORT(layout.sizeOffset + 3);
            CHECK(!d.decode(r));
            r.UserDataLength = 64;
            r.EventHeader.EventDescriptor.Opcode = 64; // Create events are not byte throughput.
            EtwDecoder ignored;
            CHECK(!ignored.decode(r));
            CHECK(ignored.diskStatus.state == SourceState::Ready);
            r.EventHeader.EventDescriptor.Opcode = UCHAR(op);
            r.EventHeader.EventDescriptor.Version = 4;
            CHECK(!ignored.decode(r));
            CHECK(ignored.diskStatus.state == SourceState::Failed);
        }
}
