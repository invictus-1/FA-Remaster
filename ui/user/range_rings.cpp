// Range rings: the circles that show how far a unit's weapons and its radar, sonar and other
// intel reach.
//
// As read from the original (the range renderer, FA exe 0x7ec3a0 .. 0x7ef5a0):
// - The game's script names its filters with SetOverlayFilter(name, categories, normalColor,
//   selectColor, rolloverColor, innerNear, innerFar, outerNear, outerFar) (0x846b60; the list is
//   /lua/ui/game/rangeoverlayparams.lua) and says which are switched on for every unit with
//   SetOverlayFilters(names) (0x846f70).
// - What a filter's ring measures goes by its NAME (a fixed table, 0x7ed4b0): "DirectFire",
//   "IndirectFire", "AntiAir", "AntiNavy" are the unit's weapons of that RangeCategory: the outer
//   ring the greatest of their radii (EffectiveRadius where the blueprint has one, else
//   MaxRadius), the inner ring the least MinRadius (0x7ec650). "Radar", "Sonar", "Omni" are the
//   blueprint's Intel radii (0x7ed000, 0x7ed150, 0x7eceb0); "AllIntel" the greatest of omni,
//   radar, sonar and the radar stealth field (0x7ecce0); "CounterIntel" of the jamming, spoofing
//   and radar stealth field radii (0x7ed270).
// - Which units get rings: every unit in a switched-on filter's categories, in the filter's
//   normal colour; and by three console variables the scripts set (true unless the player's
//   profile says otherwise): range_RenderSelected (0x7ef280: the player's selected units, for
//   every named filter but "AllMilitary" and "AllIntel", in the select colour),
//   range_RenderHighlighted (0x7ef420: the same for the player's unit under the mouse, rollover
//   colour) and range_RenderBuild (0x7eee50: the same for the building about to be placed, at
//   the mouse, normal colour).
//
// First form, OURS where it differs:
// - the rings go to the picture's source in groups, one for a filter in one colour, with the
//   filter's Inner / Outer numbers and the colour's first byte (the glow): the renderer draws a
//   group as one outline round what its rings cover together, as the original does (its
//   func_RenderRings 0x7ef5a0 is not read: how thickness and glow go with the zoom is the
//   renderer's guess);
// - the radii are the blueprint's: a radius a script has changed, or intel switched off, is not
//   followed (the original asks the live unit); a weapon that an enhancement adds is left out,
//   before and after the enhancement;
// - "Defense" (0x7ec980): half the blueprint's Defense.Shield.ShieldSize where there is one (the
//   float at +0x458, times 0.5: a shield's radius), else the weapons as below. "Miscellaneous"
//   (0x7ed430): AI.StagingPlatformScanRadius (+0x468) where it is above 0, else
//   AI.GuardScanRadius (+0x460). (The fields' names are the engine track's, from the blueprint's
//   layout: docs/engine-answers.md, "ui 15".) The game's own scripts make the Miscellaneous ring
//   the BUILD RANGE of engineers and commanders: /lua/system/blueprints-units.lua sets
//   StagingPlatformScanRadius = Economy.MaxBuildDistance + the footprint's size + 1 for them and
//   adds the category OVERLAYMISC, and the game's filter list gives that ring its colour.
// - "Defense" without a shield is taken as the weapons of RangeCategory UWRC_Countermeasure and
//   "AllMilitary" as every weapon with a range category (the original's weapon test for these
//   two is not read to the end);
// - switched-on filters show the player's own units only (whose units the original shows is
//   not read);
// - of rings that lie on one another the filter that comes first in the original's table of
//   names (0x7ed4b0: AllMilitary, DirectFire, IndirectFire, AntiAir, AntiNavy, Defense,
//   Miscellaneous, AllIntel, Radar, Sonar, Omni, CounterIntel) is the one seen, so the groups go
//   to the picture last name first. INFERRED from a picture of the original, not read: a
//   commander whose direct fire, indirect fire and anti-air weapons all reach 22 shows that
//   ring in the direct fire colour.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "maui/world_source.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/sim.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

