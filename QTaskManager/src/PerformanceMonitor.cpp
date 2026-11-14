#include "PerformanceMonitor.h"

#include <QTimer>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <winternl.h>
#include <Psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <d3dkmthk.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>
#endif

#ifdef Q_OS_WIN
namespace
{
PDH_HCOUNTER addCounter(PDH_HQUERY query, const wchar_t *path)
{
    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, path, 0, &counter) != ERROR_SUCCESS) {
        return nullptr;
    }
    return counter;
}

bool validStatus(DWORD status)
{
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

double readDouble(PDH_HCOUNTER counter)
{
    PDH_FMT_COUNTERVALUE value{};
    if (!counter || PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS
        || !validStatus(value.CStatus)) {
        return 0.0;
    }
    return value.doubleValue;
}

quint64 readBytes(PDH_HCOUNTER counter)
{
    PDH_FMT_COUNTERVALUE value{};
    if (!counter || PdhGetFormattedCounterValue(counter, PDH_FMT_LARGE, nullptr, &value) != ERROR_SUCCESS
        || !validStatus(value.CStatus) || value.largeValue < 0) {
        return 0;
    }
    return static_cast<quint64>(value.largeValue);
}

// Every instance of a "(*)" counter with its current value. Instances that
// appeared since the last collect have no valid value yet and are skipped.
QVector<QPair<QString, double>> readInstances(PDH_HCOUNTER counter)
{
    QVector<QPair<QString, double>> result;
    if (!counter) {
        return result;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr) != PDH_MORE_DATA
        || bufferSize == 0) {
        return result;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto *items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) != ERROR_SUCCESS) {
        return result;
    }

    result.reserve(itemCount);
    for (DWORD i = 0; i < itemCount; ++i) {
        if (validStatus(items[i].FmtValue.CStatus)) {
            result.append({QString::fromWCharArray(items[i].szName), items[i].FmtValue.doubleValue});
        }
    }
    return result;
}

// "\Processor Information" names its instances "<group>,<index>", plus
// "<group>,_Total" and "_Total". Order the real ones the way the system
// numbers processors, group by group.
QVector<double> perProcessor(PDH_HCOUNTER counter)
{
    std::vector<std::tuple<int, int, double>> processors;
    for (const auto &[name, value] : readInstances(counter)) {
        const int comma = name.indexOf(QLatin1Char(','));
        if (comma < 0) {
            continue;
        }
        bool groupOk = false;
        bool indexOk = false;
        const int group = name.left(comma).toInt(&groupOk);
        const int index = name.mid(comma + 1).toInt(&indexOk);
        if (groupOk && indexOk) {
            processors.emplace_back(group, index, qBound(0.0, value, 100.0));
        }
    }
    std::sort(processors.begin(), processors.end());

    QVector<double> result;
    result.reserve(static_cast<int>(processors.size()));
    for (const auto &processor : processors) {
        result.append(std::get<2>(processor));
    }
    return result;
}

int numberAfter(const QString &text, QLatin1String marker)
{
    const int at = text.indexOf(marker, 0, Qt::CaseInsensitive);
    if (at < 0) {
        return -1;
    }
    int end = at + marker.size();
    int value = 0;
    while (end < text.size() && text.at(end).isDigit()) {
        value = value * 10 + text.at(end).digitValue();
        ++end;
    }
    return value;
}

QString readCpuName()
{
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString",
                     RRF_RT_REG_SZ,
                     nullptr,
                     buffer,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    // Some firmware pads the brand string with spaces on either side.
    return QString::fromWCharArray(buffer).simplified();
}

