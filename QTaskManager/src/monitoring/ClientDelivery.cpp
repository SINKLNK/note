#include "ClientDelivery.h"
namespace monitoring
{
void ClientDelivery::snapshot(Snapshot s)
{
    std::lock_guard lock(mutex_);
    pending_.snapshot = std::move(s);
}
void ClientDelivery::status(Status s)
{
    std::lock_guard lock(mutex_);
    pending_.status = std::move(s);
}
void ClientDelivery::service(ServiceInfo s)
{
    std::lock_guard lock(mutex_);
    pending_.service = std::move(s);
}
void ClientDelivery::connection(bool connected)
{
    std::lock_guard lock(mutex_);
    pending_.connected = connected;
    if (!connected)
        pending_.snapshot.reset();
}
void ClientDelivery::error(std::string s)
{
    std::lock_guard lock(mutex_);
    pending_.error = std::move(s);
}
Delivery ClientDelivery::take(uint64_t now)
{
    std::lock_guard lock(mutex_);
    acknowledged_ = now;
    auto result = std::move(pending_);
    pending_ = {};
    return result;
}
bool ClientDelivery::responsive(uint64_t now)
{
    std::lock_guard lock(mutex_);
    return now >= acknowledged_ && now - acknowledged_ < 15000;
}
} // namespace monitoring
