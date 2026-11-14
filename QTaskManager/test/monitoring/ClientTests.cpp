#include "TestSupport.h"
#include "monitoring/ClientDelivery.h"
using namespace monitoring;
int main()
{
    ClientDelivery d;
    d.take(100);
    CHECK(d.responsive(101));
    CHECK(!d.responsive(15100));
    for (unsigned i = 0; i < 10000; ++i)
    {
        Snapshot s;
        s.sequence = i;
        d.snapshot(std::move(s));
    }
    auto latest = d.take(16000);
    CHECK(latest.snapshot);
    CHECK(latest.snapshot->sequence == 9999);
    CHECK(!d.take(16001).snapshot);
    CHECK(d.responsive(16002));
    d.snapshot({});
    d.connection(false);
    CHECK(!d.take(17000).snapshot);
}
