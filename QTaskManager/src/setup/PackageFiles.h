#pragma once
#include <string>
#include <windows.h>
namespace monitoring
{
std::wstring serviceDirectory();
DWORD verifyDirectory(const std::wstring &path);
DWORD verifyManagedDirectories();
DWORD prepareDirectory();
DWORD stageEmbeddedService();
} // namespace monitoring
