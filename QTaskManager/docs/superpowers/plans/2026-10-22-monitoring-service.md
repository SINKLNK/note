# Monitoring Service and ETW Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Заполнить диск/сеть по процессам через автоматически запускаемую службу, сохранив непривилегированный GUI и управление компонентами в настройках.

**Architecture:** Native C++ Windows service получает ETW, агрегирует данные и передаёт их по локальному named pipe единому клиенту внутри GUI. Отдельный повышенный setup управляет установкой и SCM; GUI и публичный канал службы не выполняют административные операции. Температуры добавляются вторым планом поверх этих же интерфейсов.

**Tech Stack:** C++20, Windows SDK (SCM, ETW/TDH, named pipes), Qt 6 Widgets/Svg для GUI, CMake/CTest, MSVC x64. Служба и setup не зависят от Qt.

**Spec:** [Утверждённая спецификация](../specs/2026-10-22-monitoring-service-design.md).

## Global Constraints

- QTaskManagerService автоматически запускается при загрузке Windows.
- ETW и опрос температур работают только пока подключён хотя бы один GUI.
- Основной GUI запускается с обычными правами; административные операции выполняет отдельный setup с UAC.
- Остановка сбора через 5 секунд после ухода последнего клиента; heartbeat timeout — 15 секунд.
- Обновление агрегатов раз в секунду; MB = 1 000 000 байтов; интервал монотонный, фактический.
- Отсутствие PawnIO не мешает ETW и существующим метрикам приложения.
- Шестерёнка закреплена у нижнего края левой панели; «Отключить службу» = остановить и отключить автозапуск.
- PID сопоставляется с временем создания; неизвестные/устаревшие данные не превращаются в ноль.
- Наши файлы — в защищённом Program Files; глобальное состояние — в защищённом HKLM. Не доверять пути или политике удаления, полученным от GUI.
- Системные install/uninstall проверки запускаются только с отдельным согласием пользователя; обычный CTest не изменяет SCM, драйверы или Program Files.
- Рабочую копию для реализации изолировать через using-git-worktrees; текущий пользовательский build/ не перенастраивать.

## Review Focus

1. Потеря идентичности при PID/TID reuse и поздних ETW-событиях: не присваивать трафик новому процессу (A3, A6).
2. Зависший/упавший GUI, два GUI и сон: сбор заканчивается без утечки сессии и возобновляется без старых скоростей (A2, A4, A6).
3. Подмена pipe, некорректные кадры и медленный клиент: никакого чужого сервера, неограниченной очереди или привилегированной операции (A1, A2).
4. Частичная установка, отмена UAC и намеренно отключённая служба: не показывать успех и не включать самовольно (A5, A7).
5. Закрытие службы во время чтения ETW/pipe и обновление занятых файлов: ограниченное завершение и восстанавливаемое состояние (A4, A5, A8).

## Разделение на поставки

Этот план A независимо даёт рабочие ETW и настройки службы. [План B](2026-10-22-cpu-temperature.md) добавляет необязательные PawnIO и температуры. Полный пользовательский запрос завершён только после обоих планов. Промежуточная поставка A не предлагает установить ещё не поддержанную функцию температур.

## Файлы и ответственность

| Пути (пары .h/.cpp, если указано) | Ответственность |
| --- | --- |
| src/monitoring/MonitoringTypes.h, SetupTypes.h, WireCodec.{h,cpp} | Общие типы метрик/операций setup и ограниченный бинарный протокол без Qt |
| src/monitoring/win/ServiceControl.{h,cpp}, ServiceConfig.{h,cpp} | SCM, защищённая конфигурация и состояния компонентов |
| src/service/ClientLeases.{h,cpp}, PipeServer.{h,cpp} | Клиенты, heartbeat, локальный канал и ACL |
| src/service/ProcessIdentity.{h,cpp}, EtwDecoder.{h,cpp}, IoAccumulator.{h,cpp}, EtwCollector.{h,cpp} | Атрибуция, схемы, расчёты, владение ETW |
| src/service/MonitoringService.{h,cpp}, main.cpp | SCM entry, координация независимых источников и остановка |
| src/setup/SetupActions.{h,cpp}, PackageFiles.{h,cpp}, main.cpp | Фиксированные операции установки, развёртывание и результат |
| src/MonitoringClient.{h,cpp}, SetupLauncher.{h,cpp}, ServiceSnapshotMerge.{h,cpp} | Worker IPC, UAC и безопасное объединение снимков |
| src/SettingsPage.{h,cpp}, resources/icons/settings.svg | Статусы, действия и вкладка настроек |
| resources/gui.manifest, setup.manifest, setup.rc | Раздельные manifests и ресурсы setup |
| test/monitoring/CMakeLists.txt, TestSupport.h, *Tests.cpp | Детерминированные тесты без повышения |
| test/monitoring/ServiceIntegration.ps1, MeasureOverhead.ps1 | Явные системные проверки и замеры |
| CMakeLists.txt, cmake/MonitoringTargets.cmake, cmake/MonitoringPackage.cmake | Цели, CTest и пакет установки |

