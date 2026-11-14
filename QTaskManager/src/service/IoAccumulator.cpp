#include "IoAccumulator.h"
namespace monitoring
{
void IoAccumulator::add(ProcessKey k, IoKind kind, uint64_t bytes)
{
    auto &c = counts_[k];
    if (kind == IoKind::DiskRead || kind == IoKind::DiskWrite)
        c.disk += bytes;
    else
        c.network += bytes;
}
std::vector<IoRate> IoAccumulator::sample(uint64_t now, uint64_t frequency)
{
    std::vector<IoRate> result;
    if (last_ && now > last_ && frequency)
    {
        double seconds = double(now - last_) / double(frequency);
        for (auto &[key, c] : counts_)
            result.push_back(
                {key, double(c.disk) / 1000000.0 / seconds, double(c.network) / 1000000.0 / seconds});
    }
    counts_.clear();
    last_ = now;
    return result;
}
} // namespace monitoring
