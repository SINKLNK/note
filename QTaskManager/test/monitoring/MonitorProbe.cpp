#include "monitoring/win/PipeIo.h"
#include "monitoring/win/ServiceControl.h"
#include <iostream>
#include <string>
using namespace monitoring;
namespace
{
void printAttribution(const AttributionCounters &counters)
{
    static const std::array<const char *, size_t(AttributionIssue::Count)> reasons = {
        "diskMissingThreadId", "diskUnknownThread", "networkMissingProcessId", "networkUnknownProcess",
        "processOpenFailed", "processTimesFailed", "processNewerThanEvent", "threadUnknownProcess"};
    std::cout << "{\"diskEvents\":" << counters.diskEvents << ",\"networkEvents\":" << counters.networkEvents
              << ",\"issues\":[";
    for (size_t i = 0; i < counters.issues.size(); ++i)
    {
        if (i)
            std::cout << ',';
        const auto &count = counters.issues[i];
        std::cout << "{\"reason\":\"" << reasons[i] << "\",\"events\":" << count.events
                  << ",\"bytes\":" << count.bytes << ",\"lastPid\":" << count.lastPid
                  << ",\"lastTid\":" << count.lastTid << ",\"lastError\":" << count.lastError << '}';
    }
    std::cout << "]}";
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    unsigned seconds = 10;
    if (argc == 2)
    {
        wchar_t *end = nullptr;
        auto n = wcstoul(argv[1], &end, 10);
        if (!end || *end || n < 1 || n > 3600)
            return 2;
        seconds = unsigned(n);
    }
    else if (argc != 1)
        return 2;
    auto service = queryService();
    if (service.state != ServiceState::Running)
    {
        std::cerr << "Service unavailable: " << service.error << '\n';
        return 3;
    }
    WinHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    WinHandle pipe(CreateFileW(
        PipeName, GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    ULONG pid{};
    if (!pipe)
    {
        std::cerr << "Open service pipe failed: " << GetLastError() << '\n';
        return 4;
    }
    if (!GetNamedPipeServerProcessId(pipe.get(), &pid))
    {
        std::cerr << "Query pipe server PID failed: " << GetLastError() << '\n';
        return 4;
    }
    if (pid != service.pid)
    {
        std::cerr << "Pipe server PID differs from SCM: " << pid << " != " << service.pid << '\n';
        return 4;
    }
    uint64_t request = 0;
    Frame answer;
    auto exchange = [&](MessageKind kind) {
        return writeFrame(pipe.get(), stop.get(), {kind, ++request, {}}, 2000) &&
               readFrame(pipe.get(), stop.get(), answer, 3000) && answer.requestId == request;
    };
    if (!exchange(MessageKind::Hello) || answer.kind != MessageKind::HelloReply)
        return 5;
    auto status = decodeStatus(answer.payload);
    if (!status.value || status.value->protocolVersion != ProtocolVersion)
        return 6;
    unsigned received = 0;
    uint64_t last = 0;
    for (unsigned i = 0; i < seconds; ++i)
    {
        if (!exchange(MessageKind::GetSnapshot))
            return 7;
        if (answer.kind == MessageKind::SnapshotReply)
        {
            auto s = decodeSnapshot(answer.payload);
            if (!s.value)
                return 8;
            if (s.value->sequence != last)
            {
                ++received;
                last = s.value->sequence;
                std::cout << "{\"sequence\":" << s.value->sequence << ",\"qpc\":" << s.value->sampleQpc
                          << ",\"diskMetric\":\"fileRequests\""
                          << ",\"frequency\":" << s.value->qpcFrequency
                          << ",\"interval\":" << s.value->intervalQpc
                          << ",\"diskState\":" << int(s.value->disk.state)
                          << ",\"networkState\":" << int(s.value->network.state)
                          << ",\"lostEvents\":" << s.value->diagnostics.lostEvents
                          << ",\"lostBuffers\":" << s.value->diagnostics.lostBuffers
                          << ",\"unresolved\":" << s.value->diagnostics.unresolvedIds
                          << ",\"schemas\":" << s.value->diagnostics.unsupportedSchemas
                          << ",\"attributionTotal\":";
                printAttribution(s.value->diagnostics.attributionTotal);
                std::cout << ",\"attributionInterval\":";
                printAttribution(s.value->diagnostics.attributionInterval);
                std::cout << ",\"processes\":[";
                bool first = true;
                for (auto &p : s.value->processes)
                {
                    if (!first)
                        std::cout << ',';
                    first = false;
                    std::cout << "{\"pid\":" << p.key.pid << ",\"birth\":" << p.key.creationTime100ns
                              << ",\"diskMBps\":" << p.diskMBps << ",\"networkMBps\":" << p.networkMBps
                              << '}';
                }
                std::cout << "]}" << std::endl;
            }
        }
        Sleep(1000);
    }
    return received ? 0 : 9;
}
