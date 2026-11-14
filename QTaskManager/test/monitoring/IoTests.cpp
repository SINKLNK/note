#include "TestSupport.h"
#include "service/IoAccumulator.h"
using namespace monitoring;
int main()
{
    IoAccumulator a;
    CHECK(a.sample(100, 1000).empty());
    a.add({1, 2}, IoKind::DiskRead, 1000000);
    a.add({1, 2}, IoKind::DiskWrite, 2000000);
    a.add({1, 2}, IoKind::NetSend, 1500000);
    auto s = a.sample(1600, 1000);
    CHECK(s.size() == 1);
    CHECK_NEAR(s[0].diskMBps, 2.0, 1e-9);
    CHECK_NEAR(s[0].networkMBps, 1.0, 1e-9);
    CHECK(a.sample(2600, 1000).empty());
    a.reset();
    CHECK(a.sample(3000, 1000).empty());
}
