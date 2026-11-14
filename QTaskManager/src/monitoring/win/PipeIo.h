#pragma once
#include "WinHandle.h"
#include "monitoring/WireCodec.h"
namespace monitoring
{
// All operations finish or cancel and drain their OVERLAPPED before returning.
bool pipeTransfer(HANDLE pipe, HANDLE stop, void *data, size_t size, bool writing, DWORD timeoutMs);
bool readFrame(HANDLE pipe, HANDLE stop, Frame &frame, DWORD timeoutMs);
bool writeFrame(HANDLE pipe, HANDLE stop, const Frame &frame, DWORD timeoutMs);
bool awaitPipeConnect(HANDLE pipe, HANDLE stop);
} // namespace monitoring