Существующие ProcessMonitor/ProcessPage/MainWindow подключаются к клиенту; ProcessProviderWin не получает IPC-вызовов. PerformancePage и CPU-топология расширяются в плане B. Не рефакторить несвязанные части приложения.

## Сборка и правила проверки

Текущий build использует MSVC x64 и Qt `C:/Qt/6.11.0/msvc2022_64`. В Developer PowerShell из корня изолированной рабочей копии:

```powershell
cmake -S . -B build-monitoring -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/msvc2022_64 -DBUILD_TESTING=ON
cmake --build build-monitoring
ctest --test-dir build-monitoring --output-on-failure -L monitoring-unit
```

Пути адаптируются к реально установленному toolchain; существующий build не переиспользовать. Каждый `*Tests.cpp` — небольшой executable CTest; TestSupport.h даёт `CHECK` и `CHECK_NEAR`, работающие и при NDEBUG. GUI-тесты используют QApplication и собственные проверки, без новой зависимости Qt Test. Фикстуры ниже создаются в указанном файле теста, а не являются дополнительным product API.

Для каждого шага red/green: добавить тест, собрать конкретную цель, запустить `ctest --test-dir build-monitoring --output-on-failure -R '^NAME$'`. Red должен выявлять целевое отсутствие/неверное поведение; случайная ошибка окружения не считается red. После реализации ожидается PASS. Коммитить только файлы задачи после проверки. Полный suite запускать на завершении этапа, не после каждой правки текста.

## A1. Типы, wire-протокол и исполняемые тесты

**Files:** создать MonitoringTypes.h, WireCodec.{h,cpp}, test/monitoring/TestSupport.h, ProtocolTests.cpp и CMakeLists.txt; изменить корневой CMakeLists.txt, создать cmake/MonitoringTargets.cmake.

**Interfaces:** namespace `monitoring`; `ProcessKey {uint32_t pid; uint64_t creationTime100ns;}` с полноценным сравнением; `LogicalCpuId {uint16_t group; uint16_t number;}`; `SourceState {Unavailable, WarmingUp, Ready, Partial, Failed}`; `SourceStatus {SourceState state; uint32_t win32Error; std::string detail;}`. `IoRate {ProcessKey key; double diskMBps; double networkMBps;}`; `SensorReading {uint32_t sensorId; SensorScope scope; uint32_t packageId; uint32_t coreId; double celsius; bool valid;}`; scope = Core/Group/Package. `Snapshot {uint64_t sequence; uint64_t sampleQpc; uint64_t qpcFrequency; uint64_t intervalQpc; SourceStatus disk; SourceStatus network; SourceStatus temperatures; std::vector<IoRate> processes; std::vector<SensorReading> sensors; Diagnostics diagnostics;}`. Diagnostics содержит lostEvents/lostBuffers/unresolvedIds/unsupportedSchemas/truncated. `Status` содержит protocolVersion, serviceVersion, temperatureEnabled и состояния disk/network/temperatures. `Frame {MessageKind kind; uint64_t requestId; std::vector<std::byte> payload;}`; MessageKind = Hello/HelloReply/Heartbeat/GetStatus/StatusReply/GetSnapshot/SnapshotReply/Error. `DecodeError {None, NeedMore, TooLarge, BadMagic, UnsupportedVersion, Malformed}`; `DecodeResult {DecodeError error; size_t consumed; std::optional<Frame> frame;}`. `encodeFrame(const Frame&) -> std::vector<std::byte>` и `decodeFrame(std::span<const std::byte>) -> DecodeResult`; `encodeStatus(const Status&)`, `encodeSnapshot(const Snapshot&)` возвращают byte vector; `decodeStatus(span)`/`decodeSnapshot(span)` возвращают `ValueDecodeResult<T> {DecodeError error; std::optional<T> value;}` с теми же ограничениями.

