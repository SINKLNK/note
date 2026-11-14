#include "SetupActions.h"
namespace monitoring
{
SetupResult runSetup(const SetupRequest &r, SetupPlatform &p)
{
    if (r.action > SetupAction::UpdateService || r.withTemperatures)
        return {SetupOutcome::Unsupported, 50, false, "Temperature component is not included in this build"};
    auto error = p.validateManagedService();
    if (error)
        return {SetupOutcome::Failed, error, false, "Service ownership/path validation failed"};
    switch (r.action)
    {
    case SetupAction::InstallService:
        error = p.install(false);
        break;
    case SetupAction::UpdateService:
        error = p.install(true);
        break;
    case SetupAction::EnableService:
        error = p.enable();
        break;
    case SetupAction::DisableService:
        error = p.disable();
        break;
    case SetupAction::RemoveService:
        error = p.remove();
        break;
    default:
        break;
    }
    if (error == SetupRollbackIncomplete)
        return {SetupOutcome::Partial, error, false,
                "Rollback incomplete; inspect the service state and recovery backup"};
    return {error ? SetupOutcome::Failed : SetupOutcome::Success, error, false,
            error ? "Operation failed; inspect the current service state" : "Operation completed"};
}
} // namespace monitoring
