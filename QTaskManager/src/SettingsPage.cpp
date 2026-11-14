#include "SettingsPage.h"
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
using namespace monitoring;
SettingsActions allowedActions(const SettingsState &s)
{
    SettingsActions a;
    if (s.operationPending)
        return a;
    switch (s.service.state)
    {
    case ServiceState::Missing:
        a.install = true;
        break;
    case ServiceState::Disabled:
    case ServiceState::Stopped:
        a.enable = a.remove = a.update = true;
        break;
    case ServiceState::Running:
        a.disable = a.remove = a.update = true;
        break;
    default:
        break;
    }
    return a;
}
namespace
{
QString sourceText(const SourceStatus &s)
{
    QString text;
    switch (s.state)
    {
    case SourceState::Unavailable:
        text = QStringLiteral("недоступно");
        break;
    case SourceState::WarmingUp:
        text = QStringLiteral("первое измерение");
        break;
    case SourceState::Ready:
        text = QStringLiteral("работает");
        break;
    case SourceState::Partial:
        text = QStringLiteral("неполные данные");
        break;
    case SourceState::Failed:
        text = QStringLiteral("ошибка");
        break;
    }
    if (s.win32Error)
        text += QStringLiteral(" (код %1)").arg(s.win32Error);
    return text;
}
QString stateText(ServiceState s)
{
    switch (s)
    {
    case ServiceState::Missing:
        return QStringLiteral("Не установлена");
    case ServiceState::Stopped:
        return QStringLiteral("Остановлена");
    case ServiceState::Starting:
        return QStringLiteral("Запускается");
    case ServiceState::Running:
        return QStringLiteral("Работает");
    case ServiceState::Stopping:
        return QStringLiteral("Останавливается");
    case ServiceState::Disabled:
        return QStringLiteral("Отключена");
    case ServiceState::AccessDenied:
        return QStringLiteral("Нет доступа к состоянию");
    default:
        return QStringLiteral("Ошибка проверки состояния");
    }
}
QString diagnosticsText(const Diagnostics &d)
{
    QStringList lines;
    lines << QStringLiteral("ETW за сеанс сбора: потери событий %1 · буферов %2 · ошибки схем %3%4")
                 .arg(d.lostEvents).arg(d.lostBuffers).arg(d.unsupportedSchemas)
                 .arg(d.truncated ? QStringLiteral(" · данные ограничены") : QString());
    const auto &total = d.attributionTotal;
    const auto &interval = d.attributionInterval;
    auto combined = [](const AttributionCounters &c, AttributionIssue a, AttributionIssue b) {
        AttributionCount result;
        result.events = c.issues[size_t(a)].events + c.issues[size_t(b)].events;
        result.bytes = c.issues[size_t(a)].bytes + c.issues[size_t(b)].bytes;
        return result;
    };
    auto ioLine = [&](const QString &name, uint64_t events, AttributionIssue a, AttributionIssue b) {
        auto all = combined(total, a, b);
        auto recent = combined(interval, a, b);
        lines << QStringLiteral("%1: не сопоставлено %2 / %3 операций; за интервал: %4; "
                                "объём без владельца: %5 MB (за интервал: %6 MB)")
                     .arg(name).arg(all.events).arg(events).arg(recent.events)
                     .arg(double(all.bytes) / 1000000.0, 0, 'f', 3)
                     .arg(double(recent.bytes) / 1000000.0, 0, 'f', 3);
    };
    ioLine(QStringLiteral("Файловый I/O"), total.diskEvents,
           AttributionIssue::DiskMissingThreadId, AttributionIssue::DiskUnknownThread);
    ioLine(QStringLiteral("Сеть"), total.networkEvents,
           AttributionIssue::NetworkMissingProcessId, AttributionIssue::NetworkUnknownProcess);
    uint64_t lifecycleTotal = 0, lifecycleInterval = 0;
    for (size_t i = size_t(AttributionIssue::ProcessOpenFailed); i < total.issues.size(); ++i)
    {
        lifecycleTotal += total.issues[i].events;
        lifecycleInterval += interval.issues[i].events;
    }
    lines << QStringLiteral("Процессы/потоки: ошибок сопоставления %1; за интервал: %2")
                 .arg(lifecycleTotal).arg(lifecycleInterval);
    static const std::array<const char *, size_t(AttributionIssue::Count)> reasons = {
        "Файловый I/O: в событии отсутствует TID",
        "Файловый I/O: владелец TID не найден на момент запроса",
        "Сеть: в событии отсутствует PID",
        "Сеть: процесс PID не найден на момент операции",
        "Процесс: OpenProcess не выполнен",
        "Процесс: GetProcessTimes не выполнен",
        "Процесс: время создания новее события (PID переиспользован или рассогласование времени)",
        "Поток: процесс-владелец не найден на момент события"};
    for (size_t i = 0; i < total.issues.size(); ++i)
    {
        const auto &count = total.issues[i];
        if (!count.events)
            continue;
        QString last;
        if (i <= size_t(AttributionIssue::DiskUnknownThread))
            last = QStringLiteral("TID=%1").arg(count.lastTid);
        else
            last = QStringLiteral("PID=%1").arg(count.lastPid);
        if (i == size_t(AttributionIssue::ThreadUnknownProcess))
            last += QStringLiteral(", TID=%1").arg(count.lastTid);
        if (count.lastError)
            last += QStringLiteral(", Windows=%1").arg(count.lastError);
        lines << QStringLiteral("%1: %2; за интервал: %3; последний: %4")
                     .arg(QString::fromUtf8(reasons[i])).arg(count.events).arg(interval.issues[i].events)
                     .arg(last);
    }
    lines << QStringLiteral("Счётчики сбрасываются при перезапуске сбора. Интервал — последний снимок службы. "
                            "Ошибки процессов/потоков сами по себе не означают пропуск файловых или сетевых операций.");
    return lines.join(QLatin1Char('\n'));
}
} // namespace
SettingsPage::SettingsPage(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *contents = new QWidget(scroll);
    scroll->setWidget(contents);
    outer->addWidget(scroll);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(18);
    auto *title = new QLabel(QStringLiteral("Настройки"), this);
    auto font = title->font();
    font.setPointSize(20);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto *group = new QGroupBox(QStringLiteral("Служба мониторинга"), this);
    auto *body = new QVBoxLayout(group);
    body->setContentsMargins(18, 22, 18, 18);
    body->setSpacing(12);
    summary_ = new QLabel(group);
    sources_ = new QLabel(group);
    diagnostics_ = new QLabel(group);
    diagnostics_->setObjectName(QStringLiteral("MonitoringDiagnostics"));
    diagnostics_->setTextFormat(Qt::PlainText);
    result_ = new QLabel(group);
    for (auto *label : {summary_, sources_, diagnostics_, result_})
    {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(label);
    }
    auto *row = new QHBoxLayout;
    body->addLayout(row);
    auto add = [&](const QString &text, SetupAction action) {
        auto *button = new QPushButton(text, group);
        button->setMinimumHeight(32);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action] {
            if (action == SetupAction::RemoveService &&
                QMessageBox::question(
                    this, QStringLiteral("Удаление службы"),
                    QStringLiteral("Удалить службу мониторинга? Данные файлового I/O и сети станут недоступны. "
                                   "PawnIO и запись о его происхождении будут сохранены.")) !=
                    QMessageBox::Yes)
                return;
            emit actionRequested({action, false});
        });
        return button;
    };
    install_ = add(QStringLiteral("Установить…"), SetupAction::InstallService);
    enable_ = add(QStringLiteral("Включить…"), SetupAction::EnableService);
    disable_ = add(QStringLiteral("Отключить…"), SetupAction::DisableService);
    update_ = add(QStringLiteral("Обновить…"), SetupAction::UpdateService);
    remove_ = add(QStringLiteral("Удалить…"), SetupAction::RemoveService);
    row->addStretch();
    auto *note = new QLabel(QStringLiteral("Изменения службы требуют подтверждения UAC. Сбор работает только "
                                           "при подключённом QTaskManager."),
                            group);
    note->setWordWrap(true);
    body->addWidget(note);
    layout->addWidget(group);
    auto *temperature = new QGroupBox(QStringLiteral("Температура CPU / PawnIO"), this);
    auto *tl = new QVBoxLayout(temperature);
    tl->setContentsMargins(18, 22, 18, 18);
    auto *text = new QLabel(
        QStringLiteral(
            "Компонент температур ещё не включён в эту сборку.\nУстановка PawnIO и датчики физических ядер "
            "появятся на следующем этапе. Файловый I/O и сеть работают независимо от драйвера."),
        temperature);
    text->setWordWrap(true);
    tl->addWidget(text);
    layout->addWidget(temperature);
    layout->addStretch();
    setState({});
}
void SettingsPage::setState(const SettingsState &s)
{
    auto a = allowedActions(s);
    install_->setEnabled(a.install);
    enable_->setEnabled(a.enable);
    disable_->setEnabled(a.disable);
    remove_->setEnabled(a.remove);
    update_->setEnabled(a.update);
    QString description =
        QStringLiteral("%1\nАвтозапуск: %2 · Соединение: %3")
            .arg(stateText(s.service.state),
                 s.service.automatic ? QStringLiteral("включён") : QStringLiteral("выключен"),
                 s.connected ? QStringLiteral("подключено") : QStringLiteral("нет"));
    if (s.connected)
        description += QStringLiteral("\nВерсия службы: %1 · Протокол: %2")
                           .arg(QString::fromStdString(s.live.serviceVersion))
                           .arg(s.live.protocolVersion);
    if (s.service.error)
        description += QStringLiteral("\nКод Windows: %1").arg(s.service.error);
    summary_->setText(description);
    sources_->setText(
        s.connected
            ? QStringLiteral("Файловый I/O: %1 · Сеть: %2").arg(sourceText(s.live.disk), sourceText(s.live.network))
            : QStringLiteral("Файловый I/O и сеть: ожидание подключения"));
    sources_->setToolTip(QString::fromStdString(s.live.disk.detail + "\n" + s.live.network.detail));
    diagnostics_->setText(s.connected ? diagnosticsText(s.diagnostics) : QString());
    result_->setText(s.operationPending ? QStringLiteral("Выполняется операция…")
                                        : s.lastResult + (!s.connectionError.isEmpty()
                                                              ? QStringLiteral("\n") + s.connectionError
                                                              : QString()));
}