- [ ] Добавить CTest harness и тесты ProtocolTests: round-trip, неполный заголовок/тело, несовместимая версия, переполнение длины, неверный enum, NaN/Inf и чрезмерное количество записей. Примеры проверок: `CHECK(decodeFrame(shortHeader).error == DecodeError::NeedMore); CHECK(decodeFrame(oversizeHeader).error == DecodeError::TooLarge); CHECK(decoded.processes[0].key.creationTime100ns == original.processes[0].key.creationTime100ns);`.
- [ ] Собрать/запустить ProtocolTests, зафиксировать целевой red.
- [ ] Реализовать version 1, magic QTM1, little-endian поля фиксированной ширины, max frame 4 MiB, max строка 4 KiB, max процессов 32768, max датчиков 4096. Никаких raw memcpy C++ structs; проверки границ до выделения памяти. Отрицательная скорость допустима только как недоступность; температуры требуют valid, без NaN. При превышении количества строк сборщик ставит truncated/Partial, не меняет wire-лимит.
- [ ] ProtocolTests PASS, сборка существующего GUI PASS; commit `feat: define bounded monitoring protocol`.

## A2. Канал и аренда соединений

**Files:** создать ClientLeases.{h,cpp}, PipeServer.{h,cpp}, test/monitoring/LeaseTests.cpp, PipeTests.cpp; добавить цели в MonitoringTargets.cmake.

**Interfaces:** `ClientLeases::connected(uint64_t clientId, uint64_t nowMs)`, `heartbeat(uint64_t clientId, uint64_t nowMs)`, `disconnected(uint64_t clientId, uint64_t nowMs)`, `shouldCollect(uint64_t nowMs) -> bool`, `expire(uint64_t nowMs)`. `PipeServer::start(std::function<Frame(const Frame&)> handler) -> SourceStatus`, `stop()`, `publish(std::shared_ptr<const Snapshot>)`; `clientCount() -> size_t`. Callback обработчика не меняет SCM. Конструктор принимает ClientLeases и монотонные часы; тест может подменить имя pipe и проверку peer identity.

- [ ] LeaseTests с fake clock: `CHECK(leases.shouldCollect(lastDisconnect + 4999)); CHECK(!leases.shouldCollect(lastDisconnect + 5000));`; истечение heartbeat в 15000 мс, не раньше; второй клиент сохраняет сбор; reconnect отменяет grace-period. PipeTests: фрагментированные кадры, обрыв записи, неизвестная команда, 4 MiB+1, медленный клиент; pending snapshots не больше одного на клиента.
- [ ] Запустить LeaseTests и PipeTests, получить red.
- [ ] Реализовать pipe `\\.\pipe\QTaskManager.Monitoring.v1`, запрет remote clients, ACL для SYSTEM/Administrators/Interactive и проверку токена; права pipe задавать минимально, без разрешения клиентам создавать серверные instances. Максимум 8 клиентов, неограниченную очередь заменить последним снимком; read/write overlapped с отменой. По connect требуется handshake за 5 секунд; heartbeat GUI каждые 3 секунды. После тайм-аута клиента сначала удалить из leases, затем закрыть канал. Первый серверный instance создаётся с защитой от уже существующего pipe.
- [ ] В PipeTests отклонить подменённый identity, проверить отсутствие административных message kinds и завершение stop при ожидающем чтении не позже 2 секунд. Обычные unit-тесты используют отдельный pipe текущего пользователя; системный ACL проверяет A8.
- [ ] LeaseTests/PipeTests PASS; commit `feat: add local monitoring transport and client leases`.

## A3. ETW: идентичность, схемы и скорости

