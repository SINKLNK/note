#include "PipeIo.h"
#include <array>
namespace monitoring
{
namespace
{
bool finish(HANDLE pipe, HANDLE stop, OVERLAPPED &ov, DWORD timeout, DWORD &count)
{
    HANDLE waits[]{stop, ov.hEvent};
    DWORD result = WaitForMultipleObjects(2, waits, FALSE, timeout);
    if (result != WAIT_OBJECT_0 + 1)
    {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &count, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &count, FALSE) != FALSE;
}
} // namespace
bool pipeTransfer(HANDLE pipe, HANDLE stop, void *data, size_t size, bool writing, DWORD timeout)
{
    auto deadline = GetTickCount64() + timeout;
    size_t done = 0;
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    while (done < size)
    {
        auto now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            return false;
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        ResetEvent(ov.hEvent);
        DWORD count = 0;
        BOOL ok = writing ? WriteFile(pipe, static_cast<char *>(data) + done, DWORD(size - done), &count, &ov)
                          : ReadFile(pipe, static_cast<char *>(data) + done, DWORD(size - done), &count, &ov);
        if (!ok &&
            (GetLastError() != ERROR_IO_PENDING || !finish(pipe, stop, ov, DWORD(deadline - now), count)))
            return false;
        if (!count)
            return false;
        done += count;
    }
    return true;
}
bool readFrame(HANDLE pipe, HANDLE stop, Frame &frame, DWORD timeout)
{
    std::vector<std::byte> bytes(HeaderSize);
    auto begin = GetTickCount64();
    if (!pipeTransfer(pipe, stop, bytes.data(), bytes.size(), false, timeout))
        return false;
    auto d = decodeFrame(bytes);
    if (d.error != DecodeError::None && d.error != DecodeError::NeedMore)
        return false;
    size_t n = 0;
    for (size_t i = 0; i < 4; ++i)
        n |= size_t(std::to_integer<uint8_t>(bytes[8 + i])) << (8 * i);
    if (n > MaxPayload)
        return false;
    auto elapsed = GetTickCount64() - begin;
    if (elapsed >= timeout)
        return false;
    bytes.resize(HeaderSize + n);
    if (n && !pipeTransfer(pipe, stop, bytes.data() + HeaderSize, n, false, timeout - DWORD(elapsed)))
        return false;
    d = decodeFrame(bytes);
    if (!d.frame)
        return false;
    frame = std::move(*d.frame);
    return true;
}
bool writeFrame(HANDLE pipe, HANDLE stop, const Frame &frame, DWORD timeout)
{
    auto b = encodeFrame(frame);
    return !b.empty() && pipeTransfer(pipe, stop, b.data(), b.size(), true, timeout);
}
bool awaitPipeConnect(HANDLE pipe, HANDLE stop)
{
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &ov))
        return true;
    auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
        return true;
    DWORD count{};
    return error == ERROR_IO_PENDING && finish(pipe, stop, ov, INFINITE, count);
}
} // namespace monitoring
