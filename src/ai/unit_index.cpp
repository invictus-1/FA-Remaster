// FA Remastered's own AI, native core, phases 2-3: unit ratings, the battle estimate, the build value and the unit /
// threat index (see unit_index.h).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code (docs/ai-architecture.md 0).
#include "ai/unit_index.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <utility>

#include "ai/map_analysis.h"
#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/sim.h"
#include "sim/intel.h"
#include "sim/units.h"

namespace moho {

namespace combat {
int Relation(const Army* a, const Army* b);
}  // namespace combat

namespace ai {

float GroupHealthOfTier(const std::string& tier) {
  return tier == "T1" ? 300.0f : tier == "T2" ? 1000.0f : (tier == "T3" || tier == "T4" || tier == "C") ? 3000.0f : 500.0f;
}

float HitShare(float h, float damage, float radius, bool grouped) {
  if (!(damage > 0) || !(h > 0)) return 0;
  const float share = damage >= h ? h / damage : h / (std::ceil(h / damage) * damage);
  const float hits = grouped && radius > 0 ? std::min(8.0f, 1 + 3.14159265f * radius * radius * kGroupDensity) : 1.0f;
  return share * hits;
}

int BandOf(float range) {
  return range < 30 ? kBandShort : range < 60 ? kBandMedium : range < 150 ? kBandLong : kBandStrategic;
}

namespace {

constexpr float kRefDps = 24, kRefHealth = 300;  // uel0201's when it is not in the rules (see RatingsState)

// ---- raw reads of the blueprint tables (no metamethod, no script call; keys pushed as strings) ----

void RawField(lua_State* L, int t, const char* k) {
  if (!lua_istable(L, t)) {
    lua_pushnil(L);
    return;
  }
  lua_pushstring(L, k);
  lua_rawget(L, t);
}
float RawNum(lua_State* L, int t, const char* k, float def) {
  RawField(L, t, k);
  const float v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
bool RawBool(lua_State* L, int t, const char* k) {
  RawField(L, t, k);
  const bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}
std::string RawStr(lua_State* L, int t, const char* k) {
  RawField(L, t, k);
  const std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return v;
}
int RawLen(lua_State* L, int t) {  // entries 1..n before the first nil
  if (!lua_istable(L, t)) return 0;
  int n = 0;
  for (;;) {
    lua_rawgeti(L, t, n + 1);
    const bool nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (nil) return n;
    ++n;
  }
}

// One unit blueprint's rating from its table (at absolute index bpIdx).
void RateBlueprint(lua_State* L, int bpIdx, uint32_t catKinds, const std::string& tier, UnitRating* r) {
  const int top = lua_gettop(L);
  r->valid = true;
  RawField(L, bpIdx, "General");
  const int general = lua_gettop(L);
  std::string name = RawStr(L, general, "UnitName");
  if (name.empty()) name = RawStr(L, bpIdx, "Description");
  if (name.rfind("<LOC", 0) == 0 && name.find('>') != std::string::npos) name = name.substr(name.find('>') + 1);
  r->name = name;
  r->stock = RawStr(L, bpIdx, "Source").rfind("/units/", 0) == 0;
  RawField(L, bpIdx, "Physics");
  const int physics = lua_gettop(L);
  const std::string motion = RawStr(L, physics, "MotionType");
  if (motion == "RULEUMT_Air") r->move = kMoveAir;
  else if (motion == "RULEUMT_Land") r->move = kMoveLand;
  else if (motion == "RULEUMT_Amphibious" || motion == "RULEUMT_AmphibiousFloating") r->move = kMoveAmphibious;
  else if (motion == "RULEUMT_Hover") r->move = kMoveHover;
  else if (motion == "RULEUMT_Water" || motion == "RULEUMT_SurfacingSub") r->move = kMoveNaval;
  else r->move = kMoveStatic;
  RawField(L, bpIdx, "Air");
  r->speed = r->move == kMoveAir ? RawNum(L, lua_gettop(L), "MaxAirspeed", 0) : RawNum(L, physics, "MaxSpeed", 0);
  RawField(L, bpIdx, "Defense");
  const int defense = lua_gettop(L);
  r->health = RawNum(L, defense, "MaxHealth", 0);
  RawField(L, defense, "Shield");
  r->shield = RawNum(L, lua_gettop(L), "ShieldMaxHealth", 0);
  // a bubble shield (ShieldSize wider than the unit's own personal shield) does not stop fire from inside it
  r->shieldRadius = RawBool(L, lua_gettop(L), "PersonalShield") ? 0.0f : RawNum(L, lua_gettop(L), "ShieldSize", 0) / 2;
  RawField(L, bpIdx, "Economy");
  r->mass = RawNum(L, lua_gettop(L), "BuildCostMass", 0);
  r->kinds = catKinds;

  RawField(L, bpIdx, "Weapon");
  const int weapons = lua_gettop(L);
  const int nw = RawLen(L, weapons);
  for (int i = 1; i <= nw; ++i) {
    lua_rawgeti(L, weapons, i);
    const int w = lua_gettop(L);
    const std::string label = RawStr(L, w, "Label"), category = RawStr(L, w, "WeaponCategory"),
                      rangeCat = RawStr(L, w, "RangeCategory");
    bool skip = label == "DeathWeapon" || label.find("OverCharge") != std::string::npos || category == "Death" || category == "Teleport" || category == "Defense" ||
                rangeCat == "UWRC_Countermeasure" || !RawStr(L, w, "EnabledByEnhancement").empty() ||
                RawBool(L, w, "DummyWeapon") || RawBool(L, w, "ManualFire") || RawBool(L, w, "OverChargeWeapon");
    if (RawBool(L, w, "NukeWeapon")) {
      r->kinds |= kKindStrategic;
      skip = true;
    }
    const float damage = RawNum(L, w, "Damage", 0);
    if (skip || !(damage > 0)) {
      lua_settop(L, w - 1);
      continue;
    }
    // Which targets: the union of FireTargetLayerCapsTable's target layers (its iteration order cannot change a union)
    uint32_t vs = 0;
    RawField(L, w, "FireTargetLayerCapsTable");
    const int caps = lua_gettop(L);
    if (lua_istable(L, caps)) {
      lua_pushnil(L);
      while (lua_next(L, caps)) {
        if (lua_type(L, -1) == LUA_TSTRING) {
          const char* s = lua_tostring(L, -1);
          if (std::strstr(s, "Land")) vs |= (1u << kVsGround) | (1u << kVsStructure);
          if (std::strstr(s, "Air")) vs |= 1u << kVsAir;
          if (std::strstr(s, "Water") || std::strstr(s, "Sub") || std::strstr(s, "Seabed")) vs |= 1u << kVsNaval;
        }
        lua_pop(L, 1);
      }
    }
    lua_settop(L, caps - 1);
    // TargetRestrictOnlyAllow naming air and nothing on the ground or the water (an interceptor's "AIR"): air only
    {
      std::string only = RawStr(L, w, "TargetRestrictOnlyAllow");
      for (char& ch : only) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      if (only.find("AIR") != std::string::npos && only.find("LAND") == std::string::npos &&
          only.find("NAVAL") == std::string::npos && only.find("STRUCTURE") == std::string::npos)
        vs &= 1u << kVsAir;
    }
    if (!vs) {
      if (rangeCat == "UWRC_AntiAir") vs = 1u << kVsAir;
      else if (rangeCat == "UWRC_AntiNavy") vs = 1u << kVsNaval;
      else vs = (1u << kVsGround) | (1u << kVsStructure);
    }
    // Damage per second: one firing's damage x the firings per second.
    const float rof = RawNum(L, w, "RateOfFire", 1);
    const float dot = std::max(1.0f, RawNum(L, w, "DoTPulses", 1));
    const float beamLife = RawNum(L, w, "BeamLifetime", 0), beamDelay = RawNum(L, w, "BeamCollisionDelay", 0);
    float dps;
    if (RawBool(L, w, "ContinuousBeam")) {
      dps = damage * dot / (0.1f + beamDelay);  // a hit every collision check, all the time
    } else if (beamLife > 0) {
      dps = damage * dot * std::max(1.0f, std::floor(beamLife / (0.1f + beamDelay))) * rof;
    } else {
      RawField(L, w, "RackBones");
      const int racks = RawLen(L, lua_gettop(L));
      lua_pop(L, 1);
      // (the shots of one firing: the salvo per muzzle times the racks that fire together, or ProjectilesPerOnFire
      // where that is larger: FAF writes the same burst in either field, or in both with the same number)
      const float salvo = std::max(1.0f, RawNum(L, w, "MuzzleSalvoSize", 1));
      const float together = RawBool(L, w, "RackFireTogether") ? static_cast<float>(std::max(1, racks)) : 1.0f;
      const float perFire = std::max(1.0f, RawNum(L, w, "ProjectilesPerOnFire", 1));
      dps = damage * dot * std::max(salvo * together, perFire) * rof;
    }
    const float range = RawNum(L, w, "MaxRadius", 0);
    const float radius = RawNum(L, w, "DamageRadius", 0);
    const bool indirectW = rangeCat == "UWRC_IndirectFire" || category == "Artillery" || category == "Missile";
    if (indirectW) r->indirectDps += dps;
    UnitRating::Weapon wl;
    wl.vs = vs;
    wl.indirect = indirectW;
    wl.dps = dps;
    wl.damage = damage;
    wl.radius = radius;
    wl.range = range;
    r->weapons.push_back(wl);
    for (int t = 0; t < kTargets; ++t)
      if (vs & (1u << t)) {
        r->dps[t] += dps;
        if (indirectW) r->dpsIndirect[t] += dps;
        r->range[t] = std::max(r->range[t], range);
      }
    if (category == "Anti Air" || rangeCat == "UWRC_AntiAir") r->kinds |= kKindAntiAir;
    else if (category == "Anti Navy" || rangeCat == "UWRC_AntiNavy") r->kinds |= kKindAntiNavy;
    else if (rangeCat == "UWRC_IndirectFire" || category == "Artillery" || category == "Missile") r->kinds |= kKindIndirect;
    else r->kinds |= kKindDirect;
    lua_settop(L, w - 1);
  }
  lua_settop(L, top);
  r->tier = tier;
  r->domain = (catKinds & kKindExperimental) ? 'X'
              : r->move == kMoveStatic        ? 'S'
              : r->move == kMoveAir           ? 'A'
              : r->move == kMoveNaval         ? 'N'
                                              : 'G';
  for (int t = 0; t < kTargets; ++t) r->maxRange = std::max(r->maxRange, r->range[t]);
}

// The tier a mobile unit's mass puts it in, by movement (from the stock units' spread: a T1 tank 56, a T2 tank 198,
// a Percival 1440, a support commander 2000-2400; a T1 frigate 280, a destroyer 2250, a battleship 10000; a T1
// interceptor 50, a T2 gunship 336, a strategic bomber 2250; experimentals from about 5000 up). 0 for a structure
// (their masses overlap too much across tiers: a T3 mass fabricator costs less than a T2 artillery).
int TierOfMass(int move, float mass) {
  switch (move) {
    case kMoveAir: return mass < 150 ? 1 : mass < 500 ? 2 : mass < 3000 ? 3 : 4;
    case kMoveNaval: return mass < 600 ? 1 : mass < 4000 ? 2 : mass < 15000 ? 3 : 4;
    case kMoveStatic: return 0;
    default: return mass < 150 ? 1 : mass < 600 ? 2 : mass < 3000 ? 3 : 4;
  }
}

// The derived numbers and tags of a rated unit (UnitRating's comment): group dps and rating against its own tier,
// threat, perMass, tags. `ref` is the reference's sqrt(dps x health).
void Characterise(UnitRating& r, float ref) {
  const float ehp = r.health + r.shield;
  const float hg = GroupHealthOfTier(r.tier);
  for (int t = 0; t < kTargets; ++t) {
    r.dpsGroup[t] = 0;
    for (const UnitRating::Weapon& w : r.weapons)
      if (w.vs & (1u << t)) r.dpsGroup[t] += w.dps * HitShare(hg, w.damage, w.radius, true);
    r.valueGroup[t] = r.dpsGroup[t] > 0 ? std::sqrt(r.dpsGroup[t] * ehp) / ref : 0;
  }
  const int t = r.mainTarget;
  // the seven scores (decision 12) against the main target type
  static const float kGroupH[3] = {300, 1000, 3000}, kSingleH[4] = {300, 1000, 3000, 50000};
  auto score = [&](float h, bool grouped) {
    float d = 0;
    for (const UnitRating::Weapon& w : r.weapons)
      if (w.vs & (1u << t)) d += w.dps * HitShare(h, w.damage, w.radius, grouped);
    return d > 0 ? std::sqrt(d * ehp) / ref : 0.0f;
  };
  float best = 0;
  for (int i = 0; i < 3; ++i) best = std::max(best, r.groupVs[i] = score(kGroupH[i], true));
  for (int i = 0; i < 4; ++i) best = std::max(best, r.singleVs[i] = score(kSingleH[i], false));
  const float own = r.valueGroup[t];  // the group score against its own tier
  const float hi = best, lo = own;
  const float mr = r.range[t];
  const float rangeF = std::clamp(std::pow(std::max(mr, 1.0f) / 18.0f, 0.25f), 0.7f, 1.8f);
  const float speedF = r.move == kMoveStatic ? 0.8f : std::clamp(std::pow(std::max(r.speed, 0.1f) / 3.4f, 0.15f), 0.8f, 1.25f);
  // versatility: a quarter of what it can do against air (a surface unit) or against the ground (an aircraft whose
  // main target is air) (Chris: the Ythotha, a more versatile GC, rates above it)
  const int other = r.move == kMoveAir ? (t == kVsAir ? kVsGround : kVsAir) : kVsAir;
  r.threat = hi > 0 ? (0.75f * hi + 0.25f * lo) * rangeF * speedF + (other != t ? 0.25f * r.value[other] : 0.0f) : 0;
  // (single / group of the old list columns: value[t] and valueGroup[t] stay for the per-target-type tables)
  const float refMass = 56;  // uel0201's
  r.perMass = r.mass > 0 && r.threat > 0 ? (r.threat / r.mass) * refMass : 0;
  r.indirect = r.dps[t] > 0 && r.indirectDps >= 0.5f * r.dps[t];
  r.rangeClass = r.best <= 0 ? "-" : mr < 30 ? "short" : mr < 60 ? "medium" : mr < 150 ? "long" : "strategic";
  if (r.best <= 0) r.antiTag = "-";
  else if (r.dpsGroup[t] >= r.dps[t]) r.antiTag = "anti-group";
  else if (r.dpsGroup[t] <= 0.5f * r.dps[t]) r.antiTag = "anti-single";
  else r.antiTag = "all-round";
  std::string roles;
  auto add = [&](const char* tag) {
    if (!roles.empty()) roles += ',';
    roles += tag;
  };
  if (r.best > 0 && t == kVsAir) add("AA");
  else if (r.best > 0 && t == kVsNaval && r.dps[kVsGround] <= 0) add("torpedo");
  if (r.indirect) add("artillery");
  if (r.best > 0 && r.move != kMoveStatic && mr >= 60 && !r.indirect) add("standoff");
  const float ehpPerMass = r.mass > 0 ? ehp / r.mass : 0;  // uel0201: 300 / 56 = 5.4
  if (r.move != kMoveStatic && r.mass > 0 && ehpPerMass >= 2 * (300.0f / 56)) add("tank");
  if (r.move != kMoveStatic && r.best > 0 && r.mass > 0 && !(r.kinds & kKindExperimental) &&
      ehpPerMass <= 0.5f * (300.0f / 56))
    add("glass");
  if ((r.move == kMoveLand || r.move == kMoveHover || r.move == kMoveAmphibious) && r.speed >= 4) add("fast");
  if (r.kinds & kKindEngineer) add("engineer");
  if (r.kinds & kKindShield) add("shield");
  r.roles = roles.empty() ? "-" : roles;
}

struct RatingsState {
  bool made = false;
  std::vector<UnitRating> byEntity;  // by entity index (invalid for non-units)
  float ref = std::sqrt(kRefDps * kRefHealth);
  bool refFromRules = false;
  // Chris's "A beats B" (Remastered.AISetBeats): A's dps against B in the battle estimate x the factor; by entity
  // indices (an ordered map: never walked to decide anything, only looked up)
  std::map<std::pair<int, int>, float> beats;
};

RatingsState& Ratings(Sim& sim) {
  if (!sim.aiRatingsState) sim.aiRatingsState = std::make_shared<RatingsState>();
  RatingsState& st = *std::static_pointer_cast<RatingsState>(sim.aiRatingsState);
  if (st.made) return st;
  st.made = true;
  const SimBlueprints& bps = sim.blueprints();
  const int n = bps.EntityCount();
  st.byEntity.assign(n, UnitRating{});
  // Kinds and tiers from the categories (by name; no Lua).
  std::vector<uint32_t> kinds(n, 0);
  std::vector<int> tier(n, 0);  // 1..3, 4 experimental, 5 commander
  const struct {
    const char* cat;
    uint32_t kind;
  } kCats[] = {{"ENGINEER", kKindEngineer},     {"COMMAND", kKindCommander}, {"EXPERIMENTAL", kKindExperimental},
               {"STRUCTURE", kKindStructure},   {"MASSEXTRACTION", kKindEconomy}, {"MASSFABRICATION", kKindEconomy},
               {"ENERGYPRODUCTION", kKindEconomy}, {"SHIELD", kKindShield}, {"FACTORY", kKindFactory}};
  for (const auto& c : kCats) {
    auto it = bps.Categories().find(c.cat);
    if (it == bps.Categories().end()) continue;
    for (int i : it->second)
      if (i >= 0 && i < n) kinds[i] |= c.kind;
  }
  const struct {
    const char* cat;
    int t;
  } kTiers[] = {{"TECH1", 1}, {"TECH2", 2}, {"TECH3", 3}, {"EXPERIMENTAL", 4}, {"COMMAND", 5}};
  for (const auto& c : kTiers) {
    auto it = bps.Categories().find(c.cat);
    if (it == bps.Categories().end()) continue;
    for (int i : it->second)
      if (i >= 0 && i < n) tier[i] = std::max(tier[i], c.t);
  }
  // Not a unit to tune (civilians, campaign operation units, dummies and effects): left out of the stock list
  std::vector<char> notPlayable(n, 0);
  for (const char* cat : {"CIVILIAN", "OPERATION", "INSIGNIFICANTUNIT", "UNSELECTABLE", "DUMMYUNIT"}) {
    auto it = bps.Categories().find(cat);
    if (it == bps.Categories().end()) continue;
    for (int i : it->second)
      if (i >= 0 && i < n) notPlayable[i] = 1;
  }
  std::vector<char> builtBy(n, 0);  // in some BUILTBY... category (a factory, an engineer or a gate builds it)
  for (const auto& [name, members] : bps.Categories())
    if (name.rfind("BUILTBY", 0) == 0)
      for (int i : members)
        if (i >= 0 && i < n) builtBy[i] = 1;
  std::vector<const char*> faction(n, "");
  const struct {
    const char* cat;
    const char* name;
  } kFactions[] = {{"UEF", "UEF"}, {"AEON", "Aeon"}, {"CYBRAN", "Cybran"}, {"SERAPHIM", "Seraphim"}};
  for (const auto& c : kFactions) {
    auto it = bps.Categories().find(c.cat);
    if (it == bps.Categories().end()) continue;
    for (int i : it->second)
      if (i >= 0 && i < n && !*faction[i]) faction[i] = c.name;
  }
  lua_State* L = sim.L();
  const int top = lua_gettop(L);
  float refRaw = 0;
  for (int i = 0; i < n; ++i) {
    const BlueprintInfo* bp = bps.ByEntityIndex(i);
    if (!bp || bp->kind != BpKind::Unit) continue;
    UnitRating& r = st.byEntity[i];
    r.id = bp->id;
    r.entity = i;
    bps.PushTable(L, *bp);
    int t = tier[i];
    RateBlueprint(L, lua_gettop(L), kinds[i], "", &r);
    lua_settop(L, top);
    r.faction = faction[i];
    // (the stock list: the four factions' playable units; one nothing builds, other than a commander, or one with no
    // mass cost is no unit to tune: Fatboy's external factory uel0401ef, the drone uxl0021)
    if (notPlayable[i] || !*faction[i] || r.mass <= 0 || (!builtBy[i] && t != 5)) r.stock = false;
    // The tier: the categories', unless they give none or one two tiers off the unit's mass band (mobile units only)
    if (t != 5) {
      const int band = TierOfMass(r.move, r.mass);
      if (band > 0 && r.mass > 0 && (t == 0 || band >= t + 2)) {  // (only upward: a mod's T1-tagged giant)
        char note[64];
        std::snprintf(note, sizeof note, "%s, mass %.0f", t == 0 ? "no TECH category" : t == 4 ? "EXPERIMENTAL" : ("TECH" + std::to_string(t)).c_str(), r.mass);
        r.tierNote = note;
        r.tierGuessed = true;
        t = band;
      }
    }
    r.tier = t == 5 ? "C" : t >= 1 ? "T" + std::to_string(t) : "T?";
    if (t == 4 && r.move != kMoveStatic) r.domain = 'X';
    if (r.id == "uel0201") refRaw = std::sqrt(r.dps[kVsGround] * (r.health + r.shield));
  }
  if (refRaw > 0) {
    st.ref = refRaw;
    st.refFromRules = true;
  }
  for (UnitRating& r : st.byEntity) {
    if (!r.valid) continue;
    const float ehp = r.health + r.shield;
    r.best = 0;
    for (int t = 0; t < kTargets; ++t) {
      r.value[t] = r.dps[t] > 0 ? std::sqrt(r.dps[t] * ehp) / st.ref : 0;
      if (r.value[t] > r.best) {
        r.best = r.value[t];
        r.mainTarget = t;
      }
    }
    // The main target is the unit's natural one when it has any dps there: ground for land units and structures,
    // naval for ships (a land unit's torpedo or a ship's shore guns do not make it a naval or a land unit)
    if ((r.move == kMoveLand || r.move == kMoveHover || r.move == kMoveAmphibious || r.move == kMoveStatic) &&
        r.dps[kVsGround] > 0)
      r.mainTarget = kVsGround;
    else if (r.move == kMoveNaval && r.dps[kVsNaval] > 0)
      r.mainTarget = kVsNaval;
    Characterise(r, st.ref);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%c%s %.1f", r.tier.c_str(), r.domain, r.tierGuessed ? "*" : "", r.threat);
    r.label = buf;
  }
  return st;
}

// ---- the index ----

enum Side : int { kOwn = 0, kAlly = 1, kEnemy = 2, kSides = 3 };

struct Cell {  // per zone, side, movement class
  float count = 0, mass = 0, unknown = 0;
  float remembered = 0;  // enemy units counted from memory (seen before, not held now; see Memo)
  float str[kTargets] = {};
  float band[kBands] = {};  // best rating by the band of the unit's longest range
  float maxRange = 0;
};
struct ZoneSideExtra {
  float engineers = 0, commanders = 0, experimentals = 0, factories = 0, shieldHealth = 0, economyMass = 0;
};

// What an army remembers of an enemy unit it has seen (phase 3, docs/ai-architecture.md 3.2): where and what it last
// was. A structure is remembered until the army sees the place again without it; a mobile unit for kMemoryMobile beats,
// fading out. Only what the army saw goes in, so memory is fair intel.
constexpr uint32_t kMemoryMobile = 600;
struct Memo {
  Vec3 pos;
  int entity = -1;
  uint32_t tick = 0;
  bool structure = false;
  float hf = 1;
};

struct ArmyIndex {
  int army0 = -1;
  bool vision = false;  // the "AI vision" cheat option (Remastered.AISetVision): every enemy unit known, as omni
  std::mt19937 rng;      // its own random stream (Remastered.AIRandom), seeded on first use
  bool rngSeeded = false;
  std::map<uint32_t, Memo> memory;  // by entity id (walked in id order)
  uint32_t builtTick = 0xffffffffu;
  int units[kSides] = {}, unknown = 0, outside = 0;
  std::vector<Cell> cells;          // [zone][side][class]
  // The units themselves (phase 4), for the battle estimate of a zone (AIZoneEstimate): per zone and side, blueprint
  // entity index -> effective count (memory weight x health share), in entity order
  std::vector<std::map<int, float>> forces;  // [zone][side]
  std::vector<ZoneSideExtra> extra;  // [zone][side]
  Cell& At(int zone0, int side, int cls) { return cells[(static_cast<size_t>(zone0) * kSides + side) * kMoveClasses + cls]; }
  ZoneSideExtra& Extra(int zone0, int side) { return extra[static_cast<size_t>(zone0) * kSides + side]; }
};

struct IndexState {
  std::vector<ArmyIndex> armies;  // the armies that play our AI, by army index
  bool logBuilds = false;
};

IndexState* IndexOf(Sim& sim) { return static_cast<IndexState*>(sim.aiIndexState.get()); }

void BuildIndex(Sim& sim, ArmyIndex& ix) {
  const bool logBuilds = IndexOf(sim)->logBuilds;
  const auto t0 = logBuilds ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const MapAnalysis& m = MapOf(sim);
  RatingsState& rs = Ratings(sim);
  const int nz = static_cast<int>(m.zones.size());
  ix.cells.assign(static_cast<size_t>(nz) * kSides * kMoveClasses, Cell{});
  ix.forces.assign(static_cast<size_t>(nz) * kSides, std::map<int, float>{});
  ix.extra.assign(static_cast<size_t>(nz) * kSides, ZoneSideExtra{});
  for (int& u : ix.units) u = 0;
  ix.unknown = ix.outside = 0;
  const Army* me = sim.armies()[ix.army0].get();
  const int a = ix.army0;
  int enemiesAll = 0;
  auto ratingOf = [&](const Unit* u) -> const UnitRating* {
    if (!u->blueprint || u->blueprint->entityIndex < 0 ||
        u->blueprint->entityIndex >= static_cast<int>(rs.byEntity.size()))
      return nullptr;
    const UnitRating& r = rs.byEntity[u->blueprint->entityIndex];
    return r.valid ? &r : nullptr;
  };
  // a known unit's (or a remembered one's, with its weight) ratings into its cell
  auto addKnown = [&](int side, const Vec3& p, const UnitRating* r, float hf, float weight, bool remembered) {
    const int z = m.ZoneAt(p.x, p.z, -1);
    if (!z) {
      ++ix.outside;
      return;
    }
    if (!remembered) ++ix.units[side];
    ix.forces[static_cast<size_t>(z - 1) * kSides + side][r->entity] += weight * hf * hf;  // (hf = sqrt(health share))
    Cell& c = ix.At(z - 1, side, r->move);
    const float k = hf * r->fix * weight;
    c.count += weight;
    if (remembered) c.remembered += weight;
    c.mass += r->mass * weight;
    for (int t = 0; t < kTargets; ++t) c.str[t] += r->value[t] * k;
    c.band[BandOf(r->maxRange)] += r->best * k;
    c.maxRange = std::max(c.maxRange, r->maxRange);
    ZoneSideExtra& e = ix.Extra(z - 1, side);
    if (r->kinds & kKindEngineer) e.engineers += weight;
    if (r->kinds & kKindCommander) e.commanders += weight;
    if (r->kinds & kKindExperimental) e.experimentals += weight;
    if (r->kinds & kKindFactory) e.factories += weight;
    if (r->kinds & kKindShield) e.shieldHealth += r->shield * weight;
    if (r->kinds & kKindEconomy) e.economyMass += r->mass * weight;
  };
  const uint32_t now = sim.tick();
  auto addUnknown = [&](int side, const Vec3& p, int cls) {
    const int z = m.ZoneAt(p.x, p.z, -1);
    if (!z) {
      ++ix.outside;
      return;
    }
    Cell& c = ix.At(z - 1, side, cls);
    c.count += 1;
    c.unknown += 1;
    ++ix.unknown;
  };
  for (Unit* u : sim.units()) {  // (id order)
    if (u->dead || !u->army) continue;
    const int rel = u->army == me ? 2 : combat::Relation(me, u->army);
    const int side = u->army == me ? kOwn : rel == 2 ? kAlly : rel == 0 ? kEnemy : -1;
    if (side < 0) continue;
    const UnitRating* r = ratingOf(u);
    if (!r) continue;
    const float hf = u->maxHealth > 0 ? std::sqrt(std::clamp(u->health / u->maxHealth, 0.0f, 1.0f)) : 1.0f;
    if (side == kEnemy) {
      ++enemiesAll;
      if (ix.vision) {  // the cheat option: every enemy unit known where it is
        if (u->fractionComplete < 1) continue;
        addKnown(kEnemy, u->position, r, hf, 1, false);
        continue;
      }
      // a jammer's fake blips: unknown contacts until known as fakes (whatever the real unit's own blip says)
      for (const ReconBlip* f : u->fakeBlips)
        if (f && a < 16 && (f->rflags[a] & kReconInUse) && !(f->rflags[a] & 0x20)) addUnknown(kEnemy, f->position, r->move);
      const uint8_t flags = a < 16 ? u->recon[a] : 0;
      if (!(flags & kReconInUse) || (flags & 0x20)) continue;  // no blip, or a known fake
      const Vec3 p = u->blip ? u->blip->position : u->position;
      if (!(flags & (0x10 | 0x08 | 0x04))) {  // never seen, no omni: a contact of unknown type
        addUnknown(kEnemy, p, r->move);
        continue;
      }
      if (u->fractionComplete < 1) continue;
      addKnown(kEnemy, p, r, hf, 1, false);
      // what the army has seen goes into its memory (refreshed while it holds the blip)
      Memo& mo = ix.memory[u->id];
      mo.pos = p;
      mo.entity = r->entity;
      mo.tick = now;
      mo.structure = r->move == kMoveStatic;
      mo.hf = hf;
      continue;
    }
    if (u->fractionComplete < 1) continue;
    addKnown(side, u->position, r, hf, 1, false);
  }
  // Remembered enemies the army does not hold now: structures until the army sees their place again (vision there),
  // mobile units fading out over kMemoryMobile beats. In id order; entries refreshed this build are counted above.
  if (!ix.vision)
    for (auto it = ix.memory.begin(); it != ix.memory.end();) {
      Memo& mo = it->second;
      if (mo.tick == now) {
        ++it;
        continue;
      }
      const uint32_t age = now - mo.tick;
      bool forget = !mo.structure && age >= kMemoryMobile;
      if (!forget && mo.structure && (ReconAtPoint(sim, a, mo.pos, 0) & 8)) forget = true;  // seen again: not there
      if (forget || mo.entity < 0 || mo.entity >= static_cast<int>(rs.byEntity.size()) || !rs.byEntity[mo.entity].valid) {
        it = ix.memory.erase(it);
        continue;
      }
      const float weight = mo.structure ? 1.0f : 1.0f - static_cast<float>(age) / kMemoryMobile;
      addKnown(kEnemy, mo.pos, &rs.byEntity[mo.entity], mo.hf, weight, true);
      ++it;
    }
  ix.builtTick = sim.tick();
  if (logBuilds)  // (MOHO64_AI_INDEXLOG: the log only; enemiesAll and the time are not fair or exact, never given to Lua)
    Logf(LogLevel::Info, "moho64 ai: index army %d tick %u: own %d, allied %d, enemy known %d of %d, unknown contacts %d, "
                         "outside zones %d, %.3f ms", a + 1, sim.tick(), ix.units[kOwn], ix.units[kAlly], ix.units[kEnemy],
         enemiesAll, ix.unknown, ix.outside,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

ArmyIndex* ArmyIndexOf(Sim& sim, int army0) {
  IndexState* st = IndexOf(sim);
  if (!st) return nullptr;
  for (ArmyIndex& ix : st->armies)
    if (ix.army0 == army0) {
      if (ix.builtTick == 0xffffffffu) BuildIndex(sim, ix);
      return &ix;
    }
  return nullptr;
}

}  // namespace

const UnitRating* RatingOf(Sim& sim, int entityIndex) {
  RatingsState& st = Ratings(sim);
  if (entityIndex < 0 || entityIndex >= static_cast<int>(st.byEntity.size())) return nullptr;
  return st.byEntity[entityIndex].valid ? &st.byEntity[entityIndex] : nullptr;
}

bool PlaysOurAi(const Sim& sim, int army0) {
  if (army0 < 0 || army0 >= static_cast<int>(sim.armies().size())) return false;
  const Army& a = *sim.armies()[army0];
  return !a.human && a.personality.compare(0, 10, "remastered") == 0;
}

void IndexAfterSessionStart(Sim& sim) {
  if (sim.fixes && SessionHasOurAi(sim)) {
    auto st = std::make_shared<IndexState>();
    for (int i = 0; i < static_cast<int>(sim.armies().size()); ++i)
      if (PlaysOurAi(sim, i)) {
        st->armies.emplace_back();
        st->armies.back().army0 = i;
      }
    if (const char* l = std::getenv("MOHO64_AI_INDEXLOG"); l && *l == '1') st->logBuilds = true;
    sim.aiIndexState = st;
    Ratings(sim);
  }
  if (const char* f = std::getenv("MOHO64_AI_RATINGS"); f && *f) {
    if (WriteRatings(sim, f)) Logf(LogLevel::Info, "moho64 ai: unit ratings written to %s", f);
    else Logf(LogLevel::Error, "moho64 ai: cannot write the unit ratings %s", f);
  }
}

void IndexBeat(Sim& sim) {
  IndexState* st = IndexOf(sim);
  if (!st || sim.tick() % kIndexPeriod != 0) return;
  for (ArmyIndex& ix : st->armies) BuildIndex(sim, ix);
}

namespace {

// The two reference enemies of the lists' build-value columns (Chris, decision 11): a group of 3 Galactic Colossi and
// a spam of 60 UEF T1 tanks (empty when the rules lack them)
struct BuildRefs {
  std::vector<ForceStack> gc, t1;
};

// One list: the header, then a row per unit in `v`'s order.
bool WriteRatingList(const RatingsState& st, const std::string& path, const std::vector<const UnitRating*>& v,
                     const char* what, const BuildRefs& refs) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  static const char* kT = "GANS";
  std::fprintf(f, "# FA Remastered AI: unit ratings from the blueprints (src/ai/unit_index.h): %s, %zu units\n", what,
               v.size());
  std::fprintf(f, "# THREAT (the label's number; never cost): 0.75 x the best of the seven + 0.25 x its own tier's group score, x range and speed\n");
  std::fprintf(f, "#   factors, + a quarter of its rating against air (versatility); the T1 tank uel0201 = 1.0 (sqrt(dps x health) %.1f%s)\n",
               st.ref, st.refFromRules ? "" : ", built-in: not in the rules");
  std::fprintf(f, "# the seven scores (decision 12), sqrt(dps there x (health + shield)) / the T1 tank's, for the main target (tgt: G ground,\n");
  std::fprintf(f, "#   A air, N naval, S structure): gT1 gT2 gT3 against an army of T1 / T2 / T3 units (300 / 1000 / 3000 health;\n");
  std::fprintf(f, "#   overkill, whole shots and splash per shot), sT1 sT2 sT3 sX against one unit (300 / 1000 / 3000 / 50000 health)\n");
  std::fprintf(f, "# BUILD side (what a unit gives for its cost; not threat): perMass = threat per mass, the T1 tank's = 1.0;\n");
  std::fprintf(f, "#   bv GC / bv T1 = build value: enemy mass killed per own mass lost when as much of the unit as the enemy weighs\n");
  std::fprintf(f, "#   fights 3 Galactic Colossi / 60 UEF T1 tanks (battle estimate from the longer range; 20 = a clean win)\n");
  std::fprintf(f, "# label = tier + domain + threat: T1 T2 T3, T4 experimental, C commander; G ground, A air, N naval, S structure,\n");
  std::fprintf(f, "#   X experimental; a '*': the tier was guessed from the mass (the last column says why)\n");
  std::fprintf(f, "# range class short < 30, medium < 60, long < 150, strategic; fix = Chris's multiplier (1.0 until corrected)\n");
  std::fprintf(f, "%-11s %-11s %-8s %-26s %6s %6s %5s | %3s %7s %5s %5s | %5s %5s %5s %5s %5s %5s %5s | %7s | %6s %6s %6s | %-11s %-9s %-22s | %4s | %s\n",
               "label", "id", "faction", "name", "mass", "health", "shld", "tgt", "dps", "range", "speed", "gT1", "gT2",
               "gT3", "sT1", "sT2", "sT3", "sX", "THREAT", "perMas", "bv GC", "bv T1", "anti", "range", "roles", "fix",
               "tier note");
  for (const UnitRating* r : v) {
    const int t = r->mainTarget;
    const float bvGc = refs.gc.empty() ? -1 : BuildValue(*r, refs.gc, st.beats, nullptr, nullptr);
    const float bvT1 = refs.t1.empty() ? -1 : BuildValue(*r, refs.t1, st.beats, nullptr, nullptr);
    std::fprintf(f, "%-11s %-11s %-8s %-26.26s %6.0f %6.0f %5.0f | %3c %7.1f %5.0f %5.1f | %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f | %7.2f | %6.2f %6.2f %6.2f | %-11s %-9s %-22.22s | %4.2f | %s\n",
                 r->label.c_str(), r->id.c_str(), r->faction.empty() ? "-" : r->faction.c_str(), r->name.c_str(),
                 r->mass, r->health, r->shield, kT[t], r->dps[t], r->range[t], r->speed, r->groupVs[0], r->groupVs[1],
                 r->groupVs[2], r->singleVs[0], r->singleVs[1], r->singleVs[2], r->singleVs[3], r->threat, r->perMass,
                 bvGc, bvT1, r->antiTag.c_str(), r->rangeClass.c_str(), r->roles.c_str(), r->fix,
                 r->tierGuessed ? r->tierNote.c_str() : "");
  }
  return std::fclose(f) == 0;
}

int DomainOrder(char d) { return d == 'G' ? 0 : d == 'A' ? 1 : d == 'N' ? 2 : d == 'S' ? 3 : 4; }
int TierOrder(const std::string& t) { return t == "C" ? 5 : t == "T?" ? 6 : t.size() == 2 ? t[1] - '0' : 7; }
int FactionOrder(const std::string& f) {
  return f == "UEF" ? 0 : f == "Aeon" ? 1 : f == "Cybran" ? 2 : f == "Seraphim" ? 3 : 4;
}

}  // namespace

// MOHO64_AI_RATINGS=<file>: <file> lists every unit (mods too) by domain, tier, rating; <file> with "_stock" before
// its extension lists the game's own units only (no mod's), by faction, tier, domain, rating: the list Chris tunes first.
bool WriteRatings(Sim& sim, const std::string& path) {
  RatingsState& st = Ratings(sim);
  std::vector<const UnitRating*> all, stock;
  for (const UnitRating& r : st.byEntity)
    if (r.valid) {
      all.push_back(&r);
      if (r.stock) stock.push_back(&r);
    }
  std::stable_sort(all.begin(), all.end(), [](const UnitRating* p, const UnitRating* q) {
    if (DomainOrder(p->domain) != DomainOrder(q->domain)) return DomainOrder(p->domain) < DomainOrder(q->domain);
    if (TierOrder(p->tier) != TierOrder(q->tier)) return TierOrder(p->tier) < TierOrder(q->tier);
    if (p->threat != q->threat) return p->threat > q->threat;
    return p->id < q->id;
  });
  std::stable_sort(stock.begin(), stock.end(), [](const UnitRating* p, const UnitRating* q) {
    if (FactionOrder(p->faction) != FactionOrder(q->faction)) return FactionOrder(p->faction) < FactionOrder(q->faction);
    if (TierOrder(p->tier) != TierOrder(q->tier)) return TierOrder(p->tier) < TierOrder(q->tier);
    if (DomainOrder(p->domain) != DomainOrder(q->domain)) return DomainOrder(p->domain) < DomainOrder(q->domain);
    if (p->threat != q->threat) return p->threat > q->threat;
    return p->id < q->id;
  });
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  const std::string stockPath = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                                    ? path.substr(0, dot) + "_stock" + path.substr(dot)
                                    : path + "_stock";
  BuildRefs refs;
  for (const UnitRating& r : st.byEntity) {
    if (r.valid && r.id == "ual0401") refs.gc.push_back({&r, 3});
    if (r.valid && r.id == "uel0201") refs.t1.push_back({&r, 60});
  }
  return WriteRatingList(st, path, all, "every unit, mods included", refs) &&
         WriteRatingList(st, stockPath, stock, "the game's own units (the four factions; no mod), by faction", refs);
}

// ---- the battle estimate ----

namespace {
int TargetTypeOf(const UnitRating& r) {
  return r.move == kMoveAir ? kVsAir : r.move == kMoveNaval ? kVsNaval : r.move == kMoveStatic ? kVsStructure : kVsGround;
}
}  // namespace

BattleResult EstimateBattle(const std::vector<ForceStack>& ours, const std::vector<ForceStack>& theirs,
                            float startDistance, const std::map<std::pair<int, int>, float>& beats, bool kite) {
  struct Stack {
    const UnitRating* r;
    float hpEach, shEach, hp, sh;  // per unit and pooled; the shield takes hits first (from outside its bubble)
    int tt;                        // its target type
    bool mobile;
    float arrive;                  // when it joins (reinforcements)
  };
  std::vector<Stack> side[2];
  float pool0[2] = {0, 0};
  for (int k = 0; k < 2; ++k)
    for (const ForceStack& f : k == 0 ? ours : theirs) {
      if (!f.r || !(f.count > 0)) continue;
      const float hpEach = std::max(1.0f, f.r->health), shEach = std::max(0.0f, f.r->shield);
      side[k].push_back({f.r, hpEach, shEach, hpEach * f.count, shEach * f.count, TargetTypeOf(*f.r),
                         f.r->move != kMoveStatic, std::max(0.0f, f.arrive)});
      pool0[k] += (hpEach + shEach) * f.count;
    }
  BattleResult res;
  if (pool0[0] <= 0 || pool0[1] <= 0) {
    res.ours = pool0[0] > 0 ? 1 : 0;
    res.theirs = pool0[1] > 0 ? 1 : 0;
    res.margin = 0.5f + (res.ours - res.theirs) / 2;
    return res;
  }
  float t = 0;
  auto alive = [&](const Stack& s) { return s.hp > 0 && s.arrive <= t; };  // (not yet arrived: not in the fight)
  // a side's reach: its longest weapon range against any target type the other side still has
  auto reach = [&](int k) {
    float rr = 0;
    for (const Stack& s : side[k]) {
      if (!alive(s)) continue;
      for (const UnitRating::Weapon& w : s.r->weapons)
        for (const Stack& e : side[1 - k])
          if (alive(e) && (w.vs & (1u << e.tt))) rr = std::max(rr, w.range);
    }
    return rr;
  };
  // a side's engage distance: the range within which three quarters of its dps against what the other side still has
  // fires (its weapons by range, longest first, until 75% of the dps is in): it closes in until then. A Fatboy's
  // turrets carry 75% at 100; a Loyalist needs its 20-range cannon, not only its 24-range bolter.
  auto engage = [&](int k) {
    std::vector<std::pair<float, float>> wr;  // (range, dps)
    float total = 0;
    for (const Stack& s : side[k])
      if (alive(s))
        for (const UnitRating::Weapon& w : s.r->weapons)
          for (const Stack& e : side[1 - k])
            if (alive(e) && (w.vs & (1u << e.tt))) {
              wr.push_back({w.range, w.dps * (s.hp / s.hpEach)});
              total += wr.back().second;
              break;
            }
    std::stable_sort(wr.begin(), wr.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    float sum = 0;
    for (const auto& [range, dps] : wr) {
      sum += dps;
      if (sum >= 0.75f * total) return range;
    }
    return 0.0f;
  };
  auto speedOf = [&](int k) {  // the slowest mobile unit alive; 0: none can move
    float v = 0;
    bool any = false;
    for (const Stack& s : side[k])
      if (alive(s) && s.mobile && s.r->speed > 0) {
        v = any ? std::min(v, s.r->speed) : s.r->speed;
        any = true;
      }
    return v;
  };
  // One weapon's dps on one enemy unit kind: HitShare on its real health and shield (grouped below 3,000), indirect
  // fire at half against a mobile target (shells in flight miss what moves: kIndirectVsMobile), x Chris's fix^2 and
  // his "A beats B" factor
  constexpr float kIndirectVsMobile = 0.5f;
  auto dpsOn = [&](const Stack& s, const UnitRating::Weapon& w, const Stack& e) {
    const float h = e.hpEach + e.shEach;
    float dps = w.dps * HitShare(h, w.damage, w.radius, h < 3000);
    if (w.indirect && e.mobile) dps *= kIndirectVsMobile;
    dps *= s.r->fix * s.r->fix;
    if (!beats.empty()) {
      auto it = beats.find({s.r->entity, e.r->entity});
      if (it != beats.end()) dps *= it->second;
    }
    return dps;
  };
  float d = startDistance >= 0 ? startDistance : std::max(reach(0), reach(1));
  const float dt = 0.5f;
  std::vector<float> dmg[2];
  for (; t < 180; t += dt) {
    bool anyAlive[2] = {false, false}, anyLater[2] = {false, false};
    for (int k = 0; k < 2; ++k)
      for (const Stack& s : side[k]) {
        anyAlive[k] = anyAlive[k] || alive(s);
        anyLater[k] = anyLater[k] || (s.hp > 0 && s.arrive > t);
      }
    if ((!anyAlive[0] && !anyLater[0]) || (!anyAlive[1] && !anyLater[1])) break;
    for (int k = 0; k < 2; ++k) dmg[k].assign(side[k].size(), 0.0f);
    bool fired = false;
    for (int k = 0; k < 2; ++k)
      for (const Stack& s : side[k]) {
        if (!alive(s)) continue;
        const float n = s.hp / s.hpEach;  // units left (continuous)
        for (const UnitRating::Weapon& w : s.r->weapons) {
          if (d > w.range) continue;
          float hittable = 0, room = 0;
          for (const Stack& e : side[1 - k])
            if (alive(e) && (w.vs & (1u << e.tt))) {
              hittable += e.hp + e.sh;
              // the front: around each target only so many shooters fit within this weapon's range (its ring's area
              // at the group density); a swarm of short-range units cannot all fire at a few big targets at once
              room += (e.hp / e.hpEach) * 3.14159265f * (w.range + 2) * (w.range + 2) * kGroupDensity;
            }
          if (hittable <= 0) continue;
          fired = true;
          const float firing = std::min(n, room);
          for (size_t j = 0; j < side[1 - k].size(); ++j) {
            const Stack& e = side[1 - k][j];
            if (!alive(e) || !(w.vs & (1u << e.tt))) continue;
            dmg[1 - k][j] += firing * dpsOn(s, w, e) * dt * ((e.hp + e.sh) / hittable);
          }
        }
      }
    for (int k = 0; k < 2; ++k)
      for (size_t j = 0; j < side[k].size(); ++j) {
        Stack& e = side[k][j];
        float x = dmg[k][j];
        if (e.sh > 0 && d >= e.r->shieldRadius) {  // (inside a bubble shield the fire reaches the unit itself)
          const float took = std::min(e.sh, x);
          e.sh -= took;
          x -= took;
        }
        e.hp -= x;
      }
    // a side whose main weapons cannot all fire yet closes in, at its slowest unit's speed, unless its main weapons
    // already outrange everything the other side has: then it holds and lets the other come (a Fatboy waits for the
    // Percivals; a Loyalist walks in to its cannon's range even though its bolter outranges a T2 tank)
    bool moved = false;
    const float ea = engage(0), eb = engage(1), ra = reach(0), rb = reach(1);
    float closing = 0;
    for (int k = 0; k < 2; ++k)
      if (d > (k == 0 ? ea : eb) && !((k == 0 ? ea : eb) >= (k == 0 ? rb : ra))) {
        const float v = speedOf(k);
        if (v > 0) {
          d = std::max(0.0f, d - v * dt);
          closing = std::max(closing, v);
          moved = true;
        }
      }
    // kiting: a side holding because its main weapons outrange the other backs off as the other closes, at its own
    // speed, not past its engage range
    if (kite && closing > 0)
      for (int k = 0; k < 2; ++k) {
        const float ek = k == 0 ? ea : eb, other = k == 0 ? rb : ra;
        if (ek >= other && ek > 0) {
          const float v = speedOf(k);
          if (v > 0) d = std::min(ek, d + v * dt);
        }
      }
    bool waiting = false;  // reinforcements still on the way: the fight is not over while they come
    for (int k = 0; k < 2; ++k)
      for (const Stack& s : side[k]) waiting = waiting || (s.hp > 0 && s.arrive > t);
    if (!fired && !moved && !waiting) break;  // nobody can fire, close or arrive: a standoff without a fight
  }
  float left[2] = {0, 0};
  for (int k = 0; k < 2; ++k)
    for (const Stack& s : side[k])
      if (s.hp > 0) left[k] += s.hp + std::max(0.0f, s.sh);  // (reinforcements that never came still count)
  res.ours = left[0] / pool0[0];
  res.theirs = left[1] / pool0[1];
  res.margin = 0.5f + (res.ours - res.theirs) / 2;
  res.seconds = t;
  return res;
}

float BuildValue(const UnitRating& r, const std::vector<ForceStack>& enemy,
                 const std::map<std::pair<int, int>, float>& beats, float* killed, float* lost) {
  float enemyMass = 0;
  for (const ForceStack& f : enemy)
    if (f.r) enemyMass += f.r->mass * f.count;
  if (!(r.mass > 0) || !(enemyMass > 0)) {
    if (killed) *killed = 0;
    if (lost) *lost = 0;
    return 0;
  }
  const BattleResult b = EstimateBattle({{&r, enemyMass / r.mass}}, enemy, -1.0f, beats);
  const float k = 1 - b.theirs, l = 1 - b.ours;  // shares lost (health stands in for mass)
  if (killed) *killed = k;
  if (lost) *lost = l;
  return (k * enemyMass) / (std::max(l, 0.05f) * enemyMass);
}

// ---- Remastered.AI* ----

namespace {

const char* kTargetNames[kTargets] = {"ground", "air", "naval", "structure"};
int TargetFromName(const char* s) {
  for (int t = 0; t < kTargets; ++t)
    if (s && !std::strcmp(s, kTargetNames[t])) return t;
  return -1;
}

void SetNum(lua_State* L, const char* k, lua_Number v) {
  lua_pushstring(L, k);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}

// A unit blueprint's rating as a table.
void PushRating(lua_State* L, const UnitRating& r) {
  lua_newtable(L);
  lua_pushstring(L, "id");
  lua_pushstring(L, r.id.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "label");
  lua_pushstring(L, r.label.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "tier");
  lua_pushstring(L, r.tier.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "domain");
  const char d[2] = {r.domain, 0};
  lua_pushstring(L, d);
  lua_rawset(L, -3);
  SetNum(L, "mass", r.mass);
  SetNum(L, "health", r.health);
  SetNum(L, "shield", r.shield);
  SetNum(L, "speed", r.speed);
  SetNum(L, "best", r.best * r.fix);
  SetNum(L, "maxRange", r.maxRange);
  SetNum(L, "kinds", r.kinds);
  for (const char* what : {"dps", "dpsGroup", "range", "rating", "ratingGroup"}) {  // (group: against its own tier)
    lua_pushstring(L, what);
    lua_newtable(L);
    const float* src = !std::strcmp(what, "dps")        ? r.dps
                       : !std::strcmp(what, "dpsGroup") ? r.dpsGroup
                       : !std::strcmp(what, "range")    ? r.range
                       : !std::strcmp(what, "rating")   ? r.value
                                                        : r.valueGroup;
    const float k = (src == r.value || src == r.valueGroup) ? r.fix : 1.0f;  // (Chris's correction on the ratings)
    for (int t = 0; t < kTargets; ++t) SetNum(L, kTargetNames[t], src[t] * k);
    if (src == r.value) {  // rating.group = {t1, t2, t3}, rating.single = {t1, t2, t3, x} (decision 12; main target)
      for (int g = 0; g < 2; ++g) {
        lua_pushstring(L, g == 0 ? "group" : "single");
        lua_newtable(L);
        const int n = g == 0 ? 3 : 4;
        for (int i = 0; i < n; ++i) {
          lua_pushnumber(L, (g == 0 ? r.groupVs[i] : r.singleVs[i]) * r.fix);
          lua_rawseti(L, -2, i + 1);
        }
        lua_rawset(L, -3);
      }
    }
    lua_rawset(L, -3);
  }
  SetNum(L, "threat", r.threat * r.fix);
  SetNum(L, "perMass", r.perMass * r.fix);
  SetNum(L, "fix", r.fix);
  for (const auto& kv : {std::make_pair("mainTarget", std::string(kTargetNames[r.mainTarget])),
                         std::make_pair("anti", r.antiTag), std::make_pair("rangeClass", r.rangeClass),
                         std::make_pair("roles", r.roles)}) {
    lua_pushstring(L, kv.first);
    lua_pushstring(L, kv.second.c_str());
    lua_rawset(L, -3);
  }
}

// Remastered.AIUnitRating(blueprintId) -> the rating table, or nil for no such unit
int l_AIUnitRating(lua_State* L) {
  Sim& sim = *Sim::From(L);
  const BlueprintInfo* bp = sim.blueprints().Find(luaL_checkstring(L, 1));
  const UnitRating* r = bp && bp->kind == BpKind::Unit ? RatingOf(sim, bp->entityIndex) : nullptr;
  if (!r) {
    lua_pushnil(L);
    return 1;
  }
  PushRating(L, *r);
  return 1;
}

ArmyIndex& CheckArmyIndex(lua_State* L, int i) {
  Sim& sim = *Sim::From(L);
  const int army = static_cast<int>(luaL_checknumber(L, i));
  ArmyIndex* ix = ArmyIndexOf(sim, army - 1);
  if (!ix) luaL_argerror(L, i, "not an army that plays our AI");
  return *ix;
}
int CheckZoneArg(lua_State* L, int i, const MapAnalysis& m) {
  const int z = static_cast<int>(luaL_checknumber(L, i));
  if (z < 1 || z > static_cast<int>(m.zones.size())) luaL_argerror(L, i, "no such zone");
  return z;
}

// Remastered.AIZoneForces(army, zone) -> {own = S, ally = S, enemy = S}, each S = {land = D, naval = D, air = D,
// structure = D, engineers, commanders, experimentals, factories, shieldHealth, economyMass}, each D = {count, mass,
// unknown, maxRange, ground, air, naval, structure (summed ratings against each target type)}; land sums the land,
// hover and amphibious classes
int l_AIZoneForces(lua_State* L) {
  Sim& sim = *Sim::From(L);
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  const MapAnalysis& m = MapOf(sim);
  const int z0 = CheckZoneArg(L, 2, m) - 1;
  static const char* kSideNames[kSides] = {"own", "ally", "enemy"};
  lua_newtable(L);
  for (int s = 0; s < kSides; ++s) {
    lua_pushstring(L, kSideNames[s]);
    lua_newtable(L);
    const struct {
      const char* name;
      int classes[3];
      int n;
    } kDomains[] = {{"land", {kMoveLand, kMoveHover, kMoveAmphibious}, 3},
                    {"naval", {kMoveNaval}, 1},
                    {"air", {kMoveAir}, 1},
                    {"structure", {kMoveStatic}, 1}};
    for (const auto& d : kDomains) {
      Cell sum;
      for (int k = 0; k < d.n; ++k) {
        const Cell& c = ix.At(z0, s, d.classes[k]);
        sum.count += c.count;
        sum.mass += c.mass;
        sum.unknown += c.unknown;
        sum.remembered += c.remembered;
        sum.maxRange = std::max(sum.maxRange, c.maxRange);
        for (int t = 0; t < kTargets; ++t) sum.str[t] += c.str[t];
      }
      lua_pushstring(L, d.name);
      lua_newtable(L);
      SetNum(L, "count", sum.count);
      SetNum(L, "mass", sum.mass);
      SetNum(L, "unknown", sum.unknown);
      SetNum(L, "remembered", sum.remembered);
      SetNum(L, "maxRange", sum.maxRange);
      for (int t = 0; t < kTargets; ++t) SetNum(L, kTargetNames[t], sum.str[t]);
      lua_rawset(L, -3);
    }
    const ZoneSideExtra& e = ix.Extra(z0, s);
    SetNum(L, "engineers", e.engineers);
    SetNum(L, "commanders", e.commanders);
    SetNum(L, "experimentals", e.experimentals);
    SetNum(L, "factories", e.factories);
    SetNum(L, "shieldHealth", e.shieldHealth);
    SetNum(L, "economyMass", e.economyMass);
    lua_rawset(L, -3);
  }
  return 1;
}

// Remastered.AIZoneThreat(army, zone, target ['ground'|'air'|'naval'|'structure'], horizon [, from]) -> enemy,
// friendly: the summed ratings against that target type of the enemy's (and of own + allied) units that can come to
// the zone within `horizon` world units of travel (by their own movement class; aircraft in a straight line) or whose
// longest range reaches the zone; unknown contacts not included (AIZoneForces counts them). `from` ('land', 'naval',
// 'air', 'structure') counts only units of that domain (land = land, hover and amphibious).
int l_AIZoneThreat(lua_State* L) {
  Sim& sim = *Sim::From(L);
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  const MapAnalysis& m = MapOf(sim);
  const int z = CheckZoneArg(L, 2, m);
  const int t = TargetFromName(luaL_checkstring(L, 3));
  if (t < 0) luaL_argerror(L, 3, "target: 'ground', 'air', 'naval' or 'structure'");
  const float horizon = luaL_checknumber(L, 4);
  int fromMask = (1 << kMoveClasses) - 1;
  if (!lua_isnoneornil(L, 5)) {
    const std::string from = luaL_checkstring(L, 5);
    fromMask = from == "land"        ? (1 << kMoveLand) | (1 << kMoveHover) | (1 << kMoveAmphibious)
               : from == "naval"     ? 1 << kMoveNaval
               : from == "air"       ? 1 << kMoveAir
               : from == "structure" ? 1 << kMoveStatic
                                     : 0;
    if (!fromMask) luaL_argerror(L, 5, "from: 'land', 'naval', 'air' or 'structure'");
  }
  const Zone& zt = m.zones[z - 1];
  const float radius = std::sqrt(static_cast<float>(zt.segments) * MapAnalysis::kSeg * MapAnalysis::kSeg / 3.14159265f);
  float enemy = 0, friendly = 0;
  for (int zs = 1; zs <= static_cast<int>(m.zones.size()); ++zs) {
    const Zone& from = m.zones[zs - 1];
    const float dx = from.x - zt.x, dz = from.z - zt.z;
    const float straight = std::sqrt(dx * dx + dz * dz);
    for (int s = 0; s < kSides; ++s)
      for (int c = 0; c < kMoveClasses; ++c) {
        if (!(fromMask & (1 << c))) continue;
        const Cell& cell = ix.At(zs - 1, s, c);
        if (!(cell.str[t] > 0)) continue;
        bool reach = cell.maxRange + radius >= straight;  // it can shoot into the zone from where it is
        if (!reach) {
          if (c == kMoveAir) reach = straight <= horizon;
          else if (c != kMoveStatic) {
            const float d = m.Distance(zs, z, c);  // kMoveLand..kMoveAmphibious are the map's domains
            reach = d >= 0 && d <= horizon;
          }
        }
        if (!reach) continue;
        (s == kEnemy ? enemy : friendly) += cell.str[t];
      }
  }
  lua_pushnumber(L, enemy);
  lua_pushnumber(L, friendly);
  return 2;
}

// Remastered.AIIndexInfo(army) -> {tick (of the last build), own, ally, enemy (known units), unknown (contacts),
// outside (units outside every zone), period}
int l_AIIndexInfo(lua_State* L) {
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  lua_newtable(L);
  SetNum(L, "tick", ix.builtTick);
  SetNum(L, "own", ix.units[kOwn]);
  SetNum(L, "ally", ix.units[kAlly]);
  SetNum(L, "enemy", ix.units[kEnemy]);
  SetNum(L, "unknown", ix.unknown);
  SetNum(L, "outside", ix.outside);
  SetNum(L, "period", kIndexPeriod);
  return 1;
}

// Remastered.AISquareLaw(ours, theirs) -> margin, survivors: two summed ratings (AIZoneThreat's, AIZoneForces') fought
// out by the square law: margin = ours^2 / (ours^2 + theirs^2) (0.5 even, 1 we win outright) and the share of the
// winner's strength left at the end, sqrt(1 - (loser / winner)^2), negative when it is theirs (0 for an even fight).
int l_AISquareLaw(lua_State* L) {
  const float a = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 1)));
  const float b = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 2)));
  const float a2 = a * a, b2 = b * b;
  lua_pushnumber(L, a2 + b2 > 0 ? a2 / (a2 + b2) : 0.5f);
  lua_pushnumber(L, a > b ? std::sqrt(1 - b2 / a2) : b > a ? -std::sqrt(1 - a2 / b2) : 0.0f);
  return 2;
}

