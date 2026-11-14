#include "MonitoringClient.h"
#include "monitoring/win/PipeIo.h"
#include <QTimer>
using namespace monitoring;
MonitoringClient::MonitoringClient(QObject *parent)
    : QObject(parent), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    qRegisterMetaType<Snapshot>();
    qRegisterMetaType<Status>();
    qRegisterMetaType<ServiceInfo>();
    deliveryTimer_ = new QTimer(this);
    deliveryTimer_->setInterval(500);
    connect(deliveryTimer_, &QTimer::timeout, this, [this] {
        auto latest = delivery_.take(GetTickCount64());
        if (latest.service)
            emit serviceChanged(*latest.service);
        if (latest.status)
            emit statusChanged(*latest.status);
        if (latest.connected)
            emit connectionChanged(*latest.connected);
        if (latest.error)
            emit connectionError(QString::fromStdString(*latest.error));
        if (latest.snapshot)
            emit snapshotReady(*latest.snapshot);
    });
}
MonitoringClient::~MonitoringClient()
{
    stop();
}
void MonitoringClient::start()
{
    if (!worker_.joinable() && stop_)
    {
        ResetEvent(stop_.get());
        delivery_.take(GetTickCount64());
        deliveryTimer_->start();
        worker_ = std::thread([this] { run(); });
    }
}
void MonitoringClient::stop()
{
    deliveryTimer_->stop();
    if (stop_)
        SetEvent(stop_.get());
    if (worker_.joinable())
        worker_.join();
}
void MonitoringClient::run()
{
    unsigned failures = 0;
    uint64_t requestId = 0;
    while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
    {
        if (!delivery_.responsive(GetTickCount64()))
        {
            if (WaitForSingleObject(stop_.get(), 1000) == WAIT_OBJECT_0)
                break;
            continue;
        }
        auto info = queryService();
        delivery_.service(info);
        bool connected = false;
        if (info.state == ServiceState::Running)
        {
            WinHandle pipe(CreateFileW(
                PipeName, GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            ULONG pid = 0;
            if (pipe && GetNamedPipeServerProcessId(pipe.get(), &pid) && pid == info.pid &&
                pid == queryService().pid)
            {
                auto exchange = [&](MessageKind kind, Frame &answer) {
                    if (!delivery_.responsive(GetTickCount64())) return false;
                    auto id = ++requestId;
                    return writeFrame(pipe.get(), stop_.get(), {kind, id, {}}, 2000) &&
                           readFrame(pipe.get(), stop_.get(), answer, 3000) && answer.requestId == id;
                };
                Frame f;
                if (exchange(MessageKind::Hello, f) && f.kind == MessageKind::HelloReply)
                {
                    auto status = decodeStatus(f.payload);
                    if (status.value && status.value->protocolVersion == ProtocolVersion)
                    {
                        connected = true;
                        failures = 0;
                        delivery_.status(*status.value);
                        delivery_.connection(true);
                        delivery_.error({});
                    }
                }
                if (!connected)
                    delivery_.error(
                        "Не удалось согласовать протокол службы. Возможно, требуется обновление.");
                uint64_t lastSequence = 0;
                while (connected && WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
                {
                    if (!delivery_.responsive(GetTickCount64()))
                        break;
                    if (!exchange(MessageKind::GetStatus, f) || f.kind != MessageKind::StatusReply)
                        break;
                    auto status = decodeStatus(f.payload);
                    if (!status.value)
                        break;
                    delivery_.status(*status.value);
                    delivery_.service(queryService());
                    if (!exchange(MessageKind::GetSnapshot, f))
                        break;
                    if (f.kind == MessageKind::SnapshotReply)
                    {
                        auto s = decodeSnapshot(f.payload);
                        if (!s.value)
                            break;
                        if (s.value->sequence > lastSequence)
                        {
                            lastSequence = s.value->sequence;
                            delivery_.snapshot(std::move(*s.value));
                        }
                    }
                    else if (f.kind != MessageKind::Error)
                        break;
                    // Requests renew the same 15-second lease as explicit heartbeats.
                    if (WaitForSingleObject(stop_.get(), 1000) == WAIT_OBJECT_0)
                        break;
                }
            }
            else
                delivery_.error("Канал службы недоступен или PID сервера не совпал с SCM.");
        }
        delivery_.connection(false);
        DWORD delay = failures == 0 ? 1000 : failures == 1 ? 2000 : 5000;
        ++failures;
        if (WaitForSingleObject(stop_.get(), delay) == WAIT_OBJECT_0)
            break;
    }
}
