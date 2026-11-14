#pragma once
#include "ProcessEntry.h"
#include "monitoring/MonitoringTypes.h"
void mergeServiceSnapshot(QVector<ProcessEntry> &, const monitoring::Snapshot *, uint64_t nowQpc,
                          uint64_t frequency);