**Files:** создать ProcessIdentity.{h,cpp}, EtwDecoder.{h,cpp}, IoAccumulator.{h,cpp}, EtwCollector.{h,cpp}; test/monitoring/IdentityTests.cpp, IoTests.cpp, EtwDecoderTests.cpp; изменить test/EtwIoProbe/src/EtwIoProbe.cpp и его build.cmd для использования общего сборщика.

**Interfaces:** `DecodedEvent` — variant ProcessStart/End, ThreadStart/End, DiskRead/Write, NetSend/Receive с QPC timestamp и полями payload. `EtwDecoder::decode(const EVENT_RECORD&) -> std::optional<DecodedEvent>`, `status() -> SourceStatus`. `ProcessIdentity::apply(const DecodedEvent&)`, `resolvePid(uint32_t pid, uint64_t qpc) -> std::optional<ProcessKey>`, `resolveTid(uint32_t tid, uint64_t qpc) -> std::optional<ProcessKey>`. `IoKind {DiskRead, DiskWrite, NetSend, NetReceive}`; `IoAccumulator::add(ProcessKey, IoKind, uint64_t bytes)`, `sample(uint64_t nowQpc, uint64_t frequency) -> std::vector<IoRate>`, `reset()`. `EtwCollector::start() -> SourceStatus`, `stop()`, `snapshot(uint64_t nowQpc) -> Snapshot`. Доступ к накопителям из publish-потока синхронизирован, raw EVENT_RECORD не живёт после callback.

- [ ] Добавить IdentityTests: PID/TID reuse, поздний event старой жизни, исчезнувший процесс, гонка стартового снимка; неоднозначность даёт unknown, не нового владельца. IoTests: `CHECK_NEAR(sample.diskMBps, 2.0, 1e-9)` для 3 000 000 байт за 1.5 сек; первый интервал WarmingUp, reset, ноль у известного живого неактивного процесса. EtwDecoderTests: IPv4/IPv6, TCP/UDP, read/write, разные версии и размеры указателей, короткий payload, mismatch TDH, счётчик потерь.
- [ ] Запустить три набора и получить red.
- [ ] Реализовать ETW real-time SYSTEM_LOGGER, QPC timestamps, DISK_IO/NETWORK_TCPIP/PROCESS/THREAD, без ETL и стеков. Сначала подписка, затем согласование начального снимка с буферизованными lifecycle-событиями; kernel creation time приводить к FILETIME той же шкалы, что ProcessEntry. Для поздних событий хранить ограниченную историю завершённых PID/TID 30 секунд; более старые или неоднозначные помечать unresolved. Предел 200000 записей истории; переполнение отражать в Diagnostics, не назначать угадываемый PID.
- [ ] Кэшировать схему по provider/version/opcode/bitness и проверять TDH один раз для каждой схемы. При неизвестной схеме отключить затронутые метрики с причиной; повторяющуюся ошибку не логировать на каждое событие. OpenThread fallback ограничить 32 попытками/секунду с отрицательным кэшем. Состояния дискового и сетевого декодеров хранить раздельно, отказ одного оставляет корректные значения второго.
- [ ] Переделать probe: после генерации ждать фактического накопления ожидаемых байтов до deadline 10 секунд, логировать прогресс и позднюю доставку; фиксированные 700 мс не считать барьером. Для окончательных итогов остановить сессию и дочитать consumer, не отбрасывая хвост из-за stop-флага. Не превращать отсутствие UDP/IPv6 теста в PASS.
- [ ] Unit tests PASS; собрать probe без запуска нагрузок. Commit `feat: collect process disk and network rates with ETW`.

## A4. Служба и конфигурация

**Files:** создать src/service/MonitoringService.{h,cpp}, main.cpp, src/monitoring/win/ServiceConfig.{h,cpp}, ServiceControl.{h,cpp}, test/monitoring/ServiceLifecycleTests.cpp.

