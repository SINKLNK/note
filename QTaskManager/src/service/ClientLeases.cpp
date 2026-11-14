#include "ClientLeases.h"
namespace monitoring
{
void ClientLeases::connected(uint64_t id, uint64_t now)
{
    std::lock_guard lock(mutex_);
    clients_[id] = now;
    emptySince_.reset();
}
void ClientLeases::heartbeat(uint64_t id, uint64_t now)
{
    std::lock_guard lock(mutex_);
    auto it = clients_.find(id);
    if (it != clients_.end())
        it->second = now;
}
void ClientLeases::disconnected(uint64_t id, uint64_t now)
{
    std::lock_guard lock(mutex_);
    if (clients_.erase(id) && clients_.empty())
        emptySince_ = now;
}
void ClientLeases::expire(uint64_t now)
{
    std::lock_guard lock(mutex_);
    bool removed = false;
    for (auto it = clients_.begin(); it != clients_.end();)
        if (now >= it->second && now - it->second >= 15000)
        {
            it = clients_.erase(it);
            removed = true;
        }
        else
            ++it;
    if (removed && clients_.empty())
        emptySince_ = now;
}
bool ClientLeases::shouldCollect(uint64_t now)
{
    std::lock_guard lock(mutex_);
    return !clients_.empty() || (emptySince_ && now >= *emptySince_ && now - *emptySince_ < 5000);
}
size_t ClientLeases::count()
{
    std::lock_guard lock(mutex_);
    return clients_.size();
}
} // namespace monitoring
