// The economy: what each army has in store, what its units produce, and how the resources are
// shared out among the units that ask for them (the original's CEconomy, CEconRequest,
// CEconStorage and CEconomyEvent).
//
// What the original does (FA exe, read 2026-10-08; see FINDINGS "Rebuild M4"):
// - Every army has an economy: resources in store, a maximum storage (a whole number: every
//   storage source adds its value truncated to an integer), and a list of requests. A request is
//   what one consumer asks for per tick (energy, mass) and what it was granted so far.
// - At the start of every beat each army's economy shares out what it has (Sim::AdvanceBeat ->
//   CArmyImpl::OnTick -> ArmyProcessEconomy 0x771b50): available = stored + this tick's income;
//   when the requests ask for more than is available, the scarcer resource sets one ratio for
//   every request that needs it (requests that need both resources get it in both); requests that
//   only need the other resource share what is left of that one. Overflow above the maximum goes
//   to allies with room when resource sharing is on; the rest is lost.
// - During the beat every unit tops up its request's grant into what it consumes (the fraction it
//   got is GetResourceConsumed()) and adds its production to the army's income for the next beat
//   (scaled by that fraction, unless Economy.NaturalProducer). Units being built produce nothing.
// - Economy events (CreateEconomyEvent) are requests of their own that take a fixed amount per
//   tick for a number of ticks and only advance in ticks where the whole amount was granted.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/script_object.h"

namespace moho {

class Sim;
class Army;
class Unit;

enum : int { kEnergy = 0, kMass = 1 };

// One consumer's claim on its army's economy (the original's CEconRequest).
struct EconRequest {
  float requested[2] = {0, 0};  // per tick
  float granted[2] = {0, 0};    // granted and not used yet
  bool live = true;             // false: the owner dropped it (removed from the army's list next beat)
  // LimitingRate: min over requested resources of granted/requested (1 when nothing is requested).
  float LimitingRate() const {
    float r = 1.0f;
    for (int i = 0; i < 2; ++i)
      if (requested[i] > 0 && granted[i] / requested[i] <= r) r = granted[i] / requested[i];
    return r;
  }
  // Use up to `amount` of the grant; returns what was taken.
  void Take(const float amount[2], float out[2]) {
    for (int i = 0; i < 2; ++i) {
      out[i] = amount[i] < granted[i] ? amount[i] : granted[i];
      float left = granted[i] - out[i];
      granted[i] = left > 0 ? left : 0;
    }
  }
};

// An army's economy (the original's CEconomy).
struct ArmyEconomy {
  float income[2] = {0, 0};       // added during the beat (production, gifts), shared out next beat
  float incomeTrend[2] = {0, 0};  // the same amounts, kept for the trend
  float stored[2] = {0, 0};
  float incomeStat[2] = {0, 0};   // last beat's income (GetEconomyIncome)
  float reclaimed[2] = {0, 0};
  float requested[2] = {0, 0};    // last beat's total requests (GetEconomyRequested)
  float usage[2] = {0, 0};        // last beat's grants (GetEconomyUsage)
  int64_t maxStorage[2] = {0, 0};
  float brainStorage[2] = {0, 0};  // CAiBrain:GiveStorage's own storage (replaced by each call)
  bool sharing = false;
  float peak[2] = {0, 0};
  std::vector<std::shared_ptr<EconRequest>> requests;  // oldest first
  std::shared_ptr<EconRequest> NewRequest() {
    auto r = std::make_shared<EconRequest>();
    requests.push_back(r);
    return r;
  }
  void ChangeStorage(int sign, const float amount[2]) {
    for (int i = 0; i < 2; ++i) maxStorage[i] += static_cast<int64_t>(sign) * static_cast<int64_t>(amount[i]);
  }
};

// CreateEconomyEvent(unit, energy, mass, seconds [, callback]): a timed drain on the unit's army.
class EconomyEvent : public ScriptObject {
 public:
  Unit* unit = nullptr;
  float perTick[2] = {0, 0};
  int ticksLeft = 1, ticksTotal = 1;
  int callbackRef = -2;  // LUA_NOREF
  std::shared_ptr<EconRequest> request;
  bool done = false;
  int EventState() const override { return done ? 1 : 0; }
};

// Start of a beat: per-tick unit figures reset, then every army shares out its resources.
void EconomyBeginBeat(Sim& sim);
// A unit's part of the beat (after its motion): economy events, regeneration or decay, and its
// consumption and production (Unit::HandleResourceManagement).
void UnitEconomyTick(Sim& sim, Unit* u);
// A unit leaves the world: its requests, storage and events go.
void UnitEconomyRelease(Unit* u);
// Unit creation: build rate, regeneration and the like from the blueprint.
void UnitEconomyInit(lua_State* L, Unit* u);
// Add `delta` (fraction of the whole) to a unit being built (Unit::Materialize): health grows with
// it; at 1 the unit is finished (OnStopBeingBuilt). Returns the fraction actually added.
float Materialize(Sim& sim, lua_State* L, Unit* u, float delta);
void RegisterEconomyBindings(lua_State* L);

}  // namespace moho
