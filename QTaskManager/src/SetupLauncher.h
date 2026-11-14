#pragma once
#include "monitoring/SetupTypes.h"
#include <QObject>
#include <thread>
Q_DECLARE_METATYPE(monitoring::SetupRequest)
Q_DECLARE_METATYPE(monitoring::SetupResult)
class SetupLauncher : public QObject
{
    Q_OBJECT
  public:
    explicit SetupLauncher(QObject *parent = nullptr);
    ~SetupLauncher() override;
  public slots:
    void run(monitoring::SetupRequest request);
  signals:
    void finished(const monitoring::SetupResult &result);

  private:
    std::thread worker_;
    bool busy_{};
};