struct RangeFilter {
  std::vector<uint64_t> categories;
  // As the script gives them: the first byte is the glow amount, then red, green, blue.
  uint32_t normal = 0x00ffffffu, select = 0x00ffffffu, rollover = 0x00ffffffu;
  float inner[2] = {0, 0}, outer[2] = {0, 0};  // line thickness at the nearest and the farthest zoom
};

struct RangeStore : UserState::Extra {
  std::map<std::string, RangeFilter> filters;
  std::vector<std::string> active;
  bool selected = false, highlighted = false, build = false;
  std::map<std::string, std::pair<float, float>> radii;  // "blueprint|filter" -> inner, outer
};

RangeStore& Store(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->ranges) user->ranges = std::make_unique<RangeStore>();
  return static_cast<RangeStore&>(*user->ranges);
}

// "ggrrggbb" or "rrggbb" -> glow amount (first byte), red, green, blue.
uint32_t ColorOf(lua_State* L, int idx) {
  const char* s = lua_tostring(L, idx);
  if (!s) return 0x00ffffffu;
  return static_cast<uint32_t>(std::strtoul(s, nullptr, 16));
}

// t[name] as a number (0 when it is none); the table is at stack index t.
float Field(lua_State* L, int t, const char* name) {
  lua_pushstring(L, name);
  lua_rawget(L, t);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0;
  lua_pop(L, 1);
  return v;
}

// What a filter's ring measures on a blueprint: false when it has no such ring.
bool Measure(lua_State* L, const std::string& blueprint, const std::string& filter, float* inner, float* outer) {
  static const std::map<std::string, const char*> kWeaponClass = {
      {"DirectFire", "UWRC_DirectFire"}, {"IndirectFire", "UWRC_IndirectFire"}, {"AntiAir", "UWRC_AntiAir"},
      {"AntiNavy", "UWRC_AntiNavy"},     {"Defense", "UWRC_Countermeasure"},    {"AllMilitary", ""}};
  *inner = *outer = 0;
  int top = lua_gettop(L);
  lua_getglobal(L, "__blueprints");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, blueprint.c_str());
    lua_rawget(L, -2);
  }
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return false;
  }
  int bp = lua_gettop(L);
  auto weapon = kWeaponClass.find(filter);
  if (weapon != kWeaponClass.end()) {
    bool any = false;
    lua_pushstring(L, "Weapon");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
      int list = lua_gettop(L);
      for (int i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        int w = lua_gettop(L);
        lua_pushstring(L, "RangeCategory");
        lua_rawget(L, w);
        std::string category = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        bool counts = *weapon->second ? category == weapon->second : !category.empty() && category != "UWRC_Undefined";
        // A weapon that only an enhancement adds is not one the unit has (the original asks the
        // live unit for its weapons; here such weapons are left out, after the enhancement too).
        lua_pushstring(L, "EnabledByEnhancement");
        lua_rawget(L, w);
        if (lua_isstring(L, -1) && *lua_tostring(L, -1)) counts = false;
        lua_pop(L, 1);
        if (counts) {
          float reach = Field(L, w, "EffectiveRadius");
          if (!(reach > 0)) reach = Field(L, w, "MaxRadius");
          float least = Field(L, w, "MinRadius");
          *outer = std::max(*outer, reach);
          *inner = any ? std::min(*inner, least) : least;
          any = true;
        }
        lua_pop(L, 1);
      }
    }
    if (filter == "Defense") {  // a shield's radius goes before the weapons (0x7ec980)
      float shield = 0;
      lua_pushstring(L, "Defense");
      lua_rawget(L, bp);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "Shield");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) shield = Field(L, lua_gettop(L), "ShieldSize") * 0.5f;
      }
      if (shield > 0) *inner = 0, *outer = shield;
    }
  } else if (filter == "Miscellaneous") {  // (0x7ed430; the build range, by the game's scripts)
    lua_pushstring(L, "AI");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
      int ai = lua_gettop(L);
      *outer = Field(L, ai, "StagingPlatformScanRadius");
      if (!(*outer > 0)) *outer = Field(L, ai, "GuardScanRadius");
    }
  } else {
    lua_pushstring(L, "Intel");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
      int intel = lua_gettop(L);
      auto pair = [&](const char* name) {  // JamRadius / SpoofRadius = {Min, Max}
        lua_pushstring(L, name);
        lua_rawget(L, intel);
        float v = lua_istable(L, -1) ? Field(L, lua_gettop(L), "Max") : 0;
        lua_pop(L, 1);
        return v;
      };
      if (filter == "Radar") *outer = Field(L, intel, "RadarRadius");
      else if (filter == "Sonar") *outer = Field(L, intel, "SonarRadius");
      else if (filter == "Omni") *outer = Field(L, intel, "OmniRadius");
      else if (filter == "AllIntel")
        *outer = std::max({Field(L, intel, "OmniRadius"), Field(L, intel, "RadarRadius"), Field(L, intel, "SonarRadius"),
                           Field(L, intel, "RadarStealthFieldRadius")});
      else if (filter == "CounterIntel")
        *outer = std::max({pair("JamRadius"), pair("SpoofRadius"), Field(L, intel, "RadarStealthFieldRadius")});
    }
  }
  lua_settop(L, top);
  return *outer > 0;
}

