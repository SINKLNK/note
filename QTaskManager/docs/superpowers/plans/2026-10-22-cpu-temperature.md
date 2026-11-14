# Optional PawnIO and Physical-Core Temperature Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Добавить необязательные температуры физических ядер, выбор при установке и управление PawnIO с удалением только установленного QTaskManager драйвера.

**Architecture:** Температурный сборщик работает внутри уже созданной службы и отдаёт датчики через её Snapshot. GUI получает топологию CPU независимо от драйвера и показывает температуру один раз на физическое ядро. Setup устанавливает официальный подписанный пакет и проверяет защищённые сведения о происхождении перед удалением.

**Tech Stack:** C++20, Windows SDK (CPU topology, registry, SCM, Authenticode), официальный PawnIO и подписанные модули, Qt 6 Widgets, CMake/CTest.

**Spec:** [Утверждённая спецификация](../specs/2026-10-22-monitoring-service-design.md).

## Global Constraints

- Зависимость: [план A — служба и ETW](2026-10-22-monitoring-service.md), включая MonitoringTypes, TemperatureSource, SetupActions и SettingsPage.
- Основной GUI запускается с обычными правами. Все аппаратные обращения через службу, административные действия через setup с UAC.
- При установке пользователь явно выбирает, нужны ли температуры. При отказе PawnIO не устанавливается и температуры не отображаются.
- Отсутствие PawnIO не мешает ETW и существующим метрикам приложения.
- Удаление доступно только для драйвера, установленного QTaskManager; ранее установленный сторонний драйвер не удаляется.
- Отсутствующая, неоднозначная или не соответствующая текущему пакету запись означает, что удаление запрещено.
- Температура показывается один раз на физическое ядро, без дублирования на его SMT-потоках.
- Существующие графики загрузки логических процессоров сохраняются; чётность номера CPU не используется как эвристика.
- Опрос примерно раз в секунду, один раз на датчик, только при подключённом GUI и включённой функции.
- Подпись драйвера не заменяет проверку поддержки CPU и валидности показаний. Начальная целевая проверка — Intel Core i7-12700K; для неподдерживаемого CPU — честная недоступность.
- Автоматической перезагрузки нет; состояние удаления определяется результатом реальной операции.
- Не устанавливать, обновлять и удалять драйвер на машине пользователя без отдельного согласия на такой тест.

## Review Focus

1. PawnIO появился до QTaskManager, запись потеряна или пакет заменён: удаление запрещено и в GUI, и в setup (B1, B2).
2. Наш драйвер используется другой программой: не закрывать чужие handles и не обещать немедленную выгрузку (B2, B6).
3. Hybrid/SMT, несколько processor groups, отсутствующий PDH sample: температура не перепрыгивает на соседнее ядро (B3, B5).
4. Подписанный драйвер установлен, но модуль/CPU/датчик несовместим: ETW работает, ложной температуры нет (B1, B4).
5. Отказ от температур, частичная установка, отмена UAC и повторная установка GUI: не включать сбор самовольно и не терять сведения о владении (B2, B5, B6).

## Файлы и границы

| Пути | Назначение |
| --- | --- |
| packaging/pawnio/contract.md, payloads.json, ThirdPartyNotices.txt | Проверенный релиз, параметры операций, контроль целостности и происхождение компонентов |
| cmake/PawnIoPayloads.cmake | Преобразование проверенного package manifest во встроенный каталог setup |
| src/setup/PawnIoPackage.{h,cpp}, PawnIoOwnership.{h,cpp} | Официальный пакет и защищённая политика удаления |
| src/monitoring/CpuTopology.{h,cpp} | Physical core ↔ processor group/logical CPU, без Qt и PawnIO |
| src/service/PawnIoSession.{h,cpp}, IntelTemperatureSource.{h,cpp} | Жизненный цикл драйверного соединения и поддержанный Intel backend |
| src/CoreTemperaturePresentation.{h,cpp} | Сопоставление датчиков с графиками, подписи |
| test/monitoring/PawnIoPackageTests.cpp, OwnershipTests.cpp, TopologyTests.cpp, TemperatureTests.cpp, TemperaturePresentationTests.cpp | Детерминированная проверка внешних отказов и сопоставления |
| test/monitoring/TemperatureIntegration.ps1, results/ | Явные аппаратные/install/uninstall проверки |

