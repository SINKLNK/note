#include "MonitoringService.h"
#include <atomic>
using namespace monitoring;
namespace
{
SERVICE_STATUS_HANDLE statusHandle;
SERVICE_STATUS status{};
std::atomic<MonitoringService *> service{};
std::mutex controlMutex;
PTP_TIMER shutdownTimer{};
void report(DWORD state, DWORD error = 0)
{
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT
                                 : 0;
    status.dwWaitHint = state == SERVICE_STOP_PENDING ? 10000 : 0;
    SetServiceStatus(statusHandle, &status);
}
DWORD WINAPI control(DWORD code, DWORD event, void *, void *)
{
    std::lock_guard lock(controlMutex);
    auto *s = service.load();
    if (s && (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN))
    {
        report(SERVICE_STOP_PENDING);
        FILETIME due{};
        ULARGE_INTEGER ticks;
        ticks.QuadPart = uint64_t(-100000000LL);
        due.dwLowDateTime = ticks.LowPart;
        due.dwHighDateTime = ticks.HighPart;
        if (shutdownTimer)
            SetThreadpoolTimer(shutdownTimer, &due, 0, 0);
        s->requestStop();
    }
    else if (s && code == SERVICE_CONTROL_POWEREVENT &&
             (event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND))
        s->resume();
    return NO_ERROR;
}
void WINAPI entry(DWORD, wchar_t **)
{
    statusHandle = RegisterServiceCtrlHandlerExW(ServiceName, control, nullptr);
    if (!statusHandle)
        return;
    report(SERVICE_START_PENDING);
    MonitoringService instance;
    shutdownTimer = CreateThreadpoolTimer(
        [](PTP_CALLBACK_INSTANCE, void *, PTP_TIMER) {
            // Bound shutdown even if an OS consumer fails to return. SCM observes failure, never false
            // success.
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        },
        nullptr, nullptr);
    service = &instance;
    auto result = instance.run([] {
        std::lock_guard lock(controlMutex);
        report(SERVICE_RUNNING);
    });
    {
        std::lock_guard lock(controlMutex);
        service = nullptr;
        if (shutdownTimer)
            SetThreadpoolTimer(shutdownTimer, nullptr, 0, 0);
    }
    if (shutdownTimer)
    {
        WaitForThreadpoolTimerCallbacks(shutdownTimer, TRUE);
        CloseThreadpoolTimer(shutdownTimer);
        shutdownTimer = nullptr;
    }
    report(SERVICE_STOPPED, result);
}
} // namespace
int wmain()
{
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(ServiceName), entry}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table))
        return int(GetLastError());
    return 0;
}
