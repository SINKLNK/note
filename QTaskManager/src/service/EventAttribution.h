#pragma once
#include "EtwDecoder.h"
#include "IoAccumulator.h"
#include "ProcessIdentity.h"
namespace monitoring
{
struct ProcessLookupResult
{
    std::optional<ProcessKey> key;
    AttributionIssue issue = AttributionIssue::ProcessOpenFailed;
    uint32_t error{};
};
// Account for identity failures at the event boundary, before any bytes are discarded.
// All storage is fixed size; the hot I/O path performs no additional OS queries.
class EventAttribution
{
  public:
    using ProcessLookup = std::function<ProcessLookupResult(uint32_t pid, uint64_t atQpc)>;
    explicit EventAttribution(ProcessLookup lookup) : lookup_(std::move(lookup)) {}
    void consume(const DecodedEvent &, ProcessIdentity &, IoAccumulator &);
    void finishInterval(Diagnostics &, SourceStatus &disk, SourceStatus &network);
    void reset();

  private:
    void unresolved(AttributionIssue, const DecodedEvent &, uint32_t error = 0);
    ProcessLookup lookup_;
    AttributionCounters total_, interval_;
};
} // namespace monitoring
