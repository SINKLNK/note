#pragma once
#include "monitoring/MonitoringTypes.h"
#include "monitoring/ClientDelivery.h"
#include "monitoring/win/ServiceControl.h"
#include "monitoring/win/WinHandle.h"
#include <QObject>
#include <thread>
class QTimer;
Q_DECLARE_METATYPE(monitoring::Snapshot)
Q_DECLARE_METATYPE(monitoring::Status)
Q_DECLARE_METATYPE(monitoring::ServiceInfo)
class MonitoringClient : public QObject
{
    Q_OBJECT
  public:
    explicit MonitoringClient(QObject *parent = nullptr);
    ~MonitoringClient() override;
  public slots:
    void start();
    void stop();
  signals:
    void snapshotReady(const monitoring::Snapshot &);
    void statusChanged(const monitoring::Status &);
    void serviceChanged(const monitoring::ServiceInfo &);
    void connectionChanged(bool);
    void connectionError(const QString &);

  private:
    void run();
    monitoring::WinHandle stop_;
    std::thread worker_;
    QTimer *deliveryTimer_{};
    monitoring::ClientDelivery delivery_;
};