Изменить SetupActions/SetupLauncher/SettingsPage/MonitoringService из плана A; PerformanceMonitor, PerformancePage, PerfGraph — только в точках подключения топологии и подписей. GUI не начинает напрямую использовать PawnIOLib. Собственные драйверы, универсальные операции записи регистров и поддержка всех семейств CPU не входят в этап.

## Проверки и сборка

Использовать build-monitoring, CTest и TestSupport.h из плана A. Перед каждым product change — целевой red, затем green. Сборка: `cmake --build build-monitoring`; конкретный тест: `ctest --test-dir build-monitoring --output-on-failure -R '^NAME$'`. Обычные тесты используют fake package/driver и не открывают устройство. GUI-проверки используют QApplication без нового test framework.

## B1. Проверить и закрепить контракт официального PawnIO

**Files:** создать packaging/pawnio/contract.md, payloads.json, ThirdPartyNotices.txt, cmake/PawnIoPayloads.cmake, src/setup/PawnIoPackage.{h,cpp}, test/monitoring/PawnIoPackageTests.cpp; изменить cmake/MonitoringPackage.cmake и src/monitoring/MonitoringTypes.h.

**Interfaces:** в MonitoringTypes.h определить `PawnIoPackageInfo {std::string version; std::string packageId; std::string sha256; bool signatureValid;}` и `PawnIoDetection {bool installed; bool compatible; PawnIoPackageInfo package; uint32_t error;}`. `PawnIoPackage::detect() -> PawnIoDetection`, `verifyPayloads() -> SetupResult`, `install() -> SetupResult`, `uninstall(const PawnIoPackageInfo&) -> SetupResult`. `PawnIoPackageBackend` — injectable интерфейс external process/signature/device inventory, fake для тестов; product adapter запускает только зафиксированные операции. GUI использует общие DTO, не линкуется с привилегированным setup-кодом.

- [ ] Проверить официальный релиз и модули без установки: скачать в рабочую staging-папку, проверить Authenticode и SHA-256, инвентаризировать пакет и официальные сведения об install/uninstall. Начальный кандидат — обнаруженный при проектировании 2.2.0; перепроверить его доступность и выбрать один релиз, не подставлять latest во время установки. В contract.md записать реальные executable names, точные параметры, exit codes/признак reboot и метод идентификации установленного пакета. Нельзя угадывать silent switches или запускать setup «для справки», если это может установить драйвер. При отсутствии документированного silent interface использовать официальный интерактивный setup и проверку состояния после завершения.
- [ ] Составить payloads.json с реальными versions/hashes, происхождением и лицензиями driver/library/modules; ThirdPartyNotices включает применимые условия распространения. Не копировать код LHM в проект без соблюдения его лицензии. Бинарные payloads размещать в build/staging, не коммитить случайно в исходники. Сборка bundle проверяет каталог; elevated setup использует встроенные записи, а не доверяет внешнему редактируемому JSON.
- [ ] PawnIoPackageTests: `CHECK(adapter.verifyPayloads().outcome == SetupOutcome::Failed)` для изменённого файла/неверного signer; неизвестный exit code→Failed, cancelled→Cancelled; уже установленная совместимая версия→no install; installed-but-incompatible→явная причина без молчаливого downgrade. Fake reboot-required→`CHECK(result.rebootRequired)`; неподтверждённый результат не success.
- [ ] Запустить PawnIoPackageTests, получить red; реализовать adapter по проверенному contract.md. При подтверждении контрактом, что окончательный признак reboot доступен только после реальной установки, включить эту проверку в B6 и не заявлять до неё поддержку тихого полного цикла. Нельзя заменять отсутствующий контракт набором предполагаемых ключей.
- [ ] PawnIoPackageTests PASS, package target проверяет подписи/хэши; commit `build: pin and verify official PawnIO payloads`.

## B2. Владение, установка и удаление

**Files:** создать PawnIoOwnership.{h,cpp}, test/monitoring/OwnershipTests.cpp; изменить SetupActions.{h,cpp}, ServiceConfig.{h,cpp}, MonitoringService.{h,cpp}, MonitoringTypes.h, SetupTests.cpp.

