#include "TestSupport.h"
#include "service/CollectionController.h"
#include "service/TemperatureSource.h"
using namespace monitoring;
int main()
{
    int starts = 0, stops = 0;
    CollectionController c(
        [&] {
            ++starts;
            return SourceStatus{SourceState::Ready};
        },
        [&] { ++stops; });
    c.tick(false, 0);
    CHECK(starts == 0);
    c.tick(true, 1);
    CHECK(starts == 1);
    c.tick(true, 2);
    CHECK(starts == 1);
    UnavailableTemperatures t;
    CHECK(t.start().state == SourceState::Unavailable);
    CHECK(c.active());
    c.tick(false, 3);
    CHECK(stops == 1);
    c.tick(true, 4);
    CHECK(starts == 2);
    c.stop();
    CHECK(stops == 2);
    c.tick(true, 10);
    c.fault({SourceState::Failed, 5, "consumer stopped"}, 100);
    CHECK(!c.active());
    CHECK(stops == 3);
    c.tick(true, 5099);
    CHECK(starts == 3);
    c.tick(true, 5100);
    CHECK(starts == 4);
    int attempts = 0;
    CollectionController bad(
        [&] {
            ++attempts;
            return SourceStatus{SourceState::Failed, 5, "denied"};
        },
        [] {});
    bad.tick(true, 0);
    bad.tick(true, 4999);
    CHECK(attempts == 1);
    bad.tick(true, 5000);
    CHECK(attempts == 2);
}
