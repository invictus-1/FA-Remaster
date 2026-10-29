// FA Remastered's own AI, native core, phases 2-3: unit ratings from the blueprints, the battle estimate, the build
// value and the unit / threat index (docs/ai-architecture.md 3.2 and 3.3, docs/ai-design-goals.md decisions 10-12).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code: nothing in src/ai/ is taken
// from M28AI or follows its functions (docs/ai-architecture.md 0).
//
// Unit ratings (UnitRating), made once per game from the blueprints (a small fixed table per blueprint, nothing per
// beat): each weapon as the battle estimate fires it (targets, dps, damage per shot, damage radius, range, indirect;
// beams, damage over time, salvos and racks counted; death, defence, upgrade-only, manual-fire and nuke weapons left
// out), health plus shield, range per target type, speed and mass. Ratings are sqrt(dps x effective health) divided by
// the same for the T1 tank uel0201 (1.0): the per-unit term of a square-law (Lanchester) strength. Per unit: the
// ratings per target type, the seven scores of decision 12 (against T1 / T2 / T3 armies and against one T1 / T2 / T3 /
// experimental unit), the THREAT (decision 11: raw, never cost; the label's number, e.g. "T1G 1.0"), perMass (a
// build-side number), tags. MOHO64_AI_RATINGS=<file> prints the lists (all, and stock only).
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
#include <map>
#include <string>
#include <utility>
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
  // Chris's points (SupComLab AI/rating_notes_from_chris_2026-10-28.md, his decision 11; docs/ai-architecture.md 3.3):
  // - single / value: against one big target, where every point of damage counts and splash adds nothing;
  // - group / valueGroup: against a group of units of the unit's own tier (GroupHealthOfTier: T1 300, T2 1,000, T3 and
  //   up 3,000 health, kGroupDensity apart): a shot bigger than one of them wastes the rest (overkill), a shot smaller
  //   needs whole shots (ceil), a damage radius hits several (splash). antiTag: "anti-group" (group dps at least the
  //   single dps), "anti-single" (at most half), "all-round";
  // - mainTarget: ground for land units and structures that hit the ground, naval for ships that hit ships, else the
  //   best;
  // - threat (the headline, the label's number): how dangerous the unit is, never its cost. 0.75 x its best score of
  //   the seven (its best role) + 0.25 x its group score against its own tier (Chris, decisions 11 and 12: a
  //   specialist is not punished for its off-role), x a range factor
  //   ((range / 18)^0.25, 0.7..1.8) and a speed factor ((speed / 3.4)^0.15, 0.8..1.25; 0.8 for a structure), plus a
  //   quarter of its rating against air (an aircraft: against the ground) for versatility. The T1 tank (range 18,
  //   speed 3.4) stays 1.0;
  // - perMass: threat per mass, relative to the T1 tank's: a build-side number (what a unit gives for its cost), not
  //   threat; the counter-pick against an actual enemy is BuildValue;
  // - rangeClass: short (< 30), medium (< 60), long (< 150), strategic; roles: tags for building mixes.
  float dpsGroup[kTargets] = {}, valueGroup[kTargets] = {};
  int mainTarget = 0;
  // Chris's decision 12: seven scores against the main target type, made once with the ratings (a fixed table per
  // blueprint; nothing per beat): groupVs[0..2] against an army of T1 / T2 / T3 units (300 / 1,000 / 3,000 health,
  // grouped: overkill, whole shots and splash per shot), singleVs[0..3] against one T1 / T2 / T3 / experimental unit
  // (300 / 1,000 / 3,000 / 50,000 health, alone: whole shots, no splash). Each is sqrt(dps there x (health + shield)) /
  // the T1 tank's, as the other ratings.
  float groupVs[3] = {}, singleVs[4] = {};
  float threat = 0, perMass = 0;
  std::string antiTag, rangeClass, roles;
  bool indirect = false;  // its main firepower is indirect (artillery, missiles): long-range standoff
  // Its weapons as the battle estimate fires them, in blueprint order
  struct Weapon {
    uint32_t vs = 0;  // target types (1 << Target)
    bool indirect = false;
    float dps = 0, damage = 0, radius = 0, range = 0;
  };
  std::vector<Weapon> weapons;
  int entity = -1;        // its blueprint's entity index
  // Chris's correction (phase 3; data/ai/lua/remastered/ai/rating_fixes.lua through Remastered.AISetRatingFix): a
  // multiplier on every rating of the unit (value, valueGroup, overall, perMass, best); the battle estimate scales the
  // unit's dps by fix^2, so that its rating, sqrt(dps x health), scales by fix. 1 until he sets one.
  float fix = 1;
  float indirectDps = 0;  // (the indirect weapons' dps, all targets)
  float dpsIndirect[kTargets] = {};  // the part of dps that is indirect
  float shieldRadius = 0;  // a bubble shield's radius (fire from inside it reaches the unit); 0: personal or none
};

constexpr float kGroupDensity = 1.0f / 12;  // units per square world unit in a fighting group (about 3.5 apart)
float GroupHealthOfTier(const std::string& tier);  // T1 300, T2 1000, T3 / T4 / C 3000, else 500
// The share of a weapon's damage that counts on targets of health h: one shot of `damage` on a unit it overkills
// counts h / damage; on a bigger one, whole shots (h / (ceil(h / damage) x damage)); grouped units take splash from a
// damage radius (1 + pi radius^2 kGroupDensity of them, at most 8).
float HitShare(float h, float damage, float radius, bool grouped);

// The battle estimate (docs/ai-architecture.md 3.3): two forces, each a list of (unit blueprint, count), fought out in
// half-second steps from `startDistance` apart (< 0: the longer of the two sides' ranges, the long-range side's
// opening). Every weapon fires while the gap is within its own range, at the enemy stacks it can hit in proportion to
// their health, with HitShare on the target's real health and shield (grouped below 3,000), indirect fire at half
// against mobile targets, and Chris's corrections (fix^2, "A beats B"); a shield takes hits first, except a bubble
// shield once the gap is inside its radius; the side with the shorter reach closes at its slowest mobile unit's speed
// (under fire), the other holds; at most 180 s. ours / theirs: the share of each side's health left; margin = 0.5 +
// (ours - theirs) / 2.
struct ForceStack {
  const UnitRating* r = nullptr;
  float count = 0;
};
struct BattleResult {
  float ours = 0, theirs = 0, margin = 0.5f, seconds = 0;
};
BattleResult EstimateBattle(const std::vector<ForceStack>& ours, const std::vector<ForceStack>& theirs,
                            float startDistance, const std::map<std::pair<int, int>, float>& beats);
// The build value (Chris's decision 11, the counter-pick): the unit, as many as the enemy force's mass, fought against
// it (EstimateBattle from the longer range): the enemy mass killed per own mass lost (own losses counted at least 5%
// of what was spent, so a clean win reads high but finite). killed / lost: the shares of each side's mass.
float BuildValue(const UnitRating& r, const std::vector<ForceStack>& enemy,
                 const std::map<std::pair<int, int>, float>& beats, float* killed, float* lost);

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
// Remastered.AIUnitRating, AIZoneForces, AIZoneThreat, AIIndexInfo, AISquareLaw, AIBattleEstimate, and phase 3's
// AIRandom, AISetRatingFix, AISetBeats, AISetVision (into the Remastered table, with the map calls).
void RegisterIndexBindings(lua_State* L);
bool WriteRatings(Sim& sim, const std::string& path);

}  // namespace ai
}  // namespace moho
