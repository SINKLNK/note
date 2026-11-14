#include "TestSupport.h"
#include "service/EventAttribution.h"
#include <cstring>
using namespace monitoring;
int main()
{
    ProcessIdentity ids;
    IoAccumulator totals;
    EventAttribution attribution([](uint32_t pid, uint64_t) -> ProcessLookupResult {
        if (pid == 90)
            return {{}, AttributionIssue::ProcessOpenFailed, 5};
        if (pid == 91)
            return {{}, AttributionIssue::ProcessTimesFailed, 6};
        if (pid == 92)
            return {{}, AttributionIssue::ProcessNewerThanEvent, 0};
        return {ProcessKey{pid, 123}};
    });
    auto consume = [&](EventKind kind, uint32_t pid, uint32_t tid, uint32_t bytes = 0) {
        attribution.consume({kind, 200, pid, tid, bytes}, ids, totals);
    };
    auto sample = [&](SourceState diskState = SourceState::Ready,
                      SourceState networkState = SourceState::Ready) {
        Snapshot s;
        s.disk.state = diskState;
        s.network.state = networkState;
        attribution.finishInterval(s.diagnostics, s.disk, s.network);
        return s;
    };
    auto count = [](const AttributionCounters &c, AttributionIssue issue) -> const AttributionCount & {
        return c.issues[size_t(issue)];
    };
    // Missing process/thread lifecycle identities alone must not mark unrelated I/O partial.
    consume(EventKind::ProcessStart, 90, 0);
    consume(EventKind::ProcessStart, 91, 0);
    consume(EventKind::ProcessStart, 92, 0);
    consume(EventKind::ThreadStart, 90, 80);
    auto s = sample();
    CHECK(s.disk.state == SourceState::Ready);
    CHECK(s.network.state == SourceState::Ready);
    CHECK(s.diagnostics.unresolvedIds == 4);
    CHECK(count(s.diagnostics.attributionTotal, AttributionIssue::ProcessOpenFailed).lastError == 5);
    CHECK(count(s.diagnostics.attributionTotal, AttributionIssue::ProcessTimesFailed).lastError == 6);
    CHECK(count(s.diagnostics.attributionTotal, AttributionIssue::ProcessNewerThanEvent).events == 1);
    CHECK(count(s.diagnostics.attributionTotal, AttributionIssue::ThreadUnknownProcess).lastTid == 80);
    // A disk attribution failure must not mark networking partial or discard valid traffic.
    consume(EventKind::ProcessStart, 42, 0);
    consume(EventKind::ThreadStart, 42, 7);
    totals.sample(100, 1000);
    consume(EventKind::DiskRead, 0, 99, 1000000);
    consume(EventKind::DiskWrite, 0, 0, 2000000);
    consume(EventKind::NetReceive, 42, 0, 3000000);
    s = sample();
    CHECK(s.disk.state == SourceState::Partial);
    CHECK(s.network.state == SourceState::Ready);
    CHECK(s.diagnostics.attributionInterval.diskEvents == 2);
    CHECK(s.diagnostics.attributionInterval.networkEvents == 1);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::DiskUnknownThread).bytes == 1000000);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::DiskUnknownThread).lastTid == 99);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::DiskMissingThreadId).bytes == 2000000);
    auto rates = totals.sample(1100, 1000);
    CHECK(rates.size() == 1);
    CHECK(rates[0].key.pid == 42);
    CHECK_NEAR(rates[0].networkMBps, 3.0, 1e-9);
    // Network failures are independent and retain both missing PID and unknown PID reasons.
    consume(EventKind::NetSend, 55, 0, 4000);
    consume(EventKind::NetReceive, 0, 0, 5000);
    s = sample();
    CHECK(s.disk.state == SourceState::Ready);
    CHECK(s.network.state == SourceState::Partial);
    CHECK(s.diagnostics.attributionInterval.diskEvents == 0);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::NetworkUnknownProcess).lastPid == 55);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::NetworkMissingProcessId).bytes == 5000);
    CHECK(s.diagnostics.unresolvedIds == 8);
    // A clean interval recovers quality, retaining session totals and clearing interval examples.
    s = sample();
    CHECK(s.disk.state == SourceState::Ready);
    CHECK(s.network.state == SourceState::Ready);
    CHECK(s.diagnostics.attributionTotal.diskEvents == 2);
    CHECK(s.diagnostics.attributionTotal.networkEvents == 3);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::NetworkUnknownProcess).events == 0);
    CHECK(count(s.diagnostics.attributionInterval, AttributionIssue::NetworkUnknownProcess).lastPid == 0);
    CHECK(count(s.diagnostics.attributionTotal, AttributionIssue::NetworkUnknownProcess).bytes == 4000);
    // Diagnostics must never promote a failed or warming-up source into a valid one.
    consume(EventKind::DiskRead, 0, 99, 100);
    consume(EventKind::NetSend, 55, 0, 200);
    s = sample(SourceState::Failed, SourceState::WarmingUp);
    CHECK(s.disk.state == SourceState::Failed);
    CHECK(s.network.state == SourceState::WarmingUp);
    attribution.reset();
    s = sample();
    CHECK(s.diagnostics.unresolvedIds == 0);
    CHECK(s.diagnostics.attributionTotal.networkEvents == 0);
    // Verify the real decoder-to-diagnostics boundary: process events carry no thread ID.
    for (bool x32 : {false, true})
    {
        uint8_t payload[16]{};
        uint32_t pid = 90;
        memcpy(payload + (x32 ? 4 : 8), &pid, 4);
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = ProcessGuid;
        record.EventHeader.EventDescriptor.Version = 2;
        record.EventHeader.EventDescriptor.Opcode = 1;
        record.EventHeader.Flags = x32 ? EVENT_HEADER_FLAG_32_BIT_HEADER : EVENT_HEADER_FLAG_64_BIT_HEADER;
        record.EventHeader.TimeStamp.QuadPart = 200;
        record.UserData = payload;
        record.UserDataLength = sizeof(payload);
        EtwDecoder decoder([](const EVENT_RECORD &, const wchar_t *) -> std::optional<uint64_t> { return 90; });
        auto decoded = decoder.decode(record);
        CHECK(decoded);
        attribution.consume(*decoded, ids, totals);
        s = sample();
        const auto &issue = count(s.diagnostics.attributionTotal, AttributionIssue::ProcessOpenFailed);
        CHECK(issue.lastPid == 90);
        CHECK(issue.lastTid == 0);
        attribution.reset();
    }
    // End routing must retain historical TID ownership for late disk completions after TID reuse.
    ids = {};
    totals.reset();
    totals.sample(100, 1000);
    auto at = [&](EventKind kind, uint64_t qpc, uint32_t pid, uint32_t tid, uint32_t bytes = 0) {
        attribution.consume({kind, qpc, pid, tid, bytes}, ids, totals);
    };
    at(EventKind::ProcessStart, 100, 10, 0);
    at(EventKind::ThreadStart, 110, 10, 7);
    at(EventKind::ThreadEnd, 130, 10, 7);
    at(EventKind::ProcessEnd, 140, 10, 0);
    CHECK(!ids.resolvePid(10, 145));
    CHECK(!ids.resolveTid(7, 145));
    at(EventKind::ProcessStart, 150, 20, 0);
    at(EventKind::ThreadStart, 160, 20, 7);
    at(EventKind::DiskRead, 120, 0, 7, 1000000);
    at(EventKind::DiskWrite, 170, 0, 7, 2000000);
    auto routed = totals.sample(1100, 1000);
    CHECK(routed.size() == 2);
    CHECK(routed[0].key.pid == 10);
    CHECK_NEAR(routed[0].diskMBps, 1.0, 1e-9);
    CHECK(routed[1].key.pid == 20);
    CHECK_NEAR(routed[1].diskMBps, 2.0, 1e-9);
    s = sample();
    CHECK(s.diagnostics.unresolvedIds == 0);
}
