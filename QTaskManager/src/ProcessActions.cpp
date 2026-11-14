#include "ProcessActions.h"

#include <QMessageBox>
#include <QWidget>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace ProcessActions
{
#ifdef Q_OS_WIN
void *openSameProcess(QWidget *parent, const ProcessEntry &entry, unsigned long access)
{
    HANDLE process = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.pid);
    if (!process) {
        QMessageBox::warning(parent, QStringLiteral("Ошибка"), QStringLiteral("Не удалось открыть процесс."));
        return nullptr;
    }

    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (entry.creationTimeTicks != 0
        && GetProcessTimes(process, &creation, &exitTime, &kernel, &user)) {
        ULARGE_INTEGER current{};
        current.LowPart = creation.dwLowDateTime;
        current.HighPart = creation.dwHighDateTime;

        if (current.QuadPart != entry.creationTimeTicks) {
            CloseHandle(process);
            QMessageBox::warning(parent,
                                 QStringLiteral("Ошибка"),
                                 QStringLiteral("%1 уже завершился, а PID %2 занят другим "
                                                "процессом. Ничего не сделано.")
                                     .arg(entry.name)
                                     .arg(entry.pid));
            return nullptr;
        }
    }

    return process;
}
#endif

bool terminateProcess(QWidget *parent, const ProcessEntry &entry)
{
#ifdef Q_OS_WIN
    const auto answer = QMessageBox::question(parent,
                                              QStringLiteral("Завершить задачу"),
                                              QStringLiteral("Завершить %1 (PID %2)?")
                                                  .arg(entry.name)
                                                  .arg(entry.pid));
    if (answer != QMessageBox::Yes) {
        return false;
    }

    HANDLE process = openSameProcess(parent, entry, PROCESS_TERMINATE);
    if (!process) {
        return false;
    }

    const bool ok = TerminateProcess(process, 1);
    CloseHandle(process);
    if (!ok) {
        QMessageBox::warning(parent, QStringLiteral("Ошибка"), QStringLiteral("Не удалось завершить процесс."));
    }
    return ok;
#else
    Q_UNUSED(entry);
    QMessageBox::information(parent, QStringLiteral("Unsupported"), QStringLiteral("Только на Windows."));
    return false;
#endif
}
} // namespace ProcessActions
