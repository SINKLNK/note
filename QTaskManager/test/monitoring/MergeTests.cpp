#include "ServiceSnapshotMerge.h"
#include "TestSupport.h"
using namespace monitoring;
int main()
{
    QVector<ProcessEntry> rows(2);
    rows[0].pid = 3;
    rows[0].creationTimeTicks = 10;
    rows[1].pid = 3;
    rows[1].creationTimeTicks = 20;
    Snapshot s;
    s.sampleQpc = 100;
    s.qpcFrequency = 1000;
    s.intervalQpc = 1000;
    s.disk.state = SourceState::Ready;
    s.network.state = SourceState::Partial;
    s.processes = {{{3, 10}, 2, 4}};
    mergeServiceSnapshot(rows, &s, 200, 1000);
    CHECK(rows[0].diskRateMBps == 2);
    CHECK(rows[0].networkRateMBps == 4);
    CHECK(rows[1].diskRateMBps == -1);
    mergeServiceSnapshot(rows, &s, 3101, 1000);
    CHECK(rows[0].diskRateMBps == -1);
    mergeServiceSnapshot(rows, &s, 200, 1000);
    mergeServiceSnapshot(rows, nullptr, 200, 1000);
    CHECK(rows[0].networkRateMBps == -1);
    s.disk.state = SourceState::Failed;
    mergeServiceSnapshot(rows, &s, 200, 1000);
    CHECK(rows[0].diskRateMBps == -1);
    CHECK(rows[0].networkRateMBps == 4);
}
