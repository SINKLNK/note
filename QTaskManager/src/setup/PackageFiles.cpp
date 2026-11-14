#include "PackageFiles.h"
#include "PackageSecurity.h"
#include "monitoring/win/WinHandle.h"
#include <aclapi.h>
#include <filesystem>
#include <sddl.h>
#include <shlobj.h>
namespace monitoring
{
std::wstring serviceDirectory()
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &base)))
        return {};
    std::wstring path = std::wstring(base) + L"\\QTaskManager\\Monitoring";
    CoTaskMemFree(base);
    return path;
}
DWORD verifyDirectory(const std::wstring &path)
{
    if (path.empty())
        return ERROR_BAD_PATHNAME;
    std::filesystem::path current;
    for (auto &part : std::filesystem::path(path))
    {
        current /= part;
        if (current == current.root_name())
            continue;
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            return error;
        }
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            return ERROR_REPARSE_TAG_INVALID;
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_DIRECTORY;
    }
    return ERROR_SUCCESS;
}
DWORD prepareDirectory()
{
    auto path = serviceDirectory();
    if (auto error = verifyDirectory(path))
        return error;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", SDDL_REVISION_1, &sd, nullptr))
        return GetLastError();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    DWORD error = 0;
    for (auto dir : {std::filesystem::path(path).parent_path().wstring(), path})
    {
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            PSECURITY_DESCRIPTOR existing = nullptr;
            DWORD e = GetNamedSecurityInfoW(dir.data(), SE_FILE_OBJECT,
                                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr,
                                            nullptr, nullptr, nullptr, &existing);
            bool safe = e == ERROR_SUCCESS && isProtectedInstallAcl(existing);
            if (existing)
                LocalFree(existing);
            if (!safe)
            {
                error = e ? e : ERROR_ACCESS_DENIED;
                break;
            }
        }
        if (!CreateDirectoryW(dir.c_str(), &sa) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            error = GetLastError();
            break;
        }
        if (auto e = verifyDirectory(dir))
        {
            error = e;
            break;
        }
        if (!SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              sd))
        {
            error = GetLastError();
            break;
        }
    }
    LocalFree(sd);
    return error;
}
DWORD verifyManagedDirectories()
{
    auto path = serviceDirectory();
    if (auto error = verifyDirectory(path))
        return error;
    for (auto dir : {std::filesystem::path(path).parent_path().wstring(), path})
    {
        auto attributes = GetFileAttributesW(dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            return error;
        }
        PSECURITY_DESCRIPTOR sd = nullptr;
        auto error = GetNamedSecurityInfoW(dir.data(), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, nullptr, nullptr, &sd);
        bool safe = error == ERROR_SUCCESS && isProtectedInstallAcl(sd);
        if (sd)
            LocalFree(sd);
        if (!safe)
            return error ? error : ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}
DWORD stageEmbeddedService()
{
    auto path = serviceDirectory() + L"\\QTaskManagerService.new";
    if (auto e = prepareDirectory())
        return e;
    auto attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        return ERROR_ACCESS_DENIED;
    // Delete a stale staging link before CREATE_NEW; never truncate an existing hard link.
    if (attrs != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path.c_str()))
        return GetLastError();
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource)
        return GetLastError();
    HGLOBAL loaded = LoadResource(nullptr, resource);
    auto *bytes = LockResource(loaded);
    DWORD size = SizeofResource(nullptr, resource);
    if (!bytes || !size)
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    WinHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file)
        return GetLastError();
    DWORD written{};
    if (!WriteFile(file.get(), bytes, size, &written, nullptr) || written != size)
        return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    if (!FlushFileBuffers(file.get()))
        return GetLastError();
    return ERROR_SUCCESS;
}
} // namespace monitoring
