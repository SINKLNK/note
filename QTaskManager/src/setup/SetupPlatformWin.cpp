#include "PackageFiles.h"
#include "SetupActions.h"
#include "SetupRollback.h"
#include "monitoring/win/ServiceConfig.h"
#include "monitoring/win/ServiceControl.h"
#include <cwchar>
namespace monitoring
{
namespace
{
std::wstring imagePath()
{
    return serviceDirectory() + L"\\QTaskManagerService.exe";
}
DWORD waitState(SC_HANDLE h, DWORD desired)
{
    auto deadline = GetTickCount64() + 10000;
    SERVICE_STATUS_PROCESS s{};
    DWORD n{};
    do
    {
        if (!QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&s), sizeof(s), &n))
            return GetLastError();
        if (s.dwCurrentState == desired)
            return 0;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    return ERROR_TIMEOUT;
}
DWORD stopService(SC_HANDLE h)
{
    SERVICE_STATUS s{};
    if (!ControlService(h, SERVICE_CONTROL_STOP, &s))
    {
        auto e = GetLastError();
        if (e != ERROR_SERVICE_NOT_ACTIVE)
            return e;
    }
    return waitState(h, SERVICE_STOPPED);
}
DWORD startService(SC_HANDLE h)
{
    if (!StartServiceW(h, 0, nullptr))
    {
        auto e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING)
            return e;
    }
    return waitState(h, SERVICE_RUNNING);
}
class Platform final : public SetupPlatform
{
  public:
    uint32_t validateManagedService() override
    {
        if (auto error = verifyManagedDirectories())
            return error;
        auto dir = serviceDirectory();
        if (auto e = verifyDirectory(dir))
            return e;
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm)
            return GetLastError();
        ScHandle svc(OpenServiceW(scm.get(), ServiceName, SERVICE_QUERY_CONFIG));
        if (!svc)
            return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : GetLastError();
        DWORD n{};
        QueryServiceConfigW(svc.get(), nullptr, 0, &n);
        std::vector<BYTE> b(n);
        auto *c = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(b.data());
        if (!n || !QueryServiceConfigW(svc.get(), c, n, &n))
            return GetLastError();
        auto expected = L"\"" + imagePath() + L"\"";
        if (_wcsicmp(c->lpBinaryPathName, expected.c_str()) != 0 ||
            c->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
            _wcsicmp(c->lpServiceStartName, L"LocalSystem") != 0)
            return ERROR_ACCESS_DENIED;
        return 0;
    }
    uint32_t install(bool update) override
    {
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
        if (!scm)
            return GetLastError();
        ScHandle svc(OpenServiceW(scm.get(), ServiceName,
                                  SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP |
                                      SERVICE_CHANGE_CONFIG | DELETE));
        DWORD openError = GetLastError();
        if (!svc && openError != ERROR_SERVICE_DOES_NOT_EXIST)
            return openError;
        if (update && !svc)
            return ERROR_SERVICE_DOES_NOT_EXIST;
        auto old = queryService();
        if (auto e = stageEmbeddedService())
            return e;
        if (svc)
        {
            if (auto e = stopService(svc.get()))
                return e;
        }
        auto path = imagePath(), stage = serviceDirectory() + L"\\QTaskManagerService.new",
             backup = serviceDirectory() + L"\\QTaskManagerService.bak";
        bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        for (auto &file : {path, backup})
        {
            auto a = GetFileAttributesW(file.c_str());
            if (a != INVALID_FILE_ATTRIBUTES &&
                (a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                return ERROR_ACCESS_DENIED;
        }
        if (existed &&
            !MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            auto e = GetLastError();
            if (old.state == ServiceState::Running && svc)
                startService(svc.get());
            return e;
        }
        DWORD error = 0;
        bool created = false;
        if (!MoveFileExW(stage.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            error = GetLastError();
        auto rollback = [&] {
            return rollbackUpdate(
                {[&]() -> uint32_t {
                     if (svc)
                         return stopService(svc.get());
                     if (!created)
                         return 0;
                     ScHandle current(
                         OpenServiceW(scm.get(), ServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
                     if (!current)
                         return GetLastError();
                     return stopService(current.get());
                 },
                 [&]() -> uint32_t {
                     if (!created)
                         return 0;
                     ScHandle current(OpenServiceW(scm.get(), ServiceName, DELETE));
                     if (!current)
                         return GetLastError();
                     return DeleteService(current.get()) ? 0 : GetLastError();
                 },
                 [&]() -> uint32_t {
                     if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                         return GetLastError();
                     if (existed && !MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
                         return GetLastError();
                     return 0;
                 },
                 [&]() -> uint32_t {
                     return old.state == ServiceState::Running && svc ? startService(svc.get()) : 0;
                 }});
        };
        auto failed = [&](DWORD originalError) -> uint32_t {
            // A failed rollback preserves the backup and is reported as partial, never successful.
            return rollback() == ERROR_SUCCESS ? originalError : SetupRollbackIncomplete;
        };
        if (error)
        {
            return failed(error);
        }
        if (!svc)
        {
            auto command = L"\"" + path + L"\"";
            ScHandle fresh(CreateServiceW(scm.get(), ServiceName, L"QTaskManager monitoring",
                                          SERVICE_START | SERVICE_QUERY_STATUS | DELETE,
                                          SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                          command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
            if (!fresh)
            {
                error = GetLastError();
                return failed(error);
            }
            created = true;
            error = startService(fresh.get());
        }
        else if (old.state == ServiceState::Running)
            error = startService(svc.get());
        if (error)
        {
            return failed(error);
        }
        auto config = readServiceConfig();
        config.temperatureEnabled = false;
        if (!svc)
            config.intentionallyDisabled = false;
        error = writeServiceConfig(config);
        if (error)
        {
            return failed(error);
        }
        DeleteFileW(backup.c_str());
        return 0;
    }
    uint32_t enable() override
    {
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm)
            return GetLastError();
        ScHandle s(OpenServiceW(scm.get(), ServiceName,
                                SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS));
        if (!s)
            return GetLastError();
        if (!ChangeServiceConfigW(s.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return GetLastError();
        auto c = readServiceConfig();
        c.intentionallyDisabled = false;
        auto e = writeServiceConfig(c);
        return e ? e : startService(s.get());
    }
    uint32_t disable() override
    {
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm)
            return GetLastError();
        ScHandle s(OpenServiceW(scm.get(), ServiceName,
                                SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (!s)
            return GetLastError();
        if (auto e = stopService(s.get()))
            return e;
        if (!ChangeServiceConfigW(s.get(), SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return GetLastError();
        auto c = readServiceConfig();
        c.intentionallyDisabled = true;
        return writeServiceConfig(c);
    }
    uint32_t remove() override
    {
        ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm)
            return GetLastError();
        {
            ScHandle s(OpenServiceW(scm.get(), ServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
            if (s)
            {
                if (auto e = stopService(s.get()))
                    return e;
                if (!DeleteService(s.get()))
                    return GetLastError();
            }
            else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
                return GetLastError();
        }
        // Keep machine ownership records and any shared PawnIO installation.
        for (auto suffix :
             {L"QTaskManagerService.exe", L"QTaskManagerService.new", L"QTaskManagerService.bak"})
        {
            auto path = serviceDirectory() + L"\\" + suffix;
            auto attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                return ERROR_ACCESS_DENIED;
            if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                return GetLastError();
        }
        return 0;
    }
};
} // namespace
SetupResult runSetup(const SetupRequest &r)
{
    Platform p;
    return runSetup(r, p);
}
} // namespace monitoring