// A force argument: an array of {blueprintId, count} (an array, so its order is fixed)
std::vector<ForceStack> CheckForce(lua_State* L, int i) {
  luaL_checktype(L, i, LUA_TTABLE);
  Sim& sim = *Sim::From(L);
  std::vector<ForceStack> out;
  for (int k = 1;; ++k) {
    lua_rawgeti(L, i, k);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    if (!lua_istable(L, -1)) luaL_argerror(L, i, "a force is an array of {blueprintId, count}");
    lua_rawgeti(L, -1, 1);
    lua_rawgeti(L, -2, 2);
    const char* id = lua_tostring(L, -2);
    const float count = static_cast<float>(lua_tonumber(L, -1));
    const BlueprintInfo* bp = id ? sim.blueprints().Find(id) : nullptr;
    const UnitRating* r = bp && bp->kind == BpKind::Unit ? RatingOf(sim, bp->entityIndex) : nullptr;
    if (!r) luaL_argerror(L, i, "not a unit blueprint id");
    out.push_back({r, count});
    lua_pop(L, 3);
  }
  return out;
}

// Remastered.AIEnemyMix(army [, all]) -> {{id, count, mass}, ...}: the enemy as the army knows it (phase 5): its
// index's enemy units (fair intel and the memory of lost contacts, with their weights) summed over the map, by
// blueprint entity order. Without `all`, the mobile army only, for the counter-pick: land, hover, amphibious and air
// units, engineers and commanders left out (the form AIBuildValue takes as its enemy). With `all`, every known enemy
// unit and structure (the strategy layer's read of the enemy's economy and tech).
int l_AIEnemyMix(lua_State* L) {
  Sim& sim = *Sim::From(L);
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  const bool all = lua_toboolean(L, 2) != 0;
  RatingsState& rs = Ratings(sim);
  std::map<int, float> mix;  // entity -> count
  for (size_t i = kEnemy; i < ix.forces.size(); i += kSides)
    for (const auto& [entity, count] : ix.forces[i]) {
      const UnitRating& r = rs.byEntity[entity];
      if (!all && (r.move == kMoveStatic || r.move == kMoveNaval)) continue;
      if (!all && (r.kinds & (kKindEngineer | kKindCommander))) continue;
      mix[entity] += count;
    }
  lua_newtable(L);
  int n = 0;
  for (const auto& [entity, count] : mix) {
    const UnitRating& r = rs.byEntity[entity];
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushstring(L, r.id.c_str());
    lua_rawset(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, count);
    lua_rawset(L, -3);
    lua_pushstring(L, "mass");
    lua_pushnumber(L, r.mass * count);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// Remastered.AIZoneEstimate(army, zone, horizon [, kite]) -> margin, oursLeft, theirsLeft, seconds, oursMass,
// theirsMass: the battle estimate of a fight for the zone from what the army knows (phase 4): its index's units (fair
// intel, the memory of lost contacts, unknown contacts left out) that can come to the zone within `horizon` world
// units of travel or shoot into it, own + allied against enemy; a unit away from the zone joins after its travel
// time (its class's travel distance / its speed; aircraft in a straight line), in 10 s buckets. The margin is ours.
int l_AIZoneEstimate(lua_State* L) {
  Sim& sim = *Sim::From(L);
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  const MapAnalysis& m = MapOf(sim);
  const int z = CheckZoneArg(L, 2, m);
  const float horizon = static_cast<float>(luaL_checknumber(L, 3));
  const bool kite = lua_toboolean(L, 4) != 0;
  RatingsState& rs = Ratings(sim);
  const Zone& zt = m.zones[z - 1];
  const float radius = std::sqrt(static_cast<float>(zt.segments) * MapAnalysis::kSeg * MapAnalysis::kSeg / 3.14159265f);
  std::map<std::pair<int, int>, float> stacks[2];  // (entity, arrival bucket) -> count; [0] ours, [1] theirs
  float mass[2] = {0, 0};
  for (int zs = 1; zs <= static_cast<int>(m.zones.size()); ++zs) {
    const Zone& from = m.zones[zs - 1];
    const float dx = from.x - zt.x, dz = from.z - zt.z;
    const float straight = std::sqrt(dx * dx + dz * dz);
    for (int side = 0; side < kSides; ++side)
      for (const auto& [entity, count] : ix.forces[static_cast<size_t>(zs - 1) * kSides + side]) {
        const UnitRating& r = rs.byEntity[entity];
        float arrive = -1;
        if (zs == z || r.maxRange + radius >= straight) arrive = 0;
        else if (r.move == kMoveAir) arrive = straight <= horizon && r.speed > 0 ? straight / r.speed : -1;
        else if (r.move != kMoveStatic && r.speed > 0) {
          const float d = m.Distance(zs, z, r.move);
          arrive = d >= 0 && d <= horizon ? d / r.speed : -1;
        }
        if (arrive < 0) continue;
        const int bucket = static_cast<int>(arrive / 10);
        const int k = side == kEnemy ? 1 : 0;
        stacks[k][{entity, bucket}] += count;
        mass[k] += r.mass * count;
      }
  }
  std::vector<ForceStack> force[2];
  for (int k = 0; k < 2; ++k)
    for (const auto& [key, count] : stacks[k]) force[k].push_back({&rs.byEntity[key.first], count, key.second * 10.0f});
  const BattleResult r = EstimateBattle(force[0], force[1], -1.0f, rs.beats, kite);
  lua_pushnumber(L, r.margin);
  lua_pushnumber(L, r.ours);
  lua_pushnumber(L, r.theirs);
  lua_pushnumber(L, r.seconds);
  lua_pushnumber(L, mass[0]);
  lua_pushnumber(L, mass[1]);
  return 6;
}

// Remastered.AIBuildValue(blueprintId, enemy) -> value, killed, lost: the counter-pick (BuildValue): enemy mass killed
// per own mass lost when as much of this unit as the enemy force weighs fights it; enemy = an array of {id, count}
int l_AIBuildValue(lua_State* L) {
  Sim& sim = *Sim::From(L);
  const BlueprintInfo* bp = sim.blueprints().Find(luaL_checkstring(L, 1));
  const UnitRating* r = bp && bp->kind == BpKind::Unit ? RatingOf(sim, bp->entityIndex) : nullptr;
  if (!r) luaL_argerror(L, 1, "not a unit blueprint id");
  const std::vector<ForceStack> enemy = CheckForce(L, 2);
  float killed = 0, lost = 0;
  const float v = BuildValue(*r, enemy, Ratings(sim).beats, &killed, &lost);
  lua_pushnumber(L, v);
  lua_pushnumber(L, killed);
  lua_pushnumber(L, lost);
  return 3;
}

// Remastered.AIRandom(army [, n]) -> the army's own random stream (phase 3): a float in [0, 1), or with n an integer
// 1..n. One MT19937 per army that plays our AI, seeded from the session seed and the army index, never the sim's own
// stream (which the game's scripts and the original's replays draw from): the same draws on every machine.
int l_AIRandom(lua_State* L) {
  Sim& sim = *Sim::From(L);
  const int army = static_cast<int>(luaL_checknumber(L, 1));
  IndexState* st = IndexOf(sim);
  ArmyIndex* ix = nullptr;
  if (st)
    for (ArmyIndex& x : st->armies)
      if (x.army0 == army - 1) ix = &x;
  if (!ix) luaL_argerror(L, 1, "not an army that plays our AI");
  if (!ix->rngSeeded) {
    ix->rng.seed(sim.sessionSeed() ^ (0x9E3779B9u * static_cast<uint32_t>(army)));
    ix->rngSeeded = true;
  }
  const uint32_t u = ix->rng();
  if (lua_isnoneornil(L, 2)) {
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<double>(u >> 8) * (1.0 / 16777216.0)));  // 24 bits: a float in [0, 1)
  } else {
    const uint32_t n = static_cast<uint32_t>(std::max(1.0f, static_cast<float>(luaL_checknumber(L, 2))));
    lua_pushnumber(L, static_cast<lua_Number>(1 + static_cast<uint32_t>((static_cast<uint64_t>(u) * n) >> 32)));
  }
  return 1;
}