**Interfaces:** `ServiceConfig {bool temperatureEnabled; bool intentionallyDisabled; uint32_t schemaVersion;}`; `readServiceConfig() -> ServiceConfig`, `writeServiceConfig(const ServiceConfig&) -> uint32_t` (код Win32). `ServiceState {Missing, Stopped, Starting, Running, Stopping, Disabled, AccessDenied, Failed}`; `ServiceInfo {ServiceState state; uint32_t pid; uint32_t error; std::string version;}`; `queryService() -> ServiceInfo`. `TemperatureSource` — interface `start() -> SourceStatus`, `sample() -> std::vector<SensorReading>`, `stop()`; до плана B используется реализация Unavailable. `MonitoringService::run() -> uint32_t`, `requestStop()`. Источники и часы подменяются в lifecycle-тесте.

- [ ] ServiceLifecycleTests: `CHECK(fakeEtw.startCount == 0)` до клиента, один start при двух клиентах, stop после grace, start после reconnect, ошибка температуры не останавливает ETW; stop во время blocked IO возвращает управление. Отдельно reset после сна и сессия, уже занятая другим владельцем.
- [ ] Запустить тест и получить red.
- [ ] Реализовать SCM dispatcher/control handler, состояния START_PENDING/RUNNING/STOP_PENDING/STOPPED и bounded shutdown. HKLM `Software\QTaskManager\Monitoring`, запись только elevated/SYSTEM. Координатор публикует immutable Snapshot каждые 1000 мс, реагирует на leases, config-change и power/resume; работающий ETW не перезапускается на каждый тик. Привилегированное config-change событие недоступно для сигнализации обычному GUI. Повторные ошибки источников ограничивают retries до одного раза за 5 секунд.
- [ ] ServiceLifecycleTests PASS; standalone service executable собирается без Qt. При невозможности остановить в deadline 10 секунд вернуть диагностируемую ошибку, не сообщать ложный STOPPED при активной работе. Commit `feat: host monitoring collectors in an idle-by-default Windows service`.

## A5. Установщик службы и доставка файлов

**Files:** создать src/monitoring/SetupTypes.h, src/setup/SetupActions.{h,cpp}, PackageFiles.{h,cpp}, main.cpp, resources/setup.manifest, setup.rc, gui.manifest, cmake/MonitoringPackage.cmake; изменить CMakeLists.txt и resources/app.rc; test/monitoring/SetupTests.cpp.

**Interfaces:** в общем SetupTypes.h, namespace monitoring: `SetupAction {InstallService, EnableService, DisableService, RemoveService, UpdateService, EnableTemperatures, DisableTemperatures, InstallPawnIo, RemovePawnIo}`; `SetupRequest {SetupAction action; bool withTemperatures;}`; `SetupResult {SetupOutcome outcome; uint32_t win32Error; bool rebootRequired; std::string detail;}`; outcome Success/Cancelled/Partial/Failed/Unsupported. GUI включает только DTO, не implementation setup. `runSetup(const SetupRequest&) -> SetupResult`. `PackageFiles::verify() -> SetupResult`, `deployService() -> SetupResult`. `SetupPlatform` abstract wrapper для SCM/files/config с fake в тестах, реальные Win32 методы не вызываются unit-тестом.

- [ ] SetupTests: install→auto-start, disable→STOP+DISABLED, enable→AUTO+START, cancellation→no mutation, existing service path outside managed directory→reject, Unicode/spaces in protected path, failed copy/SCM rollback, locked exe→retryable result, repeated install→one service. `CHECK(fake.removedPawnIo == false)` при RemoveService. В плане A `InstallPawnIo` возвращает Unsupported, без попытки установки.
- [ ] Запустить SetupTests и получить red.
- [ ] Реализовать fixed action CLI, requireAdministrator только для setup; GUI asInvoker. Установка в `%ProgramFiles%\QTaskManager\Monitoring`, quoted absolute service ImagePath, проверка ACL и отсутствие перенаправления через reparse points. Повышенный setup принимает только enum/boolean, не произвольные пути/команды. На update остановить нашу службу, проверять принадлежность каталога, заменять атомарно после staging; сохранять прежнее состояние запуска для rollback.
- [ ] Добавить target `monitoring-package`, включающий service/setup/GUI и необходимые официальные MSVC runtime зависимости; сборка `cmake --build build-monitoring --target monitoring-package`. Проверять целостность service payload по встроенному в setup каталогу хэшей, не по редактируемому рядом файлу. Учитывать, что подпись собственного setup зависит от наличия сертификата: без него не заявлять verified publisher. Возвращать результат через отдельный защищённый result pipe; ShellExecuteEx/runas не предполагает наследование handles. GUI создаёт nonce-bound канал (ACL для initiating SID и SYSTEM/admin), запускает helper и сверяет PID отправителя с полученным process handle; helper сверяет владельца сервера с initiating GUI PID. Не писать результат в произвольный путь от GUI. Elevation под другой учётной записью также возвращает результат исходному GUI, не через HKCU elevated пользователя.
- [ ] SetupTests PASS; извлечь manifests из собранных EXE и проверить levels; собрать пакет без реальной установки. Commit `feat: install and manage the monitoring service through an elevated helper`.

