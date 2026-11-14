#include "SettingsPage.h"
#include "TestSupport.h"
#include <QApplication>
#include <QLabel>
using namespace monitoring;
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    SettingsState s;
    s.service.state = ServiceState::Missing;
    CHECK(allowedActions(s).install);
    CHECK(!allowedActions(s).remove);
    s.service.state = ServiceState::Disabled;
    CHECK(allowedActions(s).enable);
    CHECK(!allowedActions(s).install);
    s.service.state = ServiceState::AccessDenied;
    CHECK(!allowedActions(s).install);
    CHECK(!allowedActions(s).remove);
    s.service.state = ServiceState::Running;
    s.operationPending = true;
    CHECK(!allowedActions(s).disable);
    s.operationPending = false;
    CHECK(allowedActions(s).disable);
    s.live.protocolVersion = 2;
    CHECK(allowedActions(s).update);
    s.connected = true;
    s.diagnostics.attributionTotal.diskEvents = 100;
    s.diagnostics.attributionTotal.networkEvents = 200;
    s.diagnostics.attributionTotal.issues[size_t(AttributionIssue::DiskUnknownThread)] =
        {7, 3000000, 0, 99, 0};
    s.diagnostics.attributionInterval.issues[size_t(AttributionIssue::DiskUnknownThread)].events = 2;
    s.diagnostics.attributionTotal.issues[size_t(AttributionIssue::ProcessOpenFailed)] =
        {4, 0, 90, 0, 5};
    SettingsPage page;
    page.setState(s);
    auto *diagnostics = page.findChild<QLabel *>("MonitoringDiagnostics");
    CHECK(diagnostics);
    auto text = diagnostics->text();
    CHECK(text.contains(QStringLiteral("Файловый I/O")));
    CHECK(text.contains(QStringLiteral("Сеть")));
    CHECK(text.contains(QStringLiteral("7 / 100")));
    CHECK(text.contains(QStringLiteral("0 / 200")));
    CHECK(text.contains(QStringLiteral("3.000 MB")));
    CHECK(text.contains(QStringLiteral("TID=99")));
    CHECK(text.contains(QStringLiteral("OpenProcess")));
    CHECK(text.contains(QStringLiteral("Windows=5")));
    CHECK(text.contains(QStringLiteral("за интервал: 2")));
    s.connected = false;
    page.setState(s);
    CHECK(diagnostics->text().isEmpty());
}
