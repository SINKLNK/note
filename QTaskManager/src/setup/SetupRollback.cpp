#include "SetupRollback.h"
namespace monitoring
{
uint32_t rollbackUpdate(const RollbackOperations &ops)
{
    for (auto *action : {&ops.stopNew, &ops.removeRegistration, &ops.restoreFiles, &ops.restartOld})
    {
        if (auto error = (*action)())
            return error;
    }
    return 0;
}
} // namespace monitoring
