#include "TestSupport.h"
#include "setup/SetupActions.h"
using namespace monitoring;
struct Platform : SetupPlatform
{
    bool automatic = false, running = false, installed = false;
    unsigned calls = 0;
    uint32_t validation = 0, error = 0;
    uint32_t validateManagedService() override
    {
        return validation;
    }
    uint32_t install(bool) override
    {
        ++calls;
        if (error)
            return error;
        installed = automatic = running = true;
        return 0;
    }
    uint32_t enable() override
    {
        ++calls;
        automatic = running = true;
        return error;
    }
    uint32_t disable() override
    {
        ++calls;
        automatic = running = false;
        return error;
    }
    uint32_t remove() override
    {
        ++calls;
        installed = running = false;
        return error;
    }
};
int main()
{
    Platform p;
    CHECK(runSetup({SetupAction::InstallService}, p).outcome == SetupOutcome::Success);
    CHECK(p.installed && p.automatic && p.running);
    CHECK(runSetup({SetupAction::DisableService}, p).outcome == SetupOutcome::Success);
    CHECK(!p.running && !p.automatic);
    CHECK(runSetup({SetupAction::EnableService}, p).outcome == SetupOutcome::Success);
    CHECK(p.running && p.automatic);
    p.validation = 5;
    auto n = p.calls;
    CHECK(runSetup({SetupAction::RemoveService}, p).win32Error == 5);
    CHECK(p.calls == n);
    p.validation = 0;
    p.error = 32;
    CHECK(runSetup({SetupAction::UpdateService}, p).outcome == SetupOutcome::Failed);
    p.error = SetupRollbackIncomplete;
    CHECK(runSetup({SetupAction::UpdateService}, p).outcome == SetupOutcome::Partial);
    n = p.calls;
    CHECK(runSetup({SetupAction::InstallPawnIo}, p).outcome == SetupOutcome::Unsupported);
    CHECK(p.calls == n);
    CHECK(!mayRemovePawnIo(DriverOwnership::External, true));
    CHECK(!mayRemovePawnIo(DriverOwnership::Unknown, true));
    CHECK(!mayRemovePawnIo(DriverOwnership::QTaskManager, false));
    CHECK(mayRemovePawnIo(DriverOwnership::QTaskManager, true));
}