## A6. Единственный GUI-клиент и подключение колонок

**Files:** создать src/MonitoringClient.{h,cpp}, ServiceSnapshotMerge.{h,cpp}, SetupLauncher.{h,cpp}; изменить MainWindow.{h,cpp}, ProcessMonitor.{h,cpp}, ProcessPage.{h,cpp}, ProcessEntry.h, ProcessModel.cpp; test/monitoring/ClientTests.cpp, MergeTests.cpp.

**Interfaces:** `MonitoringClient` QObject: slots `start()`, `stop()`; signals `snapshotReady(const monitoring::Snapshot&)`, `statusChanged(const monitoring::Status&)`, `connectionChanged(bool)`; native IPC worker не блокирует Qt GUI. `SetupLauncher::run(monitoring::SetupRequest)` и signal `finished(const monitoring::SetupResult&)`. `mergeServiceSnapshot(QVector<ProcessEntry>&, const monitoring::Snapshot*, uint64_t nowQpc, uint64_t frequency)`. `ProcessMonitor::setServiceSnapshot(const monitoring::Snapshot&)`, `clearServiceSnapshot()` — queued slots, применяются внутри poll перед emit. ProcessPage проксирует эти slots к своему worker; MainWindow владеет единственным MonitoringClient на всё окно.

- [ ] MergeTests: точное совпадение ProcessKey, PID reuse не совпадает, неизвестный новый процесс не ноль, stale после 3000 мс, disconnect очищает обе скорости, malformed/Partial не выдаётся за полный интервал. `CHECK(row.diskRateMBps == -1.0)` для устаревшего snapshot. ClientTests: server PID не совпадает со SCM→reject; service restart resets sequence; reconnect backoff 1/2/5 секунд, ограничение 5 секунд; UAC cancellation не меняет существующие метрики.
- [ ] Запустить ClientTests/MergeTests и получить red.
- [ ] Подключить native client worker, handshake, heartbeat 3 секунды, проверки peer, queued signals и QMetaType. Снимки объединяются по PID+creation time без нового poll всех процессов. Добавить `enum class IoQuality {Unavailable, Complete, Partial}` и отдельные diskQuality/networkQuality в ProcessEntry для tooltip «Неполные данные»; SourceState и negative sentinel явно преобразовывать отдельно для диска/сети. ProcessModel показывает «—» при недоступности; numeric sort сохраняется. Повторное использование одного snapshot не считается новым измерением.
- [ ] ClientTests/MergeTests PASS; обычная сборка GUI PASS. Commit `feat: display service-provided process IO without elevating the GUI`.

## A7. Вкладка настроек и предложение установки

**Files:** создать src/SettingsPage.{h,cpp}, resources/icons/settings.svg; изменить MainWindow.{h,cpp}, resources/resources.qrc, CMakeLists.txt; test/monitoring/SettingsTests.cpp.

**Interfaces:** `SettingsState {ServiceInfo service; monitoring::Status live; bool connected; bool operationPending; bool rebootRequired; bool startupOfferDismissed;}`; `SettingsPage::setState(const SettingsState&)`, signal `actionRequested(monitoring::SetupRequest)`; `SettingsActions allowedActions(const SettingsState&)` — чистая функция, объект с bool install/enable/disable/remove/update. Состояние PawnIO добавляет план B.

