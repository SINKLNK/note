#pragma once
#include "monitoring/MonitoringTypes.h"
#include <windows.h>
namespace monitoring
{
enum class ServiceState
{
    Missing,
    Stopped,
    Starting,
    Running,
    Stopping,
    Disabled,
    AccessDenied,
    Failed
};
struct ServiceInfo
{
    ServiceState state = ServiceState::Missing;
    uint32_t pid{}, error{};
    std::string version;
    bool automatic{};
};
ServiceInfo queryService();
class ScHandle
{
    SC_HANDLE h_{};

  public:
    explicit ScHandle(SC_HANDLE h = nullptr) : h_(h)
    {
    }
    ~ScHandle()
    {
        if (h_)
            CloseServiceHandle(h_);
    }
    ScHandle(const ScHandle &) = delete;
    ScHandle &operator=(const ScHandle &) = delete;
    SC_HANDLE get() const
    {
        return h_;
    }
    explicit operator bool() const
    {
        return h_ != nullptr;
    }
};
} // namespace monitoring
