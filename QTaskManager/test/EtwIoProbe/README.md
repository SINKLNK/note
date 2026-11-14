# EtwIoProbe

Small **non-Qt Windows console probe** for validating the exact ETW approach we want to use in QTaskManager for per-process:

- physical disk I/O bytes / second;
- TCP + UDP network bytes / second;
- disk `IssuingThreadId -> PID` attribution;
- ETW event-loss detection.

It deliberately does **not** use `GetProcessIoCounters()` as a disk counter, because that is aggregate process I/O and is not a truthful physical-disk metric.

## 1. Build

Open an **x64 Developer Command Prompt for Visual Studio**, then:

```bat
cd /d D:\wherever\EtwIoProbe
build.cmd
```

Expected output:

```text
BUILD OK: ...\build\EtwIoProbe.exe
```

No Qt, CMake, NuGet or external dependencies are used. It links only Windows SDK libraries.

## 2. Run the automatic verification

From any cmd (Developer Prompt is fine):

```bat
verify.cmd
```

The script asks for UAC elevation and runs a self-test. It will:

1. Start a private real-time kernel ETW session with:
   - `EVENT_TRACE_FLAG_DISK_IO`
   - `EVENT_TRACE_FLAG_NETWORK_TCPIP`
   - `EVENT_TRACE_FLAG_THREAD`
2. Seed a `TID -> PID` map from the current thread snapshot and then maintain it using ETW thread start/end events.
3. Write **512 MiB** to a temporary file using `FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`.
4. Compare the known number of bytes written with DiskIo ETW bytes attributed to `EtwIoProbe.exe` itself.
5. Download **64 MiB** from Cloudflare's speed-test endpoint and compare the downloaded payload with TCP receive ETW bytes attributed to `EtwIoProbe.exe`.
6. Generate a small UDP loopback burst to verify that UDP events/parser are visible (this is advisory because loopback handling can vary).
7. Report:
   - Disk/TCP/UDP/thread event counts;
   - TID map hits / fallback hits / unresolved TIDs;
   - ETW lost events / lost buffers;
   - top disk/network processes seen during the run.

A report is saved as:

```text
report.txt
```

**Send me that file or paste its contents.**

### Important

The disk test creates a temporary file in `%TEMP%` and opens it with `FILE_FLAG_DELETE_ON_CLOSE`, so it is deleted automatically.

The TCP test needs Internet access to:

```text
https://speed.cloudflare.com/__down?bytes=...
```

If that endpoint is blocked/unavailable, the result will be `PARTIAL`, not a fake pass.

## 3. Optional live test

To watch real process traffic for 30 seconds:

```bat
monitor.cmd
```

Or from an elevated terminal:

```bat
build\EtwIoProbe.exe --monitor 60
```

Generate some traffic while it runs (download something, build/copy files, etc.). It prints the top disk and network PIDs once per second.

## Useful manual variants

Smaller/faster test:

```bat
build\EtwIoProbe.exe --selftest --disk-mib 128 --network-mib 16
```

Disk-only test:

```bat
build\EtwIoProbe.exe --selftest --disk-mib 512 --no-network --no-udp
```

## What constitutes success

For the automated self-test we want approximately:

```text
Disk attribution: PASS
TCP attribution: PASS
Trace integrity: PASS
Lost: events=0 logBuffers=0 realtimeBuffers=0
VERDICT: PASS
```

The exact ETW/file or ETW/TCP ratio does not have to be exactly 1.000, but it should be close. The probe uses deliberately broad sanity thresholds because filesystem metadata, transport framing/retransmits, and timing can produce small differences.

## Implementation notes

The implementation mirrors the architecture used by System Informer's ExtendedTools ETW monitor:

- a private `EVENT_TRACE_SYSTEM_LOGGER_MODE` session on modern Windows;
- DiskIo Read/Write events;
- TCP/UDP Send/Receive events;
- disk attribution via `IssuingThreadId`;
- a real-time `OpenTrace` / `ProcessTrace` consumer thread.

The test probe also keeps a fallback `OpenThread -> GetProcessIdOfThread` only when a TID is unexpectedly missing from the map. The report tells us how often that fallback was necessary; production QTaskManager can then decide whether the ETW-maintained map is sufficient by itself.
