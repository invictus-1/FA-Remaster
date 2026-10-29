// FA Remastered's own AI, native core, phase 2: unit ratings from the blueprints and the unit / threat index
// (docs/ai-architecture.md 3.2 and 3.3, docs/ai-design-goals.md decision 10).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code: nothing in src/ai/ is taken
// from M28AI or follows its functions (docs/ai-architecture.md 0).
//
// Unit ratings (UnitRating): every unit blueprint gets, from its blueprint alone, its damage per second against
// ground, air, naval and structure targets (its weapons: damage x shots per firing x rate of fire; beams, damage over
// time, salvos and racks counted; death, defence, upgrade-only, manual-fire and nuke weapons left out), its health
// plus personal shield, its longest range per target type, speed and mass cost. Its rating against a target type is
// sqrt(dps x effective health) divided by the same for the reference unit (the UEF T1 tank uel0201, 24 dps x 300 hp:
// 1.0): the per-unit term of a square-law (Lanchester) strength, so a group's fighting strength is (sum of ratings)^2
// and two groups compare by their sums. A label for Chris's tuning list: tier + domain letter + value, e.g. "T1G 1.0"
// (tier T1 T2 T3, T4 = experimental, C = commander; G ground, A air, N naval, S structure, X experimental; the value is
// the unit's best rating over the four target types). MOHO64_AI_RATINGS=<file> prints the whole list.
//
// The index (per army that plays our AI, rebuilt by the engine every kIndexPeriod beats before the script threads run,
// and on a query before the first build): per zone (map_analysis.h) and side (own, allied, enemy, as that army sees
// them) and movement class (land, naval, hover, amphibious, air, static), the units' count, mass, summed ratings per
// target type, longest range and ratings by range band (short < 30, medium < 60, long < 150, strategic), the contacts
// whose type is unknown, and per zone and side the engineers, commanders, experimentals, shields and factories.
// - Fair intel: an enemy unit counts only while the army holds a blip of it (Unit::recon, kReconInUse), where the blip
//   is. Its type and rating are known once the army has had it in sight (LOSNow / LOSEver) or omni sees it; a radar or
//   sonar contact never seen counts as an unknown contact of its movement class (a player sees air, land or naval on
//   the radar icon). A jammer's fake blips count as unknown contacts until the army knows them as fakes (KnownFake);
//   a real blip marked KnownFake is left out. Own and allied units are known in full.
// - "Can it reach" (AIZoneThreat): the enemy strength that can come to a zone within a travel horizon or shoot into it:
//   mobile units by the map analysis' travel distance for their own movement class (a navy in a pond that touches no
//   land zone cannot come to one), aircraft by straight distance, and anything whose longest range covers the zone.
//   Strength against the asked target type only: a submarine adds nothing against ground.
// - Units under construction are left out; a damaged unit counts with sqrt(health fraction).
// The index reads the units, their blips and blueprints only (and the blueprints' Lua tables once, raw, when the
// ratings are made), in id order, and writes only its own state: a session without our AI never builds it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class Sim;

namespace ai {

constexpr int kIndexPeriod = 5;  // beats between index builds

// Target types and movement classes.
enum Target : int { kVsGround = 0, kVsAir = 1, kVsNaval = 2, kVsStructure = 3, kTargets = 4 };
enum MoveClass : int { kMoveLand = 0, kMoveNaval = 1, kMoveHover = 2, kMoveAmphibious = 3, kMoveAir = 4,
                       kMoveStatic = 5, kMoveClasses = 6 };
enum Band : int { kBandShort = 0, kBandMedium = 1, kBandLong = 2, kBandStrategic = 3, kBands = 4 };
int BandOf(float range);

enum UnitKind : uint32_t {
  kKindEngineer = 1, kKindCommander = 2, kKindExperimental = 4, kKindStructure = 8, kKindEconomy = 16,
  kKindShield = 32, kKindFactory = 64, kKindDirect = 128, kKindIndirect = 256, kKindAntiAir = 512,
  kKindAntiNavy = 1024, kKindStrategic = 2048,
};

struct UnitRating {
  bool valid = false;
  std::string id, name, label;
  std::string tier;    // "T1" .. "T4", "C"
  char domain = 'G';   // G A N S X
  // The tier comes from the categories TECH1 / TECH2 / TECH3 / EXPERIMENTAL / COMMAND. A mobile unit whose categories
  // give none, or one at least two tiers below what its mass says (a mod's 3,600-mass "TECH1" bot), takes the tier of
  // its mass band instead (TierOfMass; never lower than its categories'); tierGuessed says so, tierNote says why
  // ("TECH1, mass 3600"), the label gets a '*'.
  bool tierGuessed = false;
  std::string tierNote;
  std::string faction;  // UEF, Aeon, Cybran, Seraphim (its categories), else ""
  bool stock = false;   // the game's own playable unit: Source under /units/ (no mod), one of the four factions, and
                        // not civilian, a campaign operation's, a dummy or an effect
  int move = kMoveLand;
  uint32_t kinds = 0;
  float mass = 0, health = 0, shield = 0, speed = 0;
  float dps[kTargets] = {}, range[kTargets] = {}, value[kTargets] = {};
  float best = 0;      // max of value
  float maxRange = 0;  // max of range
};

// The ratings of the sim's unit blueprints, by entity index (made on the first call; nullptr for a non-unit index).
const UnitRating* RatingOf(Sim& sim, int entityIndex);
// Is this army one that plays our AI (AIPersonality "remastered...")?
bool PlaysOurAi(const Sim& sim, int army0);
// The engine's stage (Sim::Tick, before the script threads): rebuilds the index of every army that plays our AI every
// kIndexPeriod beats. Nothing in a session without our AI.
void IndexBeat(Sim& sim);
// The end of Sim::Start (AfterSessionStart): the ratings made in a session with our AI; MOHO64_AI_RATINGS=<file>
// makes and prints them in any session.
void IndexAfterSessionStart(Sim& sim);
// Remastered.AIUnitRating, AIZoneForces, AIZoneThreat, AIIndexInfo, AISquareLaw (into the Remastered table, with the
// map calls).
void RegisterIndexBindings(lua_State* L);
bool WriteRatings(Sim& sim, const std::string& path);

}  // namespace ai
}  // namespace moho
