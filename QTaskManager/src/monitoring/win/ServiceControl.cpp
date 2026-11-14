#include "ServiceControl.h"
namespace monitoring
{
ServiceInfo queryService()
{
    ServiceInfo result;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
    {
        result.error = GetLastError();
        result.state =
            result.error == ERROR_ACCESS_DENIED ? ServiceState::AccessDenied : ServiceState::Failed;
        return result;
    }
    ScHandle service(OpenServiceW(scm.get(), ServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service)
    {
        result.error = GetLastError();
        result.state = result.error == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::Missing
                       : result.error == ERROR_ACCESS_DENIED        ? ServiceState::AccessDenied
                                                                    : ServiceState::Failed;
        return result;
    }
    SERVICE_STATUS_PROCESS s{};
    DWORD needed{};
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&s), sizeof(s),
                              &needed))
    {
        result.error = GetLastError();
        result.state = ServiceState::Failed;
        return result;
    }
    result.pid = s.dwProcessId;
    result.error = s.dwWin32ExitCode;
    switch (s.dwCurrentState)
    {
    case SERVICE_RUNNING:
        result.state = ServiceState::Running;
        break;
    case SERVICE_START_PENDING:
        result.state = ServiceState::Starting;
        break;
    case SERVICE_STOP_PENDING:
        result.state = ServiceState::Stopping;
        break;
    default:
        result.state = ServiceState::Stopped;
    }
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<BYTE> bytes(needed);
    if (needed && QueryServiceConfigW(service.get(), reinterpret_cast<QUERY_SERVICE_CONFIGW *>(bytes.data()),
                                      needed, &needed))
    {
        auto *c = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(bytes.data());
        result.automatic = c->dwStartType == SERVICE_AUTO_START;
        if (c->dwStartType == SERVICE_DISABLED)
            result.state = ServiceState::Disabled;
    }
    return result;
}
} // namespace monitoring