// Remastered.AISetRatingFix(blueprintId, multiplier): Chris's correction of a unit's rating (sets, does not multiply:
// calling it twice with the same number changes nothing)
int l_AISetRatingFix(lua_State* L) {
  Sim& sim = *Sim::From(L);
  const BlueprintInfo* bp = sim.blueprints().Find(luaL_checkstring(L, 1));
  const float m = static_cast<float>(luaL_checknumber(L, 2));
  RatingsState& rs = Ratings(sim);
  if (!bp || bp->kind != BpKind::Unit || bp->entityIndex < 0 || bp->entityIndex >= static_cast<int>(rs.byEntity.size()) ||
      !rs.byEntity[bp->entityIndex].valid)
    luaL_argerror(L, 1, "not a unit blueprint id");
  if (!(m > 0)) luaL_argerror(L, 2, "a multiplier above 0");
  rs.byEntity[bp->entityIndex].fix = m;
  return 0;
}

// Remastered.AISetBeats(idA, idB, factor): Chris's "A beats B": A's dps against B in the battle estimate x factor
int l_AISetBeats(lua_State* L) {
  Sim& sim = *Sim::From(L);
  const BlueprintInfo* a = sim.blueprints().Find(luaL_checkstring(L, 1));
  const BlueprintInfo* b = sim.blueprints().Find(luaL_checkstring(L, 2));
  const float f = static_cast<float>(luaL_checknumber(L, 3));
  if (!a || a->kind != BpKind::Unit) luaL_argerror(L, 1, "not a unit blueprint id");
  if (!b || b->kind != BpKind::Unit) luaL_argerror(L, 2, "not a unit blueprint id");
  if (!(f > 0)) luaL_argerror(L, 3, "a factor above 0");
  Ratings(sim).beats[{a->entityIndex, b->entityIndex}] = f;
  return 0;
}

