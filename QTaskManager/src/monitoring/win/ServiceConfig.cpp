#include "ServiceConfig.h"
#include <windows.h>
#include <initializer_list>
#include <sddl.h>
#include <utility>
#include <windows.h>
namespace monitoring
{
ServiceConfig readServiceConfig()
{
    ServiceConfig c;
    DWORD n = 0, size = 4;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, ConfigKey, L"TemperatureEnabled", RRF_RT_REG_DWORD, nullptr, &n,
                     &size) == ERROR_SUCCESS)
        c.temperatureEnabled = n == 1;
    n = 0;
    size = 4;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, ConfigKey, L"IntentionallyDisabled", RRF_RT_REG_DWORD, nullptr, &n,
                     &size) == ERROR_SUCCESS)
        c.intentionallyDisabled = n == 1;
    return c;
}
uint32_t writeServiceConfig(const ServiceConfig &c)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)",
                                                              SDDL_REVISION_1, &sd, nullptr))
        return GetLastError();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HKEY key = nullptr;
    auto error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ConfigKey, 0, nullptr, 0, KEY_SET_VALUE | WRITE_DAC, &sa,
                                 &key, nullptr);
    if (error == ERROR_SUCCESS)
    {
        error = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);
        for (auto pair : {std::pair{L"TemperatureEnabled", DWORD(c.temperatureEnabled)},
                          std::pair{L"IntentionallyDisabled", DWORD(c.intentionallyDisabled)},
                          std::pair{L"SchemaVersion", DWORD(c.schemaVersion)}})
            if (error == ERROR_SUCCESS)
                error = RegSetValueExW(key, pair.first, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&pair.second), 4);
        RegCloseKey(key);
    }
    LocalFree(sd);
    return error;
}
} // namespace monitoring
