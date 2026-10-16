// A cache of what the engine reads from a sim's own objects: blueprint values keyed by the BlueprintInfo's address,
// weapon values keyed by a blueprint table's address in the sim's Lua state, a category's members over the sim's
// blueprint indices. Such a key means nothing once its sim is gone, and a later sim in the same process (the
// interface's Relaunch: the first sim is destroyed, a second one made) may get the same addresses for other
// blueprints, or for the same ones while the cached values point into the dead sim (a footprint's path grid). So
// every SimCache is emptied when a sim ends (Sim::~Sim -> ClearSimCaches), and a game after a relaunch reads them
// afresh, as in a new process.
//
//   static SimCache<std::map<const BlueprintInfo*, Data>> cache;  // used as the map itself
//
// (One sim at a time: two sims alive together would share and clear each other's entries.)
#pragma once
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace moho {

namespace detail {
struct SimCacheList {
  std::mutex m;
  std::vector<std::pair<const void*, std::function<void()>>> clears;  // (the cache, how to empty it)
};
inline SimCacheList& SimCaches() {
  static SimCacheList l;
  return l;
}
}  // namespace detail

// Empty every SimCache (Sim::~Sim).
inline void ClearSimCaches() {
  detail::SimCacheList& l = detail::SimCaches();
  std::lock_guard<std::mutex> g(l.m);
  for (auto& c : l.clears) c.second();
}

template <class C>
struct SimCache : C {
  SimCache() {
    detail::SimCacheList& l = detail::SimCaches();
    std::lock_guard<std::mutex> g(l.m);
    l.clears.emplace_back(this, [this] { this->clear(); });
  }
  // (a sim destroyed while the process exits must not reach a cache whose static has already gone)
  ~SimCache() {
    detail::SimCacheList& l = detail::SimCaches();
    std::lock_guard<std::mutex> g(l.m);
    for (size_t i = 0; i < l.clears.size(); ++i)
      if (l.clears[i].first == this) {
        l.clears.erase(l.clears.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
  }
  SimCache(const SimCache&) = delete;
  SimCache& operator=(const SimCache&) = delete;
};

}  // namespace moho
