#include "SetupLauncher.h"
#include "monitoring/win/WinHandle.h"
#include <QCoreApplication>
#include <QDir>
#include <windows.h>
#include <shellapi.h>
using namespace monitoring;
SetupLauncher::SetupLauncher(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<SetupRequest>();
    qRegisterMetaType<SetupResult>();
    connect(this, &SetupLauncher::finished, this, [this] { busy_ = false; });
}
SetupLauncher::~SetupLauncher()
{
    if (worker_.joinable())
        worker_.join();
}
void SetupLauncher::run(SetupRequest request)
{
    if (busy_)
        return;
    if (worker_.joinable())
        worker_.join();
    busy_ = true;
    auto path = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() +
                                         QStringLiteral("/QTaskManagerSetup.exe"))
                    .toStdWString();
    worker_ = std::thread([this, request, path] {
        if (request.withTemperatures || request.action > SetupAction::UpdateService)
        {
            emit finished({SetupOutcome::Unsupported, ERROR_NOT_SUPPORTED, false,
                           "Температуры пока не включены в эту сборку"});
            return;
        }
        std::wstring args = L"--action " + std::to_wstring(uint32_t(request.action));
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = path.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei))
        {
            auto error = GetLastError();
            emit finished(
                {error == ERROR_CANCELLED ? SetupOutcome::Cancelled : SetupOutcome::Failed, error, false,
                 error == ERROR_CANCELLED ? "Запрос UAC отменён" : "Не удалось запустить установщик"});
            return;
        }
        WinHandle process(sei.hProcess);
        if (!process)
        {
            emit finished({SetupOutcome::Failed, ERROR_INVALID_HANDLE, false, "Нет дескриптора установщика"});
            return;
        }
        WaitForSingleObject(process.get(), INFINITE);
        DWORD code{};
        if (!GetExitCodeProcess(process.get(), &code))
            code = GetLastError();
        if (code == SetupRollbackIncomplete)
        {
            emit finished({SetupOutcome::Partial, code, false,
                           "Не удалось полностью откатить обновление. Проверьте состояние службы и "
                           "резервный файл QTaskManagerService.bak."});
            return;
        }
        emit finished({code == 0 ? SetupOutcome::Success : SetupOutcome::Failed, code, false,
                       code == 0 ? "Операция завершена. Проверка состояния службы…"
                                 : "Операция завершилась с ошибкой"});
    });
}
