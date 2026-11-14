#include "TestSupport.h"
#include "service/ProcessIdentity.h"
using namespace monitoring;
int main()
{
    ProcessIdentity m;
    m.processStart({7, 10}, 100);
    m.threadStart(8, {7, 10}, 110);
    CHECK(m.resolveTid(8, 120) == (ProcessKey{7, 10}));
    m.threadEnd(8, 130);
    m.processEnd(7, 140);
    m.processStart({7, 20}, 150);
    m.threadStart(8, {7, 20}, 160);
    CHECK(m.resolveTid(8, 120) == (ProcessKey{7, 10}));
    CHECK(!m.resolveTid(8, 145));
    CHECK(m.resolvePid(7, 170) == (ProcessKey{7, 20}));
    CHECK(!m.resolvePid(7, 90));
    CHECK(m.live().size() == 1);
    m.prune(150);
    CHECK(!m.resolvePid(7, 120));
    // A late end of the old lifetime must not close the new one.
    m.processEnd(7, 140);
    CHECK(m.resolvePid(7, 170) == (ProcessKey{7, 20}));
}