bool RadiiOf(lua_State* L, RangeStore& store, const std::string& blueprint, const std::string& filter, float* inner, float* outer) {
  std::string key = blueprint + "|" + filter;
  auto it = store.radii.find(key);
  if (it == store.radii.end()) {
    float a = 0, b = 0;
    Measure(L, blueprint, filter, &a, &b);
    it = store.radii.emplace(key, std::make_pair(a, b)).first;
  }
  *inner = it->second.first, *outer = it->second.second;
  return *outer > 0;
}

bool InFilter(lua_State* L, const RangeFilter& f, const std::string& blueprint) {
  SimBlueprints* bps = SimBlueprints::From(L);
  const BlueprintInfo* bp = bps ? bps->Find(blueprint) : nullptr;
  return bp && bp->entityIndex >= 0 && static_cast<size_t>(bp->entityIndex >> 6) < f.categories.size() &&
         CategoryHas(f.categories.data(), bp->entityIndex);
}

// Where a filter stands in the original's table of names (0x7ed4b0).
int RankOf(const std::string& name) {
  static const char* const kOrder[] = {"AllMilitary", "DirectFire", "IndirectFire", "AntiAir", "AntiNavy", "Defense",
                                       "Miscellaneous", "AllIntel", "Radar", "Sonar", "Omni", "CounterIntel"};
  int n = static_cast<int>(sizeof kOrder / sizeof *kOrder);
  for (int i = 0; i < n; ++i)
    if (name == kOrder[i]) return i;
  return n;
}

// A ring into the group of its filter and colour (made when it is the first).
void AddRing(std::vector<WorldRangeGroup>* out, std::map<std::pair<std::string, uint32_t>, size_t>* groups,
             const std::string& name, const RangeFilter& f, uint32_t color, const float at[3], float inner, float outer) {
  auto slot = groups->find({name, color});
  if (slot == groups->end()) {
    WorldRangeGroup g;
    g.color = 0xff000000u | (color & 0x00ffffffu);
    g.glow = static_cast<int>(color >> 24);
    g.outer[0] = f.outer[0], g.outer[1] = f.outer[1];
    g.inner[0] = f.inner[0], g.inner[1] = f.inner[1];
    slot = groups->emplace(std::make_pair(name, color), out->size()).first;
    out->push_back(std::move(g));
  }
  WorldRangeRing ring;
  for (int i = 0; i < 3; ++i) ring.centre[i] = at[i];
  ring.radius = outer;
  ring.innerRadius = inner > 0 ? inner : 0;
  (*out)[slot->second].rings.push_back(ring);
}