**Interfaces:** общий `Ownership {Absent, QTaskManager, External, Unknown}` в MonitoringTypes.h; приватный `OwnershipRecord {uint32_t schemaVersion; std::string installId; std::string packageId; std::string version; std::string sha256; bool removalPending;}`; `PawnIoOwnership::classify(const PawnIoDetection&) -> Ownership`, `recordSuccessfulInstall(const PawnIoDetection& before, const PawnIoDetection& after) -> uint32_t`, `mayRemove(const PawnIoDetection&) -> bool`, `confirmRemoved() -> uint32_t`. Хранилище HKLM `Software\QTaskManager\Monitoring\PawnIO`, запись только admin/SYSTEM. При externally discovered install сохранять External; потеря записи даёт Unknown, не QTaskManager.

- [ ] OwnershipTests с fake inventory/registry: `CHECK(!ownership.mayRemove(preexisting)); CHECK(!ownership.mayRemove(changedPackage)); CHECK(!ownership.mayRemove(missingRecord));`; успешная собственная установка после confirmed absence→true, interrupted/failed install→false, сторонний upgrade→не присваивает владение. RemoveService сохраняет запись при оставшемся драйвере, повторная установка службы её не стирает. Прямой вызов RemovePawnIo в обход GUI отклоняется.
- [ ] Запустить OwnershipTests/SetupTests, получить red.
- [ ] Реализовать machine-wide mutex административных операций и повторную инвентаризацию перед действием. Если доказать происхождение нельзя (включая гонку со сторонним installer), сохранить Unknown и запретить удаление. Сверять текущий package identity с защищённой записью непосредственно перед uninstall; запись о владении не является доказательством отсутствия сторонних клиентов.
- [ ] Реализовать SetupRequest::withTemperatures: сначала работоспособная служба, затем optional PawnIO; при отказе функция false даже при существующем драйвере. При ошибке PawnIO возвращать Partial и оставлять ETW. Для InstallPawnIo проверять/создавать необходимые компоненты, но не включать намеренно отключённую службу. Enable/DisableTemperatures обновляют глобальную конфигурацию через setup и уведомляют службу защищённым событием.
- [ ] Для RemovePawnIo после ownership check записать temperatureEnabled=false и removalPending=true; уведомить работающую службу освободить PawnIO. Подтверждение освобождения через приватный admin/SYSTEM-only maintenance pipe, отдельный от public telemetry pipe. Не начинать удаление до подтверждения или установленного факта отсутствия процесса нашей службы; timeout 10 секунд→Failed с объяснением, не остановка ETW. В отключённом состоянии службу ради операции не запускать. После штатного удаления сверить inventory: удалить ownership record только при confirmed absence; при reboot-required оставить pending до следующей проверки. Не завершать сторонние процессы и не удалять драйвер автоматом вместе с нашей службой.
- [ ] OwnershipTests/SetupTests PASS, lifecycle-test подтверждает сохранение ETW при release PawnIO. Commit `feat: manage optional PawnIO with verified installation ownership`.

## B3. Топология CPU и устойчивые идентификаторы графиков

**Files:** создать src/monitoring/CpuTopology.{h,cpp}, test/monitoring/TopologyTests.cpp; изменить src/PerformanceMonitor.{h,cpp}, src/monitoring/MonitoringTypes.h, WireCodec.{h,cpp} и ProtocolTests.cpp.

**Interfaces:** `CoreKind {Unknown, Performance, Efficiency}`; `PhysicalCore {uint32_t packageId; uint32_t coreId; CoreKind kind; std::vector<LogicalCpuId> logicalCpus;}`; `CpuTopology {uint64_t generation; std::vector<PhysicalCore> cores;}`; `readCpuTopology() -> CpuTopology`; чистый `decodeCpuTopology(std::span<const std::byte> windowsRecords) -> CpuTopology` для fixtures. SystemInfo получает CpuTopology; PerformanceSample получает `QVector<LogicalCpuId> coreIds`, соответствующий coreUsage/coreKernel, не индексированный по догадке.

