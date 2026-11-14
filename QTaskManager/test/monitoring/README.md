# Monitoring validation

`ctest --test-dir build-monitoring --output-on-failure` runs without elevation and does not install services/drivers. It covers bounded decoding, leases, real local pipe cancellation, identity reuse, rate arithmetic, setup routing, install-directory ACL policy, snapshot freshness and settings geometry. `GuiTests` renders `build-monitoring/settings-preview.png` offscreen with Segoe UI.

The Release `monitoring-package` target produces a runnable Qt GUI and setup. The setup embeds the native service and uses a static CRT; installing the service needs no Qt DLLs. Both setup and service are currently unsigned development binaries. A release signing certificate is not provided by this repository.

System tests are **not yet executed**. They require explicit user consent and UAC. `ServiceIntegration.ps1` defaults to read-only `Status`; only `Install`/`Uninstall -AllowSystemChanges` change SCM. It never installs or removes PawnIO. Output distinguishes PASS, FAIL and SKIPPED. `MonitorProbe.exe [seconds]` is an ordinary IPC client, outputs JSON lines and can drive connection/lifecycle checks without the GUI. It does not start an ETW session itself.

Remaining system validation: real FileIo attribution and requested-byte totals (cached and uncached workloads), separate TCP/UDP IPv4/IPv6 processes and non-loopback traffic, two clients, crash/heartbeat/grace, reboot/resume, a second user, installer rollback under locked files, cancellation and pipe ACL/squatting. A unit-test PASS is not evidence that those scenarios passed.

`MeasureOverhead.ps1` implements only three paired service-only idle windows. Loaded workloads, GUI/system CPU and throughput are deliberately not claimed. Full overhead characterization remains pending; no percentage has been measured or promised.

The historical `test/EtwIoProbe` is a standalone prototype, not the shipped collector; its old report and fixed waits do not validate this implementation. Use the service plus MonitorProbe for current diagnostics.

### Attribution diagnostics (introduced in service 1.1.0 / protocol 2)

GUI and service must both be updated. The named pipe discovery endpoint stays the same;
an older protocol is rejected and the GUI's Settings → Update action remains available through SCM.
For a separate runnable release directory, configure `-DMONITORING_PACKAGE_DIR=<absolute path>`
before building target `monitoring-package`; this also avoids overwriting a running GUI executable.

Settings and `MonitorProbe` expose fixed-size `attributionTotal` and `attributionInterval` counters.
Totals reset at collection restart (including a reconnect after the idle grace period),
not on every GUI poll. An interval is one service sample; a GUI poll may skip an interval.
`diskEvents`/`networkEvents` count all decoded I/O operations, both matched and unmatched.
The issue buckets distinguish missing IDs, IDs absent from the event-time identity table,
OpenProcess/GetProcessTimes failures, a process creation time newer than its event, and threads
whose owner is unknown. Each bucket carries event count, discarded bytes, and the last PID/TID/Win32
error. Process/thread counters also include the initial Toolhelp seed; they are not byte-loss counters.

Only unmatched disk I/O makes disk Partial; only unmatched network I/O makes network Partial.
Lifecycle issues alone do not prove missing I/O. A clean interval restores Ready without erasing
session totals. Real ETW event/buffer loss still invalidates the identity epoch and restarts collection.
No per-event logs, unbounded diagnostic maps, or extra per-I/O process queries are introduced.

`AttributionTests` covers source-independent quality, lifecycle reasons, unmatched byte counts,
valid traffic preservation, interval recovery/reset, and failure/warm-up status preservation.
ProtocolTests checks that all detailed counters survive IPC encoding. SettingsTests checks the
rendered diagnostics; GuiTests checks that long diagnostics remain scrollable in a smaller window.

### Primary file activity column (service 1.2.0 / protocol 3)

The user selected logical file requests for the primary disk-related column. Its GUI title is now
`Файлы, MB/s`: the sum of requested FileIo read/write bytes, including cached activity. It does not
measure physical storage throughput or confirm successful completion. Internal `diskMBps`,
`diskEvents` and disk-related enum names retain their existing primary-column positions; since protocol
3 they describe file requests. MonitorProbe identifies this explicitly as `diskMetric: fileRequests`.

The single session enables `EVENT_TRACE_FLAG_FILE_IO_INIT`, not `DISK_IO`. Only FileIo opcodes 67/68
are decoded; create/info/directory records are rejected before taking the collector mutex. FileIo v2/v3
32/64-bit IoSize and TTID layouts are checked against TDH once per schema. Request TTID and timestamp
drive attribution even when event header IDs refer to System. Physical completion bytes are never
added to this metric. Names and paths in ignored FileIo create records are not decoded or retained;
stack tracing, OpEnd and ETL recording remain disabled.

Windows may perform real cache flushes in System threads. Memory-mapped modifications do not imply
an ordinary read/write request from the modifying process. The metric therefore cannot reconstruct
the original application behind every background disk operation.

GUI/service both need updating via Settings → Update (UAC). The protocol change rejects semantic
mixing with an older GUI or service. The release package is `D:\Projects\QTaskManager\build\monitoring-1.2.0`.
EtwDecoderTests covers independent v2/v3 x86/x64 fixtures, request TTID vs System header IDs,
requested-byte arithmetic, short/unsupported schemas and physical-event rejection. Full CTest is
15/15; real cached-I/O and TDH metadata validation still requires the updated installed service.

Primary schema references: [FileIo_ReadWrite](https://learn.microsoft.com/en-us/windows/win32/etw/fileio-readwrite),
[session flags](https://learn.microsoft.com/en-us/windows/win32/etw/event-trace-properties), and
[Microsoft TraceEvent parser](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/KernelTraceEventParser.cs)
(`FileIOReadWriteTraceData`).
