#pragma once
#include <utility>
#include <windows.h>
namespace monitoring
{
class WinHandle
{
    HANDLE h_ = nullptr;

  public:
    explicit WinHandle(HANDLE h = nullptr) : h_(h)
    {
    }
    ~WinHandle()
    {
        reset();
    }
    WinHandle(const WinHandle &) = delete;
    WinHandle &operator=(const WinHandle &) = delete;
    WinHandle(WinHandle &&o) noexcept : h_(o.release())
    {
    }
    WinHandle &operator=(WinHandle &&o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    HANDLE get() const
    {
        return h_;
    }
    explicit operator bool() const
    {
        return h_ && h_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release()
    {
        return std::exchange(h_, nullptr);
    }
    void reset(HANDLE h = nullptr)
    {
        if (*this)
            CloseHandle(h_);
        h_ = h;
    }
};
inline uint64_t qpcNow()
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}
inline uint64_t qpcFrequency()
{
    LARGE_INTEGER n;
    QueryPerformanceFrequency(&n);
    return n.QuadPart;
}
} // namespace monitoring
