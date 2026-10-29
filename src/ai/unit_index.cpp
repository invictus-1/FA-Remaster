// FA Remastered's own AI, native core, phase 2: unit ratings and the unit / threat index (see unit_index.h).
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
#include <memory>

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
      const float salvo = std::max(1.0f, RawNum(L, w, "MuzzleSalvoSize", 1));
      const float together = RawBool(L, w, "RackFireTogether") ? static_cast<float>(std::max(1, racks)) : 1.0f;
      const float perFire = std::max(1.0f, RawNum(L, w, "ProjectilesPerOnFire", 1));
      dps = damage * dot * salvo * together * perFire * rof;
    }
    const float range = RawNum(L, w, "MaxRadius", 0);
    for (int t = 0; t < kTargets; ++t)
      if (vs & (1u << t)) {
        r->dps[t] += dps;
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

struct RatingsState {
  bool made = false;
  std::vector<UnitRating> byEntity;  // by entity index (invalid for non-units)
  float ref = std::sqrt(kRefDps * kRefHealth);
  bool refFromRules = false;
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
    bps.PushTable(L, *bp);
    int t = tier[i];
    RateBlueprint(L, lua_gettop(L), kinds[i], "", &r);
    lua_settop(L, top);
    r.faction = faction[i];
    if (notPlayable[i] || !*faction[i]) r.stock = false;  // (the stock list: the four factions' playable units)
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
      r.best = std::max(r.best, r.value[t]);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%c%s %.1f", r.tier.c_str(), r.domain, r.tierGuessed ? "*" : "", r.best);
    r.label = buf;
  }
  return st;
}

// ---- the index ----

enum Side : int { kOwn = 0, kAlly = 1, kEnemy = 2, kSides = 3 };

struct Cell {  // per zone, side, movement class
  float count = 0, mass = 0, unknown = 0;
  float str[kTargets] = {};
  float band[kBands] = {};  // best rating by the band of the unit's longest range
  float maxRange = 0;
};
struct ZoneSideExtra {
  float engineers = 0, commanders = 0, experimentals = 0, factories = 0, shieldHealth = 0, economyMass = 0;
};