// SetOverlayFilter(name, categories, normalColor, selectColor, rolloverColor, innerNear, innerFar,
// outerNear, outerFar)
int l_SetOverlayFilter(lua_State* L) {
  RangeFilter f;
  if (const uint64_t* bits = ToCategory(L, 2)) f.categories.assign(bits, bits + CategoryWordCount());
  f.normal = ColorOf(L, 3), f.select = ColorOf(L, 4), f.rollover = ColorOf(L, 5);
  f.inner[0] = static_cast<float>(lua_tonumber(L, 6)), f.inner[1] = static_cast<float>(lua_tonumber(L, 7));
  f.outer[0] = static_cast<float>(lua_tonumber(L, 8)), f.outer[1] = static_cast<float>(lua_tonumber(L, 9));
  Store(L).filters[luaL_checkstring(L, 1)] = std::move(f);
  return 0;
}
// SetOverlayFilters(names): the filters switched on for every unit
int l_SetOverlayFilters(lua_State* L) {
  RangeStore& store = Store(L);
  store.active.clear();
  if (lua_istable(L, 1)) {
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      if (lua_isstring(L, -1)) store.active.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  return 0;
}
// moho64ui_RangeRings() -> the rings as they would be drawn now, each {x, z, radius, inner,
// color ("rrggbb"), glow, group (its group's number)}: the host's own function for tests.
int l_RangeRings(lua_State* L) {
  std::vector<WorldRangeGroup> groups;
  RangeRingsForWorld(L, UnitUnderMouse(L), nullptr, &groups);
  lua_newtable(L);
  int n = 0;
  for (size_t g = 0; g < groups.size(); ++g) {
    for (const WorldRangeRing& r : groups[g].rings) {
      lua_newtable(L);
      auto number = [&](const char* k, double d) {
        lua_pushstring(L, k);
        lua_pushnumber(L, d);
        lua_rawset(L, -3);
      };
      number("x", r.centre[0]), number("z", r.centre[2]), number("radius", r.radius), number("inner", r.innerRadius);
      number("glow", groups[g].glow), number("group", static_cast<double>(g + 1));
      char color[16];
      std::snprintf(color, sizeof color, "%06x", groups[g].color & 0x00ffffffu);
      lua_pushstring(L, "color");
      lua_pushstring(L, color);
      lua_rawset(L, -3);
      lua_rawseti(L, -2, ++n);
    }
  }
  return 1;
}

}  // namespace

bool RangeConsoleCommand(lua_State* L, const std::string& upperName, const std::string& value) {
  RangeStore& store = Store(L);
  bool* flag = upperName == "RANGE_RENDERSELECTED" ? &store.selected
               : upperName == "RANGE_RENDERHIGHLIGHTED" ? &store.highlighted
               : upperName == "RANGE_RENDERBUILD" ? &store.build : nullptr;
  if (!flag) return false;
  std::string v;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  if (v.empty()) *flag = !*flag;  // a console variable without a value turns over
  else *flag = v == "true" || v == "1" || v == "on";
  return true;
}

bool RangeConsoleValue(lua_State* L, const std::string& upperName, bool* value) {
  const RangeStore& store = Store(L);
  const bool* flag = upperName == "RANGE_RENDERSELECTED" ? &store.selected
                     : upperName == "RANGE_RENDERHIGHLIGHTED" ? &store.highlighted
                     : upperName == "RANGE_RENDERBUILD" ? &store.build : nullptr;
  if (!flag) return false;
  *value = *flag;
  return true;
}