// Remastered.AISetVision(army, on): the "AI vision" cheat option for an army that plays our AI (its index then knows
// every enemy unit where it is). Its brain sets it from the lobby option, the same on every machine.
int l_AISetVision(lua_State* L) {
  ArmyIndex& ix = CheckArmyIndex(L, 1);
  ix.vision = lua_toboolean(L, 2) != 0;
  ix.builtTick = 0xffffffffu;  // (built again at the next query)
  return 0;
}

// Remastered.AIBattleEstimate(ours, theirs [, startDistance]) -> margin, oursLeft, theirsLeft, seconds: the battle
// estimate (unit_index.h EstimateBattle); ours / theirs = arrays of {blueprintId, count}; startDistance < 0 or none:
// the longer side's range
int l_AIBattleEstimate(lua_State* L) {
  const std::vector<ForceStack> a = CheckForce(L, 1), b = CheckForce(L, 2);
  const float d = lua_isnoneornil(L, 3) ? -1.0f : static_cast<float>(luaL_checknumber(L, 3));
  const BattleResult r = EstimateBattle(a, b, d, Ratings(*Sim::From(L)).beats, lua_toboolean(L, 4) != 0);
  lua_pushnumber(L, r.margin);
  lua_pushnumber(L, r.ours);
  lua_pushnumber(L, r.theirs);
  lua_pushnumber(L, r.seconds);
  return 4;
}

}  // namespace

void RegisterIndexBindings(lua_State* L) {
  lua_pushstring(L, "Remastered");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFns[] = {{"AIUnitRating", l_AIUnitRating},
              {"AIZoneForces", l_AIZoneForces},
              {"AIZoneThreat", l_AIZoneThreat},
              {"AIIndexInfo", l_AIIndexInfo},
              {"AISquareLaw", l_AISquareLaw},
              {"AIBattleEstimate", l_AIBattleEstimate},
              {"AIRandom", l_AIRandom},
              {"AIBuildValue", l_AIBuildValue},
              {"AIZoneEstimate", l_AIZoneEstimate},
              {"AIEnemyMix", l_AIEnemyMix},
              {"AISetRatingFix", l_AISetRatingFix},
              {"AISetBeats", l_AISetBeats},
              {"AISetVision", l_AISetVision}};
  for (const auto& fn : kFns) {
    lua_pushstring(L, fn.name);
    lua_pushcfunction(L, fn.f);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}

}  // namespace ai
}  // namespace moho
