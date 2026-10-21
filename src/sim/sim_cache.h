// A cache of what the engine reads from a sim's own objects: blueprint values keyed by the BlueprintInfo's address,
// weapon values keyed by a blueprint table's address in the sim's Lua state, a category's members over the sim's
// blueprint indices, the air look-ahead pyramid of the sim's map. Such a key means nothing outside its sim, so each
// sim keeps its own: a SimCache is a slot (one per site, numbered as the sites are first reached), and Sim::caches
// holds that slot's container for the sim, made at its first use and gone with the sim.
//
//   static const SimCache<std::map<const BlueprintInfo*, Data>> cacheSlot;
//   auto& cache = cacheSlot.For(sim);  // this sim's map
//
// (Multicore step 1: the caches were process-wide statics, emptied when a sim ended, so two sims alive together
// shared and cleared each other's entries.) The slots are a fixed array, so a sim's containers never move. Filling
// one is not locked: a parallel stage may only read what the serial part of the beat has filled (docs/multicore.md).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace moho {

constexpr size_t kSimCacheSlots = 32;  // (16 sites today)

namespace detail {
inline size_t NewSimCacheSlot() {
  static std::atomic<size_t> next{0};
  const size_t s = next++;
  if (s >= kSimCacheSlots) {
    std::fprintf(stderr, "moho64: more SimCache sites than kSimCacheSlots (%zu): raise it in sim/sim_cache.h\n",
                 kSimCacheSlots);
    std::abort();
  }
  return s;
}
}  // namespace detail

template <class C>
class SimCache {
 public:
  SimCache() : slot_(detail::NewSimCacheSlot()) {}
  SimCache(const SimCache&) = delete;
  SimCache& operator=(const SimCache&) = delete;
  // This site's container in the sim (S: Sim, sim/sim.h), made at the first call.
  template <class S>
  C& For(const S& sim) const {
    std::shared_ptr<void>& p = sim.caches[slot_];
    if (!p) p = std::make_shared<C>();
    return *static_cast<C*>(p.get());
  }

 private:
  size_t slot_;
};

}  // namespace moho
