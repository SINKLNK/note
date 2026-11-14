#include "TestSupport.h"
#include "service/ClientLeases.h"
using namespace monitoring;
int main()
{
    ClientLeases l;
    CHECK(!l.shouldCollect(0));
    l.connected(1, 0);
    CHECK(l.shouldCollect(1));
    l.connected(2, 10);
    l.disconnected(1, 20);
    CHECK(l.shouldCollect(21));
    l.disconnected(2, 30);
    CHECK(l.shouldCollect(5029));
    CHECK(!l.shouldCollect(5030));
    l.connected(1, 6000);
    l.expire(20999);
    CHECK(l.count() == 1);
    l.expire(21000);
    CHECK(l.count() == 0);
    CHECK(l.shouldCollect(25999));
    CHECK(!l.shouldCollect(26000));
    l.connected(3, 27000);
    l.heartbeat(3, 28000);
    l.expire(42000);
    CHECK(l.count() == 1);
    l.disconnected(3, 43000);
    l.connected(4, 44000);
    CHECK(l.shouldCollect(50000));
}
