#include "algorithm.hpp"

#include <algorithm>

namespace pastiche {

Registry& Registry::instance()
{
    static Registry registry;
    return registry;
}

void Registry::add(AlgorithmFactory factory)
{
    if (!factory) return;
    std::unique_ptr<IStyleAlgorithm> probe = factory();
    if (!probe) return;
    const std::string id = probe->id();
    for (Entry& e : entries_) {
        if (e.id == id) { e.factory = factory; return; }  // last registration wins
    }
    entries_.push_back({id, factory});
}

std::vector<std::string> Registry::ids() const
{
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.id);
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<IStyleAlgorithm> Registry::create(const std::string& id) const
{
    for (const Entry& e : entries_)
        if (e.id == id) return e.factory();
    return nullptr;
}

} // namespace pastiche
