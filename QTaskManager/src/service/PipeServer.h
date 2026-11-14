#pragma once
#include "ClientLeases.h"
#include "monitoring/win/PipeIo.h"
#include <functional>
#include <memory>
#include <thread>
namespace monitoring
{
class PipeServer
{
  public:
    explicit PipeServer(ClientLeases &leases, std::wstring name = PipeName,
                        std::function<bool(HANDLE)> identity = {},
                        std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;IU)");
    ~PipeServer();
    SourceStatus start(std::function<Frame(const Frame &)> handler);
    void stop();
    void publish(std::shared_ptr<const Snapshot>);
    size_t clientCount()
    {
        return leases_.count();
    }

  private:
    void serve(HANDLE pipe, uint64_t id);
    ClientLeases &leases_;
    std::wstring name_;
    std::function<bool(HANDLE)> identity_;
    std::wstring acl_;
    WinHandle stop_;
    std::vector<std::thread> threads_;
    std::vector<WinHandle> pipes_;
    std::function<Frame(const Frame &)> handler_;
    std::mutex latestMutex_;
    std::shared_ptr<const Snapshot> latest_;
};
} // namespace monitoring
