#include "EventAttribution.h"
namespace monitoring
{
void EventAttribution::unresolved(AttributionIssue issue, const DecodedEvent &e, uint32_t error)
{
    for (auto *counters : {&total_, &interval_})
    {
        auto &count = counters->issues[size_t(issue)];
        ++count.events;
        count.bytes += e.bytes;
        count.lastPid = e.pid;
        count.lastTid = e.tid;
        count.lastError = error;
    }
}
void EventAttribution::consume(const DecodedEvent &e, ProcessIdentity &ids, IoAccumulator &totals)
{
    switch (e.kind)
    {
    case EventKind::ProcessStart:
    {
        auto found = lookup_(e.pid, e.qpc);
        if (found.key)
            ids.processStart(*found.key, e.qpc);
        else
            unresolved(found.issue, e, found.error);
        return;
    }
    case EventKind::ProcessEnd:
        ids.processEnd(e.pid, e.qpc);
        return;
    case EventKind::ThreadStart:
        if (auto key = ids.resolvePid(e.pid, e.qpc))
            ids.threadStart(e.tid, *key, e.qpc);
        else
            unresolved(AttributionIssue::ThreadUnknownProcess, e);
        return;
    case EventKind::ThreadEnd:
        ids.threadEnd(e.tid, e.qpc);
        return;
    default:
        break;
    }
    bool disk = e.kind == EventKind::DiskRead || e.kind == EventKind::DiskWrite;
    if (disk)
    {
        ++total_.diskEvents;
        ++interval_.diskEvents;
    }
    else
    {
        ++total_.networkEvents;
        ++interval_.networkEvents;
    }
    auto key = disk ? ids.resolveTid(e.tid, e.qpc) : ids.resolvePid(e.pid, e.qpc);
    if (!key)
    {
        unresolved(disk ? (e.tid ? AttributionIssue::DiskUnknownThread : AttributionIssue::DiskMissingThreadId)
                        : (e.pid ? AttributionIssue::NetworkUnknownProcess : AttributionIssue::NetworkMissingProcessId),
                   e);
        return;
    }
    totals.add(*key,
               disk ? (e.kind == EventKind::DiskRead ? IoKind::DiskRead : IoKind::DiskWrite)
                    : (e.kind == EventKind::NetSend ? IoKind::NetSend : IoKind::NetReceive),
               e.bytes);
}
void EventAttribution::finishInterval(Diagnostics &diagnostics, SourceStatus &disk, SourceStatus &network)
{
    diagnostics.attributionTotal = total_;
    diagnostics.attributionInterval = interval_;
    diagnostics.unresolvedIds = 0;
    for (size_t i = 0; i < total_.issues.size(); ++i)
        diagnostics.unresolvedIds += total_.issues[i].events;
    auto failed = [&](AttributionIssue issue) { return interval_.issues[size_t(issue)].events != 0; };
    if (disk.state == SourceState::Ready &&
        (failed(AttributionIssue::DiskMissingThreadId) || failed(AttributionIssue::DiskUnknownThread)))
        disk.state = SourceState::Partial;
    if (network.state == SourceState::Ready &&
        (failed(AttributionIssue::NetworkMissingProcessId) || failed(AttributionIssue::NetworkUnknownProcess)))
        network.state = SourceState::Partial;
    interval_ = {};
}
void EventAttribution::reset()
{
    total_ = {};
    interval_ = {};
}
} // namespace monitoring
