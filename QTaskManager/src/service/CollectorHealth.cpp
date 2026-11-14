#include "CollectorHealth.h"
namespace monitoring
{
void CollectorHealth::reset()
{
    invalid_ = false;
    exitCode_ = 0;
    exited_ = false;
}
void CollectorHealth::finished(uint32_t error)
{
    exitCode_ = error;
    exited_ = true;
}
std::optional<SourceStatus> CollectorHealth::check(uint64_t events, uint64_t buffers, ProcessIdentity &ids,
                                                   IoAccumulator &totals)
{
    invalid_ = invalid_ || events != 0 || buffers != 0 || exited_.load();
    if (!invalid_)
        return {};
    ids = {};
    totals.reset();
    if (exited_)
        return SourceStatus{SourceState::Failed, exitCode_ ? exitCode_.load() : 995,
                            "ETW consumer exited; restarting collection"};
    return SourceStatus{SourceState::Failed, 13, "ETW events lost; rebuilding process identities"};
}
} // namespace monitoring