- [ ] SettingsTests: Missing→install, Disabled→enable без popup при intentionallyDisabled, AccessDenied не Missing, busy блокирует повторные команды; отмена UAC возвращает controls; incompatible protocol предлагает update. Проверить через QApplication: `CHECK(settingsButtonBottom <= sidebarBottom); CHECK(sidebarBottom - settingsButtonBottom < 20);` для двух высот, вкладка скрывает process action header.
- [ ] Запустить SettingsTests и получить red.
- [ ] Добавить SettingsPage четвёртой страницей, кнопку после addStretch. Предложение установки только когда отсутствие подтверждено; закрытие предложения запоминается до явного действия пользователя. Автоматическая проверка SCM/handshake работает асинхронно. При enable/disable/remove вызывать SetupLauncher, затем повторно читать фактическое состояние; tooltip и диагностические статусы не сообщают ложный успех. В плане A блок температур сообщает «Компонент температур ещё не включён в эту сборку», без неработающей кнопки установки.
- [ ] SettingsTests PASS; визуально проверить resize/DPI/выделение кнопки в доступном UI или скриншотах, не писать тесты геометрии для SVG. Commit `feat: add bottom-anchored monitoring settings`.

## A8. Интеграция, нагрузка и документация

**Files:** создать test/monitoring/ServiceIntegration.ps1, MeasureOverhead.ps1, README.md; изменить README.md проекта, test/EtwIoProbe/README.md; результаты сохранять в test/monitoring/results/ с датой, не перезаписывать старый report.txt.

**Interfaces:** scripts `ServiceIntegration.ps1 -Scenario <Install|Lifecycle|Etw|Uninstall>` и `MeasureOverhead.ps1 -Seconds 60 -Scenario <Idle|Build|Copy|Network>`; запуск системных сценариев только по явному согласию, без auto-elevation из CTest. Скрипты проверяют абсолютные пути временных файлов, ограничивают нагрузку и выполняют cleanup только принадлежащих тесту объектов.

- [ ] Добавить сценарии с проверяемыми exit codes: GUI TokenElevation=0; service AUTO+LocalSystem; reboot/login→idle без ETW; два GUI→одна сессия; kill последнего GUI→stop сбора в lease/grace срок; stop service во время blocked IO; повторная установка и rollback занятого файла; ACL/pipe squatting и интерактивный пользователь другой учётной записи. Ожидаемый результат каждого — PASS/FAIL/SKIPPED с причиной, не общий PASS при пропуске.
- [ ] Добавить ETW сценарий с известным unbuffered disk volume, локальными TCP/UDP IPv4/IPv6 sender/receiver в разных PID и bounded wait; сравнивать генератор и агрегаты с явно записанным допуском, фиксировать loss. Loopback не считать достаточной проверкой всего сетевого пути; отдельная разрешённая LAN/Internet проверка. Сверить смысл дискового счётчика с кэшированными чтениями: это не логический file I/O.
- [ ] Выполнить unit suite, сборку Release и package. После согласия выполнить системные сценарии, включая ручную перезагрузку, либо отметить конкретные непроверенные сценарии. Не объявлять интеграцию проверенной по одним unit-тестам.
- [ ] Для каждой нагрузки собрать 3 парных интервала по 60 секунд service collection off/on: CPU-time службы и GUI, system CPU, private bytes/working set, ETW buffers/loss и throughput. Зафиксировать сырые данные, медиану и разброс; не выдумывать лимит «<1%» задним числом. Устранить рост памяти, незавершённые сессии и потери на обычной нагрузке до завершения этапа.
- [ ] Обновить README: реальные колонки, запуск без admin, настройки службы, намеренное disable, известные ограничения ETW. Проверить `git diff --check`, сохранить результаты, commit `test: validate monitoring lifecycle and ETW overhead`.

## Self-review и передача

Покрытие: privilege boundary A1/A2/A5/A6; жизненный цикл A2/A4/A8; ETW A3/A6/A8; установка/ошибки A5/A7; настройки A7; optional driver/температуры/удаление по происхождению — план B. Протокол и сервисные типы A1/A4/A5 переиспользуются без альтернативных дубликатов.

План написан и проверен по спецификации, задачи ещё не выполнялись. До реализации пользователь проверяет оба плана и выбирает способ выполнения: последовательно в текущей сессии либо с отдельными исполнителями и ревьюерами задач.
