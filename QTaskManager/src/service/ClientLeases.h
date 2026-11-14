#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
namespace monitoring
{
class ClientLeases
{
  public:
    void connected(uint64_t id, uint64_t now);
    void heartbeat(uint64_t id, uint64_t now);
    void disconnected(uint64_t id, uint64_t now);
    void expire(uint64_t now);
    bool shouldCollect(uint64_t now);
    size_t count();

  private:
    std::mutex mutex_;
    std::map<uint64_t, uint64_t> clients_;
    std::optional<uint64_t> emptySince_;
};
} // namespace monitoring
