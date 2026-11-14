#include "ServiceRuntime.h"
namespace monitoring
{
SourceStatus initializeServiceEndpoint(const std::function<SourceStatus()> &initialize,
                                       const std::function<void()> &ready)
{
    auto result = initialize();
    if (result.state == SourceState::Ready)
        ready();
    return result;
}
TraceStopResult stopTraceConsumer(const TraceStopOperations &ops)
{
    auto error = ops.stopSession();
    bool closed = false;
    if (error)
    {
        ops.closeConsumer();
        closed = true;
    }
    if (ops.waitConsumer(3000))
        return {error, true};
    if (!closed)
        ops.closeConsumer();
    return {error ? error : 1460, ops.waitConsumer(3000)};
}
} // namespace monitoring
