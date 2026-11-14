#pragma once
#include <cstdint>
#include <functional>
namespace monitoring
{
struct RollbackOperations
{
    std::function<uint32_t()> stopNew, removeRegistration, restoreFiles, restartOld;
};
uint32_t rollbackUpdate(const RollbackOperations &);
} // namespace monitoring