- [ ] TopologyTests: synthetic 8P×2+4E×1→12 физических/20 логических; два processor groups; нечетные/непоследовательные номера SMT; unsupported class→Unknown; truncated Windows records→ошибка/пустая топология без выхода за границы. Fixture с отсутствующим PDH instance: `CHECK(sample.coreIds[1] == expectedId)` и показания остаются привязаны по ID, не смещаются.
- [ ] Запустить TopologyTests и получить red.
- [ ] Реализовать Windows topology API и сопоставление package/core/group; P/E классификация только по проверенным признакам платформы, на неизвестной платформе Unknown. Изменить perProcessor в PerformanceMonitor так, чтобы сохранять пары group/index и совмещать utility/kernel по ID, а не позиции двух независимо отсортированных массивов. Топология доступна без службы/PawnIO; идентификаторы детерминированы одинаково в GUI и службе, generation — fingerprint нормализованного набора связей. Добавить topologyGeneration в Snapshot и wire codec; mismatch запрещает привязку температуры до обновления топологии. Affinity всегда group-aware.
- [ ] TopologyTests PASS и текущая загрузка CPU сохраняет смысл; commit `feat: preserve physical-core topology and logical CPU identities`.

## B4. Подписанные модули и Intel temperatures в службе

**Files:** создать PawnIoSession.{h,cpp}, IntelTemperatureSource.{h,cpp}, test/monitoring/TemperatureTests.cpp; изменить MonitoringService.{h,cpp} и MonitoringTargets.cmake.

**Interfaces:** `PawnIoSession::open() -> SourceStatus`, `close()`, `readIntelThermal(LogicalCpuId) -> ThermalRegisters`; `ThermalRegisters {bool readOk; uint32_t target; uint32_t status;}`. IntelTemperatureSource реализует TemperatureSource плана A, конструктор принимает CpuTopology и adapter PawnIoSession. В production allowlist операций соответствует выбранному IntelMSR, пути модулей фиксированы в защищённом каталоге; public pipe не принимает номера регистров.

- [ ] TemperatureTests: mock target 100 °C и valid delta 35→`CHECK_NEAR(sensor.celsius, 65.0, 1e-9)`; invalid validity bit/read error→`CHECK(!sensor.valid)`; один read на physical-core representative; дополнительный SMT не вызывает второй read; неподдерживаемый CPU→Unavailable. Disabled/no clients→zero driver opens; disconnect grace→close; module mismatch не ломает ETW. Group/package sensor не притворяется Core.
- [ ] Запустить TemperatureTests и получить red.
- [ ] Реализовать whitelist поддержанных Intel/CPUID features, чтение temperature target и thermal status через проверенный подписанный модуль, сохранение/восстановление thread affinity на каждом пути. Не предполагать TjMax=100 для всех CPU. Включить поддержку целевой Alder Lake только после проверки схемы модуля; unknown CPU даёт диагностируемый Unavailable. Открывать driver/module один раз на активную сессию, не на каждый тик.
- [ ] Подключить collector к coordinator, sample раз в 1000 мс, отдельное состояние ошибок; guarded backoff при отказах, snapshot публикуется независимо от медленного температурного источника. При переопределении config и removalPending освобождать driver, отправлять maintenance ack только после реального закрытия. При возобновлении после сна заново проверять topology/handles.
- [ ] TemperatureTests PASS; service собирается без Qt. Commit `feat: collect optional Intel core temperatures through PawnIO`.

## B5. Выбор при установке, управление драйвером и подписи

**Files:** создать CoreTemperaturePresentation.{h,cpp}, TemperaturePresentationTests.cpp; изменить SettingsPage.{h,cpp}, SetupLauncher.{h,cpp}, MainWindow.{h,cpp}, PerformancePage.{h,cpp}, PerfGraph.{h,cpp}, SettingsTests.cpp, MonitoringTypes.h, WireCodec.{h,cpp}.

**Interfaces:** `PawnIoState {PawnIoDetection detection; Ownership ownership; bool enabled; bool removalPending; bool rebootRequired; SourceStatus sensors;}` добавляется в SettingsState и Status. `CoreCaption {LogicalCpuId cpu; QString text; QString tooltip;}`; `makeCoreCaptions(const CpuTopology&, std::span<const SensorReading>, bool showTemperatures) -> QVector<CoreCaption>`. `PerformancePage::setServiceSnapshot(const monitoring::Snapshot&)`, `clearServiceSnapshot()`. `PerfGraph::setCaption(const QString&, const QString&)` обновляет только overlay/caption, не history. Расширение wire schema синхронизировать в service/client/setup, повысить protocolVersion если нарушена совместимость v1; тест несовместимости обязателен.

