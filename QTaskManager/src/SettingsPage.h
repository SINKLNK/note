#pragma once
#include "MonitoringClient.h"
#include "SetupLauncher.h"
#include <QWidget>
class QLabel;
class QPushButton;
struct SettingsState
{
    monitoring::ServiceInfo service;
    monitoring::Status live;
    monitoring::Diagnostics diagnostics;
    bool connected{}, operationPending{}, rebootRequired{}, startupOfferDismissed{};
    QString connectionError, lastResult;
};
struct SettingsActions
{
    bool install{}, enable{}, disable{}, remove{}, update{};
};
SettingsActions allowedActions(const SettingsState &);
class SettingsPage : public QWidget
{
    Q_OBJECT
  public:
    explicit SettingsPage(QWidget *parent = nullptr);
    void setState(const SettingsState &);
  signals:
    void actionRequested(monitoring::SetupRequest);

  private:
    QLabel *summary_, *sources_, *diagnostics_, *result_;
    QPushButton *install_, *enable_, *disable_, *remove_, *update_;
};
