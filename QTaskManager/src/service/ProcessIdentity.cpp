#include "ProcessIdentity.h"
namespace monitoring
{
std::optional<ProcessKey> ProcessIdentity::resolve(const std::multimap<uint32_t, Life> &m, uint32_t id,
                                                   uint64_t at)
{
    std::optional<ProcessKey> found;
    auto [b, e] = m.equal_range(id);
    for (; b != e; ++b)
        if (b->second.begin <= at && at < b->second.end)
        {
            if (found && *found != b->second.key)
                return {};
            found = b->second.key;
        }
    return found;
}
void ProcessIdentity::start(std::multimap<uint32_t, Life> &m, uint32_t id, ProcessKey key, uint64_t at)
{
    auto [b, e] = m.equal_range(id);
    uint64_t end = UINT64_MAX;
    for (auto it = b; it != e; ++it)
    {
        auto &l = it->second;
        if (l.key == key && l.begin == at)
            return;
        if (l.begin > at)
            end = (std::min)(end, l.begin);
        else if (l.end > at)
            l.end = at;
    }
    m.emplace(id, Life{at, end, key});
}
void ProcessIdentity::end(std::multimap<uint32_t, Life> &m, uint32_t id, uint64_t at)
{
    auto [b, e] = m.equal_range(id);
    for (; b != e; ++b)
        if (b->second.begin <= at && at < b->second.end)
            b->second.end = at;
}
void ProcessIdentity::processStart(ProcessKey k, uint64_t at)
{
    start(pids_, k.pid, k, at);
}
void ProcessIdentity::processEnd(uint32_t id, uint64_t at)
{
    end(pids_, id, at);
}
void ProcessIdentity::threadStart(uint32_t id, ProcessKey k, uint64_t at)
{
    start(tids_, id, k, at);
}
void ProcessIdentity::threadEnd(uint32_t id, uint64_t at)
{
    end(tids_, id, at);
}
std::optional<ProcessKey> ProcessIdentity::resolvePid(uint32_t id, uint64_t at) const
{
    return resolve(pids_, id, at);
}
std::optional<ProcessKey> ProcessIdentity::resolveTid(uint32_t id, uint64_t at) const
{
    return resolve(tids_, id, at);
}
std::vector<ProcessKey> ProcessIdentity::live() const
{
    std::vector<ProcessKey> result;
    for (auto &[id, l] : pids_)
        if (l.end == UINT64_MAX)
            result.push_back(l.key);
    return result;
}
bool ProcessIdentity::prune(uint64_t before)
{
    bool truncated = false;
    for (auto *m : {&pids_, &tids_})
    {
        for (auto it = m->begin(); it != m->end();)
            if (it->second.end < before)
                it = m->erase(it);
            else
                ++it;
        while (m->size() > 100000)
        {
            auto oldest = m->begin();
            for (auto it = m->begin(); it != m->end(); ++it)
                if (it->second.begin < oldest->second.begin)
                    oldest = it;
            m->erase(oldest);
            truncated = true;
        }
    }
    return truncated;
}
} // namespace monitoring
