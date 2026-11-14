#include "TestSupport.h"
#include "monitoring/WireCodec.h"
#include <limits>
using namespace monitoring;
int main()
{
    Snapshot s;
    s.sequence = 9;
    s.sampleQpc = 100;
    s.qpcFrequency = 1000;
    s.intervalQpc = 10;
    s.disk.state = SourceState::Ready;
    s.processes.push_back({{123, 456789}, 2.5, 0});
    s.diagnostics.unresolvedIds = 19;
    s.diagnostics.attributionTotal.diskEvents = 100;
    s.diagnostics.attributionTotal.networkEvents = 200;
    s.diagnostics.attributionInterval.diskEvents = 10;
    s.diagnostics.attributionInterval.networkEvents = 20;
    for (size_t i = 0; i < size_t(AttributionIssue::Count); ++i)
    {
        s.diagnostics.attributionTotal.issues[i] = {i + 1, i + 1000, uint32_t(i + 10),
                                                   uint32_t(i + 20), uint32_t(i + 30)};
        s.diagnostics.attributionInterval.issues[i] = {i + 50, i + 2000, uint32_t(i + 40),
                                                      uint32_t(i + 50), uint32_t(i + 60)};
    }
    auto bytes = encodeSnapshot(s);
    auto d = decodeSnapshot(bytes);
    CHECK(d.value);
    CHECK(d.value->processes[0].key.creationTime100ns == 456789);
    CHECK_NEAR(d.value->processes[0].diskMBps, 2.5, 1e-9);
    CHECK(d.value->diagnostics.unresolvedIds == 19);
    CHECK(d.value->diagnostics.attributionTotal.diskEvents == 100);
    CHECK(d.value->diagnostics.attributionInterval.networkEvents == 20);
    CHECK(d.value->diagnostics.attributionTotal == s.diagnostics.attributionTotal);
    CHECK(d.value->diagnostics.attributionInterval == s.diagnostics.attributionInterval);
    Frame f{MessageKind::SnapshotReply, 7, bytes};
    auto frame = encodeFrame(f);
    CHECK(decodeFrame(std::span(frame).first(5)).error == DecodeError::NeedMore);
    CHECK(decodeFrame(std::span(frame).first(frame.size() - 1)).error == DecodeError::NeedMore);
    CHECK(decodeFrame(frame).frame->requestId == 7);
    frame[4] = std::byte{ProtocolVersion + 1};
    CHECK(decodeFrame(frame).error == DecodeError::UnsupportedVersion);
    frame = encodeFrame(f);
    frame[8] = frame[9] = frame[10] = frame[11] = std::byte{255};
    CHECK(decodeFrame(frame).error == DecodeError::TooLarge);
    frame = encodeFrame(f);
    frame[6] = std::byte{255};
    CHECK(decodeFrame(frame).error == DecodeError::Malformed);
    bytes.pop_back();
    CHECK(!decodeSnapshot(bytes).value);
    s.processes[0].diskMBps = std::numeric_limits<double>::infinity();
    CHECK(encodeSnapshot(s).empty());
    s.processes[0].diskMBps = std::numeric_limits<double>::quiet_NaN();
    CHECK(encodeSnapshot(s).empty());
    s.processes.clear();
    s.processes.resize(32769);
    CHECK(encodeSnapshot(s).empty());
    Status status;
    status.serviceVersion = "1.0";
    CHECK(decodeStatus(encodeStatus(status)).value->serviceVersion == "1.0");
    status.disk.state = static_cast<SourceState>(99);
    CHECK(encodeStatus(status).empty());
    status.disk.state = SourceState::Failed;
    status.disk.detail = std::string(4097, 'x');
    CHECK(encodeStatus(status).empty());
}