QString memoryTypeName(BYTE type)
{
    switch (type) {
    case 0x0F: return QStringLiteral("SDRAM");
    case 0x12: return QStringLiteral("DDR");
    case 0x13: return QStringLiteral("DDR2");
    case 0x14: return QStringLiteral("DDR2 FB-DIMM");
    case 0x18: return QStringLiteral("DDR3");
    case 0x1A: return QStringLiteral("DDR4");
    case 0x1B: return QStringLiteral("LPDDR");
    case 0x1C: return QStringLiteral("LPDDR2");
    case 0x1D: return QStringLiteral("LPDDR3");
    case 0x1E: return QStringLiteral("LPDDR4");
    case 0x22: return QStringLiteral("DDR5");
    case 0x23: return QStringLiteral("LPDDR5");
    default: return {};
    }
}

// The memory type of the first populated slot, from the SMBIOS "Memory
// Device" structures (type 17). Windows has no API that reports it directly.
QString readMemoryType()
{
    const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size < 8) {
        return {};
    }
    std::vector<BYTE> buffer(size);
    if (GetSystemFirmwareTable('RSMB', 0, buffer.data(), size) != size) {
        return {};
    }

    // RawSMBIOSData: four version bytes and a DWORD length ahead of the table.
    DWORD tableLength = 0;
    memcpy(&tableLength, buffer.data() + 4, sizeof(tableLength));
    const BYTE *structure = buffer.data() + 8;
    const BYTE *end = structure + std::min<size_t>(tableLength, size - 8);

    while (structure + 4 <= end) {
        const BYTE type = structure[0];
        const BYTE length = structure[1];
        if (length < 4 || structure + length > end || type == 127) {
            break;
        }

        if (type == 17 && length > 0x12) {
            // Size 0 is an empty slot, 0xFFFF an unknown size.
            const WORD moduleSize = static_cast<WORD>(structure[0x0C] | (structure[0x0D] << 8));
            if (moduleSize != 0 && moduleSize != 0xFFFF) {
                const QString name = memoryTypeName(structure[0x12]);
                if (!name.isEmpty()) {
                    return name;
                }
            }
        }

        // The formatted part is followed by its strings, ending in two nulls.
        const BYTE *next = structure + length;
        while (next + 1 < end && (next[0] != 0 || next[1] != 0)) {
            ++next;
        }
        structure = next + 2;
    }
    return {};
}

double readGpuTemperature(D3DKMT_HANDLE adapter)
{
    if (!adapter) {
        return -1.0;
    }

    D3DKMT_ADAPTER_PERFDATA perf{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = adapter;
    query.Type = KMTQAITYPE_ADAPTERPERFDATA;
    query.pPrivateDriverData = &perf;
    query.PrivateDriverDataSize = sizeof(perf);
    // Drivers that do not track temperature succeed and leave it at zero.
    if (!NT_SUCCESS(D3DKMTQueryAdapterInfo(&query)) || perf.Temperature == 0) {
        return -1.0;
    }
    return perf.Temperature / 10.0; // deci-Celsius
}
} // namespace
#endif

struct PerformanceMonitor::Sources
{
#ifdef Q_OS_WIN
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER coreUtility = nullptr;
    PDH_HCOUNTER coreKernel = nullptr;
    PDH_HCOUNTER totalUtility = nullptr;
    PDH_HCOUNTER performance = nullptr;
    PDH_HCOUNTER frequency = nullptr;
    PDH_HCOUNTER modified = nullptr;
    PDH_HCOUNTER standbyReserve = nullptr;
    PDH_HCOUNTER standbyNormal = nullptr;
    PDH_HCOUNTER standbyCore = nullptr;
    PDH_HCOUNTER freeAndZero = nullptr;
    PDH_HCOUNTER gpuEngine = nullptr;
    PDH_HCOUNTER gpuDedicated = nullptr;
    PDH_HCOUNTER gpuShared = nullptr;

    // How PDH instance names spell the chosen adapter, "luid_0x..._0x...".
    // Empty when there is no hardware adapter; then GPU counters are skipped.
    QString gpuLuid;
    D3DKMT_HANDLE gpuAdapter = 0;

