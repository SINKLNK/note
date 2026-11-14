#include "ServiceSnapshotMerge.h"
#include <cmath>
#include <map>
void mergeServiceSnapshot(QVector<ProcessEntry> &rows, const monitoring::Snapshot *s, uint64_t now,
                          uint64_t frequency)
{
    for (auto &row : rows)
    {
        row.diskRateMBps = row.networkRateMBps = -1;
        row.diskQuality = row.networkQuality = ProcessEntry::IoQuality::Unavailable;
    }
    if (!s || !frequency || s->qpcFrequency != frequency || !s->intervalQpc || now < s->sampleQpc ||
        double(now - s->sampleQpc) / double(frequency) > 3.0)
        return;
    std::map<monitoring::ProcessKey, const monitoring::IoRate *> index;
    for (auto &p : s->processes)
        index.emplace(p.key, &p);
    auto assign = [](monitoring::SourceState state, double value, double &rate,
                     ProcessEntry::IoQuality &quality) {
        if (std::isfinite(value) && value >= 0 &&
            (state == monitoring::SourceState::Ready || state == monitoring::SourceState::Partial))
        {
            rate = value;
            quality = state == monitoring::SourceState::Ready ? ProcessEntry::IoQuality::Complete
                                                              : ProcessEntry::IoQuality::Partial;
        }
    };
    for (auto &row : rows)
    {
        auto it = index.find({row.pid, row.creationTimeTicks});
        if (it == index.end())
            continue;
        assign(s->disk.state, it->second->diskMBps, row.diskRateMBps, row.diskQuality);
        assign(s->network.state, it->second->networkMBps, row.networkRateMBps, row.networkQuality);
    }
}
