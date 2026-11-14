#pragma once
#include "MonitoringTypes.h"
#include "win/ServiceControl.h"
#include <mutex>
#include <optional>
namespace monitoring
{
struct Delivery
{
    std::optional<Snapshot> snapshot;
    std::optional<Status> status;
    std::optional<ServiceInfo> service;
    std::optional<bool> connected;
    std::optional<std::string> error;
};
class ClientDelivery
{
    std::mutex mutex_;
    Delivery pending_;
    uint64_t acknowledged_{};

  public:
    void snapshot(Snapshot);
    void status(Status);
    void service(ServiceInfo);
    void connection(bool);
    void error(std::string);
    Delivery take(uint64_t now);
    bool responsive(uint64_t now);
};
} // namespace monitoring