void RangeRingsForWorld(lua_State* L, uint32_t hover, const WorldGhost* build, std::vector<WorldRangeGroup>* out) {
  UserState* user = UserState::From(L);
  Sim* sim = user->LiveSim("range rings");
  if (!sim || !user->ranges) return;
  RangeStore& store = static_cast<RangeStore&>(*user->ranges);
  if (store.filters.empty()) return;
  const int own = sim->focusArmy + 1;
  using Entry = std::pair<const std::string, RangeFilter>;  // (a filter as store.filters keeps it: its address stays)
  // Per blueprint, the filters it is in and their rings' radii, worked out once a call: a
  // selection may hold thousands of one kind (2,000 tanks: one blueprint, not 24,000 tests).
  struct Kind {
    std::vector<const Entry*> filters;
    std::map<const Entry*, std::pair<float, float>> radii;  // (inner, outer; outer 0: no ring)
  };
  std::map<std::string, Kind> kinds;
  auto kindOf = [&](const std::string& bp) -> Kind& {
    auto it = kinds.find(bp);
    if (it != kinds.end()) return it->second;
    Kind& k = kinds[bp];
    for (const Entry& entry : store.filters)
      if (InFilter(L, entry.second, bp)) k.filters.push_back(&entry);
    return k;
  };
  auto has = [](const Kind& k, const Entry* e) { return std::find(k.filters.begin(), k.filters.end(), e) != k.filters.end(); };
  // A unit's ring of a filter is drawn once: under the mouse before selected before switched on.
  std::map<std::pair<uint32_t, const Entry*>, uint32_t> colors;
  std::string blueprint;
  float at[3];
  int army = 0;
  if (!store.active.empty()) {
    std::vector<uint32_t> all;
    UserUnitIds(L, false, &all);
    for (const std::string& name : store.active) {
      auto f = store.filters.find(name);
      if (f == store.filters.end()) continue;
      for (uint32_t id : all)
        if (UserUnitFacts(L, id, &blueprint, at, &army) && army == own && has(kindOf(blueprint), &*f))
          colors[{id, &*f}] = f->second.normal;
    }
  }
  auto named = [&](uint32_t id, bool rollover) {
    if (!UserUnitFacts(L, id, &blueprint, at, &army) || army != own) return;
    for (const Entry* e : kindOf(blueprint).filters)
      if (e->first != "AllMilitary" && e->first != "AllIntel") colors[{id, e}] = rollover ? e->second.rollover : e->second.select;
  };
  if (store.selected) {
    std::vector<uint32_t> selected;
    UserUnitIds(L, true, &selected);
    for (uint32_t id : selected) named(id, false);
  }
  if (store.highlighted && hover != 0xffffffffu) named(hover, true);
  // The picture draws the groups in the order given, a later one over an earlier: the filter
  // that stands first in the original's table goes last (see the head of this file).
  struct Found {
    const std::string* name;
    const RangeFilter* filter;
    uint32_t color;
    float at[3], inner, outer;
  };
  std::vector<Found> found;
  float inner = 0, outer = 0;
  for (const auto& [key, color] : colors) {
    if (!UserUnitFacts(L, key.first, &blueprint, at, &army)) continue;
    Kind& k = kindOf(blueprint);
    auto r = k.radii.find(key.second);
    if (r == k.radii.end()) {
      if (!RadiiOf(L, store, blueprint, key.second->first, &inner, &outer)) outer = 0;
      r = k.radii.emplace(key.second, std::make_pair(inner, outer)).first;
    }
    if (r->second.second > 0)
      found.push_back({&key.second->first, &key.second->second, color, {at[0], at[1], at[2]}, r->second.first, r->second.second});
  }
  if (store.build && build)
    for (const auto& [name, f] : store.filters)
      if (name != "AllMilitary" && name != "AllIntel" && InFilter(L, f, build->blueprint) &&
          RadiiOf(L, store, build->blueprint, name, &inner, &outer))
        found.push_back({&name, &f, f.normal, {build->position[0], build->position[1], build->position[2]}, inner, outer});
  std::stable_sort(found.begin(), found.end(),
                   [](const Found& a, const Found& b) { return RankOf(*a.name) > RankOf(*b.name); });
  std::map<std::pair<std::string, uint32_t>, size_t> groups;
  for (const Found& r : found) AddRing(out, &groups, *r.name, *r.filter, r.color, r.at, r.inner, r.outer);
}

void RegisterRangeBindings(lua_State* L) {
  if (!UserState::From(L)->HasSim()) return;
  SetGlobal(L, "SetOverlayFilter", l_SetOverlayFilter);
  SetGlobal(L, "SetOverlayFilters", l_SetOverlayFilters);
  lua_register(L, "moho64ui_RangeRings", l_RangeRings);
}

}  // namespace moho::ui
