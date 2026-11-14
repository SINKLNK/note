#include "SetupActions.h"
#include <windows.h>
#include <shellapi.h>
#include <string_view>
#include <windows.h>
using namespace monitoring;
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc{};
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return ERROR_INVALID_PARAMETER;
    SetupRequest r;
    bool valid = argc == 3 && std::wstring_view(argv[1]) == L"--action" && wcslen(argv[2]) == 1 &&
                 argv[2][0] >= L'0' && argv[2][0] <= L'8';
    if (valid)
        r.action = SetupAction(argv[2][0] - L'0');
    LocalFree(argv);
    if (!valid)
        return ERROR_INVALID_PARAMETER;
    auto result = runSetup(r);
    return result.outcome == SetupOutcome::Success
               ? 0
               : int(result.win32Error ? result.win32Error : ERROR_GEN_FAILURE);
}
