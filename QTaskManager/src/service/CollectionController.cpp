#include "CollectionController.h"
namespace monitoring
{
void CollectionController::fault(SourceStatus status, uint64_t now)
{
    if (active_)
        stop_();
    active_ = false;
    status_ = std::move(status);
    retryAt_ = now + 5000;
}
void CollectionController::tick(bool wanted, uint64_t now)
{
    if (!wanted)
    {
        stop();
        return;
    }
    if (!active_ && now >= retryAt_)
    {
        status_ = start_();
        active_ = status_.state != SourceState::Failed && status_.state != SourceState::Unavailable;
        retryAt_ = now + 5000;
    }
}
void CollectionController::stop()
{
    if (active_)
        stop_();
    active_ = false;
    retryAt_ = 0;
    status_ = {SourceState::Unavailable, 0, "Waiting for a GUI connection"};
}
} // namespace monitoring
