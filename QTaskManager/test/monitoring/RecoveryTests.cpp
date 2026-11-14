#include "TestSupport.h"
#include "service/CollectorHealth.h"
#include "service/ServiceRuntime.h"
#include "setup/SetupRollback.h"
#include <vector>
using namespace monitoring;
int main()
{
    ProcessIdentity ids;
    IoAccumulator totals;
    CollectorHealth health;
    health.reset();
    ids.processStart({1, 10}, 100);
    ids.threadStart(7, {1, 10}, 110);
    totals.sample(100, 1000);
    totals.add({1, 10}, IoKind::DiskWrite, 1000000);
    CHECK(!health.check(0, 0, ids, totals));
    CHECK(health.check(1, 0, ids, totals));
    CHECK(!ids.resolveTid(7, 200));
    CHECK(totals.sample(200, 1000).empty());
    CHECK(health.check(1, 0, ids, totals));
    health.reset();
    ids.processStart({2, 20}, 300);
    ids.threadStart(7, {2, 20}, 310);
    CHECK(!health.check(0, 0, ids, totals));
    CHECK(ids.resolveTid(7, 320) == (ProcessKey{2, 20}));
    health.finished(0);
    CHECK(health.check(0, 0, ids, totals)->state == SourceState::Failed);
    health.reset();
    health.finished(5);
    CHECK(health.check(0, 0, ids, totals)->win32Error == 5);
    bool running = false;
    auto failure = initializeServiceEndpoint(
        [] { return SourceStatus{SourceState::Failed, 5, "pipe denied"}; }, [&] { running = true; });
    CHECK(!running);
    CHECK(failure.win32Error == 5);
    CHECK(initializeServiceEndpoint([] { return SourceStatus{SourceState::Ready}; }, [&] { running = true; })
              .state == SourceState::Ready);
    CHECK(running);
    std::vector<int> order;
    auto stop = stopTraceConsumer({[] { return 5; }, [&] { order.push_back(1); },
                                   [&](uint32_t timeout) {
                                       CHECK(timeout <= 4000);
                                       order.push_back(2);
                                       return true;
                                   }});
    CHECK(stop.error == 5);
    CHECK(stop.consumerExited);
    CHECK(order == (std::vector<int>{1, 2}));
    unsigned waits = 0;
    CHECK(!stopTraceConsumer({[] { return 0; }, [] {},
                              [&](uint32_t) {
                                  ++waits;
                                  return false;
                              }})
               .consumerExited);
    CHECK(waits <= 2);
    order.clear();
    CHECK(rollbackUpdate({[&] {
                              order.push_back(1);
                              return 0;
                          },
                          [&] {
                              order.push_back(2);
                              return 0;
                          },
                          [&] {
                              order.push_back(3);
                              return 0;
                          },
                          [&] {
                              order.push_back(4);
                              return 0;
                          }}) == 0);
    CHECK(order == (std::vector<int>{1, 2, 3, 4}));
    order.clear();
    CHECK(rollbackUpdate({[] { return 32; },
                          [&] {
                              order.push_back(2);
                              return 0;
                          },
                          [&] {
                              order.push_back(3);
                              return 0;
                          },
                          [&] {
                              order.push_back(4);
                              return 0;
                          }}) == 32);
    CHECK(order.empty());
    CHECK(rollbackUpdate({[] { return 0; }, [] { return 0; }, [] { return 5; }, [] { return 0; }}) == 5);
}