struct ArmyIndex {
  int army0 = -1;
  uint32_t builtTick = 0xffffffffu;
  int units[kSides] = {}, unknown = 0, outside = 0;
  std::vector<Cell> cells;          // [zone][side][class]
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
    if (side == kEnemy) {
      ++enemiesAll;
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
      const int z = m.ZoneAt(p.x, p.z, -1);
      if (!z) {
        ++ix.outside;
        continue;
      }
      ++ix.units[kEnemy];
      Cell& c = ix.At(z - 1, kEnemy, r->move);
      const float hf = u->maxHealth > 0 ? std::sqrt(std::clamp(u->health / u->maxHealth, 0.0f, 1.0f)) : 1.0f;
      c.count += 1;
      c.mass += r->mass;
      for (int t = 0; t < kTargets; ++t) c.str[t] += r->value[t] * hf;
      c.band[BandOf(r->maxRange)] += r->best * hf;
      c.maxRange = std::max(c.maxRange, r->maxRange);
      ZoneSideExtra& e = ix.Extra(z - 1, kEnemy);
      if (r->kinds & kKindEngineer) e.engineers += 1;
      if (r->kinds & kKindCommander) e.commanders += 1;
      if (r->kinds & kKindExperimental) e.experimentals += 1;
      if (r->kinds & kKindFactory) e.factories += 1;
      if (r->kinds & kKindShield) e.shieldHealth += r->shield;
      if (r->kinds & kKindEconomy) e.economyMass += r->mass;
      continue;
    }
    if (u->fractionComplete < 1) continue;
    const int z = m.ZoneAt(u->position.x, u->position.z, -1);
    if (!z) {
      ++ix.outside;
      continue;
    }
    ++ix.units[side];
    Cell& c = ix.At(z - 1, side, r->move);
    const float hf = u->maxHealth > 0 ? std::sqrt(std::clamp(u->health / u->maxHealth, 0.0f, 1.0f)) : 1.0f;
    c.count += 1;
    c.mass += r->mass;
    for (int t = 0; t < kTargets; ++t) c.str[t] += r->value[t] * hf;
    c.band[BandOf(r->maxRange)] += r->best * hf;
    c.maxRange = std::max(c.maxRange, r->maxRange);
    ZoneSideExtra& e = ix.Extra(z - 1, side);
    if (r->kinds & kKindEngineer) e.engineers += 1;
    if (r->kinds & kKindCommander) e.commanders += 1;
    if (r->kinds & kKindExperimental) e.experimentals += 1;
    if (r->kinds & kKindFactory) e.factories += 1;
    if (r->kinds & kKindShield) e.shieldHealth += r->shield;
    if (r->kinds & kKindEconomy) e.economyMass += r->mass;
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

// One list: the header, then a row per unit in `v`'s order.
bool WriteRatingList(const RatingsState& st, const std::string& path, const std::vector<const UnitRating*>& v,
                     const char* what) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f, "# FA Remastered AI: unit ratings from the blueprints (src/ai/unit_index.h): %s, %zu units\n", what,
               v.size());
  std::fprintf(f, "# rating = sqrt(dps x (health + shield)) / the reference's (uel0201: %.1f%s); label = tier + domain + best rating\n",
               st.ref, st.refFromRules ? "" : ", not in the rules: built-in 24 dps x 300 hp");
  std::fprintf(f, "# tier T1 T2 T3, T4 experimental, C commander; domain G ground, A air, N naval, S structure, X experimental\n");
  std::fprintf(f, "# a '*' after the domain: the tier was guessed from the mass (its categories gave none, or one two tiers off;\n");
  std::fprintf(f, "#   the last column says what they gave)\n");
  std::fprintf(f, "# fix = Chris's multiplier (1.0 until corrected)\n");
  std::fprintf(f, "%-11s %-11s %-8s %-30s %7s %7s %6s | %8s %7s %8s | %5s %5s %5s | %5s | %7s %6s %7s %7s | %4s | %s\n",
               "label", "id", "faction", "name", "mass", "health", "shield", "dpsG", "dpsA", "dpsN", "rngG", "rngA", "rngN",
               "speed", "vsG", "vsA", "vsN", "vsS", "fix", "tier note");
  for (const UnitRating* r : v)
    std::fprintf(f, "%-11s %-11s %-8s %-30.30s %7.0f %7.0f %6.0f | %8.1f %7.1f %8.1f | %5.0f %5.0f %5.0f | %5.1f | %7.2f %6.2f %7.2f %7.2f | 1.0  | %s\n",
                 r->label.c_str(), r->id.c_str(), r->faction.empty() ? "-" : r->faction.c_str(), r->name.c_str(), r->mass,
                 r->health, r->shield, r->dps[kVsGround], r->dps[kVsAir], r->dps[kVsNaval], r->range[kVsGround],
                 r->range[kVsAir], r->range[kVsNaval], r->speed, r->value[kVsGround], r->value[kVsAir],
                 r->value[kVsNaval], r->value[kVsStructure], r->tierGuessed ? r->tierNote.c_str() : "");
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
    if (p->best != q->best) return p->best > q->best;
    return p->id < q->id;
  });
  std::stable_sort(stock.begin(), stock.end(), [](const UnitRating* p, const UnitRating* q) {
    if (FactionOrder(p->faction) != FactionOrder(q->faction)) return FactionOrder(p->faction) < FactionOrder(q->faction);
    if (TierOrder(p->tier) != TierOrder(q->tier)) return TierOrder(p->tier) < TierOrder(q->tier);
    if (DomainOrder(p->domain) != DomainOrder(q->domain)) return DomainOrder(p->domain) < DomainOrder(q->domain);
    if (p->best != q->best) return p->best > q->best;
    return p->id < q->id;
  });
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  const std::string stockPath = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                                    ? path.substr(0, dot) + "_stock" + path.substr(dot)
                                    : path + "_stock";
  return WriteRatingList(st, path, all, "every unit, mods included") &&
         WriteRatingList(st, stockPath, stock, "the game's own units (the four factions; no mod), by faction");
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
  SetNum(L, "best", r.best);
  SetNum(L, "maxRange", r.maxRange);
  SetNum(L, "kinds", r.kinds);
  for (const char* what : {"dps", "range", "rating"}) {
    lua_pushstring(L, what);
    lua_newtable(L);
    const float* src = !std::strcmp(what, "dps") ? r.dps : !std::strcmp(what, "range") ? r.range : r.value;
    for (int t = 0; t < kTargets; ++t) SetNum(L, kTargetNames[t], src[t]);
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
        sum.maxRange = std::max(sum.maxRange, c.maxRange);
        for (int t = 0; t < kTargets; ++t) sum.str[t] += c.str[t];
      }
      lua_pushstring(L, d.name);
      lua_newtable(L);
      SetNum(L, "count", sum.count);
      SetNum(L, "mass", sum.mass);
      SetNum(L, "unknown", sum.unknown);
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
              {"AISquareLaw", l_AISquareLaw}};
  for (const auto& fn : kFns) {
    lua_pushstring(L, fn.name);
    lua_pushcfunction(L, fn.f);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}

}  // namespace ai
}  // namespace moho
