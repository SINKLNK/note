#include "TestSupport.h"
#include "service/PipeServer.h"
using namespace monitoring;
int main()
{
    ClientLeases leases;
    auto name = L"\\\\.\\pipe\\QTaskManager.Test." + std::to_wstring(GetCurrentProcessId());
    PipeServer server(leases, name, [](HANDLE) { return true; }, L"D:P(A;;GA;;;OW)");
    CHECK(server
              .start([](const Frame &f) {
                  return Frame{f.kind == MessageKind::Hello ? MessageKind::HelloReply
                                                            : MessageKind::StatusReply,
                               f.requestId, encodeStatus({})};
              })
              .state == SourceState::Ready);
    WinHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    WinHandle client(CreateFileW(
        name.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    CHECK(client);
    Frame f;
    auto bytes = encodeFrame({MessageKind::Hello, 42, {}});
    CHECK(pipeTransfer(client.get(), stop.get(), bytes.data(), 7, true, 1000));
    CHECK(pipeTransfer(client.get(), stop.get(), bytes.data() + 7, bytes.size() - 7, true, 1000));
    CHECK(readFrame(client.get(), stop.get(), f, 1000));
    CHECK(f.kind == MessageKind::HelloReply);
    CHECK(f.requestId == 42);
    CHECK(leases.count() == 1);
    auto s = std::make_shared<Snapshot>();
    s->sequence = 10;
    server.publish(s);
    s = std::make_shared<Snapshot>();
    s->sequence = 11;
    server.publish(s);
    CHECK(writeFrame(client.get(), stop.get(), {MessageKind::GetSnapshot, 43, {}}, 1000));
    CHECK(readFrame(client.get(), stop.get(), f, 1000));
    CHECK(decodeSnapshot(f.payload).value->sequence == 11);
    auto start = GetTickCount64();
    server.stop();
    CHECK(GetTickCount64() - start < 2000);
    CHECK(leases.count() == 0);
    client.reset();
    PipeServer denied(leases, name, [](HANDLE) { return false; }, L"D:P(A;;GA;;;OW)");
    CHECK(denied.start([](const Frame &) { return Frame{}; }).state == SourceState::Ready);
    client.reset(CreateFileW(name.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    CHECK(client);
    CHECK(writeFrame(client.get(), stop.get(), {MessageKind::Hello, 1, {}}, 1000));
    CHECK(!readFrame(client.get(), stop.get(), f, 1000));
    CHECK(leases.count() == 0);
    denied.stop();
}