- [ ] TemperaturePresentationTests: 12 физических/20 логических → максимум 12 core-temperature подписей; SMT labels не содержат °C; missing sensor→нет выдуманного числа; disconnected snapshot→температура очищена, topology labels остаются; группа датчика не дублируется как per-core. Не соединять датчики и PDH по порядковому индексу. SettingsTests: External/Unknown→remove disabled, QTaskManager→remove enabled, operationPending→all administrative actions disabled.
- [ ] Запустить TemperaturePresentationTests/SettingsTests, получить red.
- [ ] Заменить placeholder плана A настоящим optional checkbox (по умолчанию выключен) в предложении установки до UAC. Существующий PawnIO показывать как «Использовать установленный PawnIO», не обещать повторную установку. При InstallPawnIo спустя время пользователь получает тот же setup flow. Pending reboot и partial setup видны отдельно от installed/running; обновить действия после фактической проверки состояния. Глобальные настройки не хранить в HKCU elevated пользователя.
- [ ] Добавить температуры на одном стабильном представителе physical core, его SMT-графики подписать без повторения числа; E-core с единственным потоком получает свою подпись. Отобразить только валидные датчики; package/group значения вынести в соответствующую подпись/статистику, не рассредоточивать по ядрам. Сохранять graph history, hover и существующую компоновку. Сервисная freshness 3000 мс из A6 применяется и к температурам.
- [ ] Тесты PASS; визуально проверить 100/150/200% DPI, короткие/длинные подписи, минимум окна и увеличение высоты, gear внизу. Commit `feat: expose optional temperatures and PawnIO controls in settings`.

## B6. Проверить полный путь и завершить поставку

**Files:** создать test/monitoring/TemperatureIntegration.ps1; изменить test/monitoring/README.md, README.md проекта, cmake/MonitoringPackage.cmake, packaging/pawnio/contract.md; результаты — test/monitoring/results/.

**Interfaces:** `TemperatureIntegration.ps1 -Scenario <Inspect|Install|Compare|Uninstall|RebootCheck>`; Inspect read-only, остальные сценарии запускаются отдельно по согласованию. Скрипт не пытается доказать exclusive ownership сканированием случайных процессов и не удаляет сторонний драйвер.

- [ ] Добавить сценарии: установка без температур; поздняя установка; прежний сторонний PawnIO; наша установка и перезапуск/переустановка GUI; отмена UAC; partial failure; удаление нашей службы с оставлением driver ownership. Каждое действие проверяет результат из SCM/registry/inventory, а не только exit code.
- [ ] На разрешённой машине проверить i7-12700K: сопоставление P/E/SMT, число реальных датчиков, покой и CPU load, сравнение с независимым монитором в близкие моменты времени. Записать readings и разброс; не объявлять точное совпадение обязательным из-за разных моментов опроса. Отдельно измерить стоимость температур off/on и no-client idle.
- [ ] Проверить удаление нашей установки без стороннего клиента и со сторонним монитором: release своих handles, отсутствие убийства чужого процесса, корректные Failed/RebootRequired/Success. Для externally installed/Unknown убедиться, что elevated setup отклоняет RemovePawnIo, не вызывая deinstaller. Результат pending reboot проверить после согласованной ручной перезагрузки; не выдавать пропуск за PASS.
- [ ] Выполнить общий `ctest --test-dir build-monitoring --output-on-failure -L monitoring-unit`, Release build и package; проверить package на чистой тестовой системе: runtime dependencies, manifests, отсутствие требования Qt у service/setup. Обновить docs реальными параметрами официального setup и ограничениями поддержки.
- [ ] Выполнить общий review privilege boundary/ownership/ETW attribution, устранить выявленные ошибки, `git diff --check`, commit `test: validate optional temperature installation and removal`.

## Self-review и передача

Покрытие спецификации: подписи/optional choice B1/B5; ownership и удаление B2/B6; физические ядра B3/B5; аппаратный сбор B4; цена/сценарии reboot B6. Все риски Review Focus имеют владельца и проверку. Публичный протокол не предоставляет hardware write или arbitrary IOCTL. Конкретные параметры внешнего installer должны быть подтверждены B1, а не придуманы планом.

Задачи ещё не выполнены. Перед началом реализации пользователь проверяет оба плана и выбирает выполнение в текущей сессии или с отдельными исполнителями/ревьюерами. После этого работать по выбранному workflow, сохраняя ограничения спецификации.
