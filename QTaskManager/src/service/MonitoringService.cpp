#include "MonitoringService.h"
#include "ServiceRuntime.h"
#include "monitoring/win/ServiceConfig.h"
namespace monitoring
{
MonitoringService::MonitoringService() : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), pipe_(leases_)
{
}
void MonitoringService::requestStop()
{
    SetEvent(stop_.get());
}
uint32_t MonitoringService::run(const std::function<void()> &ready)
{
    if (!stop_)
        return GetLastError();
    status_.temperatures = temperatures_.start();
    auto started = initializeServiceEndpoint(
        [this] {
            return pipe_.start([this](const Frame &request) {
                std::lock_guard lock(statusMutex_);
                return Frame{request.kind == MessageKind::Hello ? MessageKind::HelloReply
                                                                : MessageKind::StatusReply,
                             request.requestId, encodeStatus(status_)};
            });
        },
        ready);
    if (started.state == SourceState::Failed)
        return started.win32Error;
    CollectionController controller([this] { return etw_.start(); }, [this] { etw_.stop(); });
    uint64_t next = 0;
    uint64_t cleanupRetry = 0;
    while (WaitForSingleObject(stop_.get(), 100) == WAIT_TIMEOUT)
    {
        auto now = GetTickCount64();
        leases_.expire(now);
        if (resume_.exchange(false))
        {
            controller.stop();
            pipe_.publish(nullptr);
        }
        controller.tick(leases_.shouldCollect(now), now);
        if (!controller.active() && etw_.cleanupPending() && now >= cleanupRetry) {
            etw_.stop();
            cleanupRetry = GetTickCount64() + 5000;
        }
        if (now >= next)
        {
            next = now + 1000;
            Snapshot s;
            if (controller.active())
                s = etw_.snapshot(qpcNow());
            else
            {
                s.sampleQpc = qpcNow();
                s.qpcFrequency = qpcFrequency();
                s.disk = s.network = etw_.cleanupPending() ? etw_.teardownStatus() : controller.status();
            }
            s.temperatures = temperatures_.start();
            if (controller.active() && s.disk.state == SourceState::Failed &&
                s.network.state == SourceState::Failed)
                controller.fault(s.disk, now);
            {
                std::lock_guard lock(statusMutex_);
                status_.disk = s.disk;
                status_.network = s.network;
                status_.temperatures = s.temperatures;
                status_.temperatureEnabled = false;
            }
            pipe_.publish(leases_.shouldCollect(now) ? std::make_shared<const Snapshot>(std::move(s))
                                                     : nullptr);
        }
    }
    pipe_.stop();
    controller.stop();
    temperatures_.stop();
    return etw_.cleanupPending() ? etw_.teardownStatus().win32Error : ERROR_SUCCESS;
}
} // namespace monitoring
