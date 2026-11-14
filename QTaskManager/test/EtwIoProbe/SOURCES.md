# Relevant API/source references

Microsoft Learn:

- DiskIo provider and enabling `EVENT_TRACE_FLAG_DISK_IO`:
  https://learn.microsoft.com/windows/win32/etw/diskio
- DiskIo_TypeGroup1 (`TransferSize`, `IssuingThreadId`):
  https://learn.microsoft.com/windows/win32/etw/diskio-typegroup1
- TcpIp provider event types:
  https://learn.microsoft.com/windows/win32/etw/tcpip
- TcpIp Send IPv4 (`PID`, `size` are the first two properties):
  https://learn.microsoft.com/windows/win32/etw/tcpip-sendipv4
- UDP IPv4 send/receive (`PID`, `size`):
  https://learn.microsoft.com/windows/win32/etw/udpip-typegroup1
- Thread provider / start/end/DCStart/DCEnd:
  https://learn.microsoft.com/windows/win32/etw/thread
- Thread V2 payload (`ProcessId`, `TThreadId`):
  https://learn.microsoft.com/windows/win32/etw/thread-v2-typegroup1
- NT Kernel Logger provider GUID constants:
  https://learn.microsoft.com/windows/win32/etw/nt-kernel-logger-constants
- EVENT_TRACE_PROPERTIES loss counters:
  https://learn.microsoft.com/windows/win32/api/evntrace/ns-evntrace-event_trace_properties

Reference implementation:

- System Informer ExtendedTools ETW monitor:
  https://github.com/winsiderss/systeminformer/blob/master/plugins/ExtendedTools/etwmon.c