    ~Sources()
    {
        if (query) {
            PdhCloseQuery(query);
        }
        if (gpuAdapter) {
            D3DKMT_CLOSEADAPTER close{};
            close.hAdapter = gpuAdapter;
            D3DKMTCloseAdapter(&close);
        }
    }
#endif
};

PerformanceMonitor::PerformanceMonitor(QObject *parent)
    : QObject(parent)
    , m_sources(std::make_unique<Sources>())
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &PerformanceMonitor::poll);
}

PerformanceMonitor::~PerformanceMonitor() = default;

void PerformanceMonitor::start()
{
    SystemInfo info;

#ifdef Q_OS_WIN
    Sources &sources = *m_sources;

    info.cpuName = readCpuName();
    info.logicalProcessors = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    ULONGLONG installedKb = 0;
    if (GetPhysicallyInstalledSystemMemory(&installedKb)) {
        info.installedMemory = installedKb * 1024;
    }
    info.memoryType = readMemoryType();

    // The adapter with the most dedicated memory is the one worth watching on
    // a laptop with integrated and discrete graphics.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 best{};
        bool found = false;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                continue;
            }
            if (!found || desc.DedicatedVideoMemory > best.DedicatedVideoMemory) {
                best = desc;
                found = true;
            }
        }

        if (found) {
            info.gpuName = QString::fromWCharArray(best.Description).simplified();
            info.gpuDedicatedTotal = best.DedicatedVideoMemory;
            info.gpuSharedTotal = best.SharedSystemMemory;
            sources.gpuLuid = QStringLiteral("luid_0x%1_0x%2")
                                  .arg(static_cast<quint32>(best.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
                                  .arg(static_cast<quint32>(best.AdapterLuid.LowPart), 8, 16, QLatin1Char('0'));

            D3DKMT_OPENADAPTERFROMLUID open{};
            open.AdapterLuid = best.AdapterLuid;
            if (NT_SUCCESS(D3DKMTOpenAdapterFromLuid(&open))) {
                sources.gpuAdapter = open.hAdapter;
                info.gpuTemperatureAvailable = readGpuTemperature(sources.gpuAdapter) >= 0.0;
            }
        }
    }

    if (PdhOpenQueryW(nullptr, 0, &sources.query) == ERROR_SUCCESS) {
        sources.coreUtility = addCounter(sources.query, L"\\Processor Information(*)\\% Processor Utility");
        sources.coreKernel = addCounter(sources.query, L"\\Processor Information(*)\\% Privileged Utility");
        sources.totalUtility = addCounter(sources.query, L"\\Processor Information(_Total)\\% Processor Utility");
        sources.performance = addCounter(sources.query, L"\\Processor Information(_Total)\\% Processor Performance");
        sources.frequency = addCounter(sources.query, L"\\Processor Information(_Total)\\Processor Frequency");
        sources.modified = addCounter(sources.query, L"\\Memory\\Modified Page List Bytes");
        sources.standbyReserve = addCounter(sources.query, L"\\Memory\\Standby Cache Reserve Bytes");
        sources.standbyNormal = addCounter(sources.query, L"\\Memory\\Standby Cache Normal Priority Bytes");
        sources.standbyCore = addCounter(sources.query, L"\\Memory\\Standby Cache Core Bytes");
        sources.freeAndZero = addCounter(sources.query, L"\\Memory\\Free & Zero Page List Bytes");
        if (!sources.gpuLuid.isEmpty()) {
            sources.gpuEngine = addCounter(sources.query, L"\\GPU Engine(*)\\Utilization Percentage");
            sources.gpuDedicated = addCounter(sources.query, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
            sources.gpuShared = addCounter(sources.query, L"\\GPU Adapter Memory(*)\\Shared Usage");
        }
        // Rate counters need two collects before they have a value; this is
        // the first, the timer's first tick is the second.
        PdhCollectQueryData(sources.query);
    } else {
        sources.query = nullptr;
    }
#endif

    emit systemInfoReady(info);
    m_timer->start();
}

void PerformanceMonitor::poll()
{
    PerformanceSample sample;

#ifdef Q_OS_WIN
    Sources &sources = *m_sources;

    if (sources.query && PdhCollectQueryData(sources.query) == ERROR_SUCCESS) {
        sample.coreUsage = perProcessor(sources.coreUtility);
        sample.coreKernel = perProcessor(sources.coreKernel);
        // Utility goes past 100% when the clock is boosted; Task Manager caps it.
        sample.cpuUsage = qBound(0.0, readDouble(sources.totalUtility), 100.0);
        sample.cpuSpeedMHz = readDouble(sources.frequency) * readDouble(sources.performance) / 100.0;

        sample.modified = readBytes(sources.modified);
        sample.standby = readBytes(sources.standbyReserve) + readBytes(sources.standbyNormal)
                         + readBytes(sources.standbyCore);
        sample.free = readBytes(sources.freeAndZero);

        if (!sources.gpuLuid.isEmpty()) {
            // One instance per process per engine. An engine's load is the sum
            // over processes; a type's load is its busiest engine.
            std::map<int, double> engineLoad;
            std::map<int, QString> engineType;
            for (const auto &[name, value] : readInstances(sources.gpuEngine)) {
                if (!name.contains(sources.gpuLuid, Qt::CaseInsensitive)) {
                    continue;
                }
                const int typeAt = name.indexOf(QLatin1String("_engtype_"), 0, Qt::CaseInsensitive);
                if (typeAt < 0) {
                    continue;
                }
                const int key = numberAfter(name, QLatin1String("_phys_")) * 1024
                                + numberAfter(name, QLatin1String("_eng_"));
                engineLoad[key] += value;
                engineType[key] = name.mid(typeAt + 9);
            }
            for (const auto &[key, load] : engineLoad) {
                const double clamped = qBound(0.0, load, 100.0);
                const QString &type = engineType[key];
                sample.gpuUsage = qMax(sample.gpuUsage, clamped);
                if (type.compare(QLatin1String("3D"), Qt::CaseInsensitive) == 0) {
                    sample.gpu3d = qMax(sample.gpu3d, clamped);
                } else if (type.compare(QLatin1String("VideoDecode"), Qt::CaseInsensitive) == 0) {
                    sample.gpuVideoDecode = qMax(sample.gpuVideoDecode, clamped);
                }
            }

            for (const auto &[name, value] : readInstances(sources.gpuDedicated)) {
                if (name.contains(sources.gpuLuid, Qt::CaseInsensitive)) {
                    sample.gpuDedicatedUsed += static_cast<quint64>(qMax(0.0, value));
                }
            }
            for (const auto &[name, value] : readInstances(sources.gpuShared)) {
                if (name.contains(sources.gpuLuid, Qt::CaseInsensitive)) {
                    sample.gpuSharedUsed += static_cast<quint64>(qMax(0.0, value));
                }
            }
        }
    }

    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    if (GetPerformanceInfo(&perf, sizeof(perf))) {
        const quint64 page = perf.PageSize;
        sample.physicalTotal = perf.PhysicalTotal * page;
        sample.physicalAvailable = perf.PhysicalAvailable * page;
        sample.commitTotal = perf.CommitTotal * page;
        sample.commitLimit = perf.CommitLimit * page;
        sample.pagedPool = perf.KernelPaged * page;
        sample.nonPagedPool = perf.KernelNonpaged * page;
        sample.processes = perf.ProcessCount;
        sample.threads = perf.ThreadCount;
        sample.handles = perf.HandleCount;
    }
    sample.uptimeMs = GetTickCount64();

    sample.gpuTemperature = readGpuTemperature(sources.gpuAdapter);
#endif

    emit sampleReady(sample);
}
