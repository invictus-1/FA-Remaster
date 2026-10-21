// Range rings: the circles on the ground that show how far a unit's weapons and its radar, sonar and
// other intel reach: the game's overlays (the military and intel filters of FAF's multifunction panel),
// and the rings of the selected units, of the unit under the mouse and of the building about to be
// placed.
//
// As read from the original (FAF's patched exe; the range renderer 0x7ec3a0 .. 0x7ef9b0, and FAF's
// replacements of two of its parts in its own section, 0x128c9e8 .. 0x1291428):
// - SetOverlayFilter(name, categories, normalColor, selectColor, rolloverColor, innerNear, innerFar,
//   outerNear, outerFar) (0x846be0 -> 0x7ee5a0; the list is /lua/ui/game/rangeoverlayparams.lua) keeps a
//   filter in a std::map by its name (0x7efd00, std::string::compare 0x40a880: the names' byte order,
//   AllIntel, AllMilitary, AntiAir, AntiNavy, CounterIntel, Defense, DirectFire, IndirectFire,
//   Miscellaneous, Omni, Radar, Sonar); its colours decoded as the script gives them (SCR_DecodeColor),
//   the first byte the glow amount, which the drawing writes to the picture's alpha. SetOverlayFilters
//   (names) (0x846ff0) keeps the names, and every frame while ren_Ranges is on (RangeRenderer::
//   MoveCategories 0x7ee950, from WRenViewport::Render 0x7f91fe) the filters of those names are taken
//   from the map, in the order given, with what the map holds then.
// - The rings are drawn after the ground and before the meshes (FAF: WRenViewport::Render 0x7f944d calls
//   its 0x1291197 while ren_Ranges and ren_Ui are on; in the cartographic view Cartographic::Render
//   0x7d1a2c calls the original's 0x7eea00), in passes, each filter of a pass one draw of its own
//   (func_RenderRings 0x7ef5a0: all its rings as one outline; a later draw lies over an earlier one, its
//   colour replacing what was there: effects/frame.fx RangeBurn, SrcBlend one, DestBlend zero):
//   1. range_RenderBuild (0x10a6414): in a build mode with a blueprint (GetLeftMouseButtonAction 0x81f7b0
//      gives 2 or 3), at the cursor's place in the world (session+0x4b4), every filter of the map but
//      AllMilitary and AllIntel whose categories hold the blueprint, measured on the blueprint (the
//      extractors' Range below), normal colour (func_RenderBuildRings 0x7eee50);
//   2. the filters switched on, in their order, normal colour: the focus army's units in the camera's view
//      (CameraImpl::GetArmyUnitsInFrustum 0x7a7910, filled by CacheCameraFrustumUnits 0x7a75a0 with the
//      units of the focus army among what the view's frustum holds), not being built (UserUnit::
//      IsBeingBuilt) nor dead, and (FAF's 0x128c9e8) not those selected units that have a ring of
//      pass 3 (FAF keeps their list from pass 3 of the frame before: 0x128cd23);
//   3. range_RenderSelected (0x10a640a): every filter of the map but AllMilitary and AllIntel, the
//      selected units of the focus army in its categories, select colour (0x7ef280; FAF's 0x128cbbe);
//   4. range_RenderHighlighted (0x10a640b): the same for the unit under the mouse (the session's rollover
//      unit, session+0x4c0) when it is the focus army's, rollover colour (0x7ef420);
//   5. the focus army's no-rush circle (ExtractFocusArmyNoRushRange 0x7ef1c0: the user army's +0x1bc..
//      +0x1d0 while its no-rush ticks run, around its start plus the offsets at height 0, grey 0.2, inner
//      0.1 / 1, outer 1 / 2; the engine's ArmyView::start, noRushTicks, noRushRadius, noRushOffset, ui
//      REQUESTS 45).
//   Without a focus army (an observer) passes 2 to 5 have no units.
// - What a filter's ring measures goes by its NAME (sBlueprintExtractors 0x7ed4b0; a name not there has
//   no rings), of the unit as it is now (Extract, passes 2 to 4; the place the unit is drawn at this
//   frame, UserEntity::GetInterpolatedTransform) or of a blueprint at a place (Range, pass 1):
//   - DirectFire, IndirectFire, AntiAir, AntiNavy (WeaponExtractor, RangeCategory 1 to 4): Extract
//     0x7ec5b0 asks the unit (UserUnit slot 0x44, 0x8bfc50) for its weapons of that RangeCategory as the
//     simulation last synced them (the unit's weapon entries, UnitView::weapons): the greatest MaxRadius
//     and the least MinRadius (the inner ring), weapons an enhancement brings included, enabled or not;
//     Range 0x7ec650: every weapon of the blueprint of that category, EffectiveRadius where it is above 0,
//     else MaxRadius, and the least MinRadius;
//   - AllMilitary (CombinedMilitaryExtractor 0x7ec3a0, no Range): a unit in OVERLAYMISC has the ring of
//     Miscellaneous below when it has one; else every weapon (slot 0x44 with 6: any category);
//   - Defense (CountermeasureExtractor 0x7ec880 / 0x7ec980): half the blueprint's Defense.Shield.
//     ShieldSize where that is above 0 (bp+0x458), else the weapons of RangeCategory 5 (Countermeasure);
//   - Miscellaneous (0x7ed370 / 0x7ed430): the blueprint's AI.StagingPlatformScanRadius (bp+0x468) where
//     it is above 0, else AI.GuardScanRadius (bp+0x460). The game's scripts make it the BUILD RANGE of
//     engineers and commanders (/lua/system/blueprints-units.lua sets StagingPlatformScanRadius =
//     Economy.MaxBuildDistance + the footprint's size + 1 and adds OVERLAYMISC for them);
//   - Radar, Sonar, Omni (0x7ecf40, 0x7ed090, 0x7ecdf0; Range 0x7ed000, 0x7ed150, 0x7eceb0): the unit's
//     radii of that intel (slot 0x48, 0x8bfd70: its intel words, UserEntity+0x100, as the simulation last
//     synced them, which SetIntelRadius changes: UnitView::intel; none when the unit has the intel toggle,
//     RULEUTC_IntelToggle 8, and has it set, script bit 3); Range: the blueprint's Intel radii;
//   - AllIntel (IntelExtractor 0x7ecc00): the greatest of the unit's omni, radar and sonar radii (Range
//     0x7ecce0 adds RadarStealthFieldRadius; never asked, pass 1 leaves AllIntel out);
//   - CounterIntel (0x7ed1e0, slot 0x4c 0x8bfe50): the greatest of the unit's cloak field, sonar stealth
//     field and radar stealth field radii and the blueprint's JamRadius.Max and SpoofRadius.Max; none when
//     the unit has the jamming toggle (4) or the stealth toggle (0x20) and has it set; Range 0x7ed270: the
//     blueprint's JamRadius.Max, SpoofRadius.Max and RadarStealthFieldRadius, and its SonarStealth and
//     Cloak switches counted as 0 or 1.
// - The line's thickness (func_RenderRings 0x7ef5a0), in world units, for the outline (the filter's
//   Outer pair) and a hole's outline (Inner): near + (far x coeff x size - near) x zoom / maxZoom, where
//   coeff is range_OuterThicknessCoeff / range_InnerThicknessCoeff (0xf57ea8 / 0xf57ea4, 1/1024 at
//   first), size the longer side of the playable map (CWldTerrainRes::GetPlayableMapRect 0x89e710) and
//   zoom / maxZoom the camera's (CameraImpl slots 0x4c GetZoom and 0x50 GetMaxZoom). The outline lies
//   inside the ring (outer - thickness .. outer), a hole's outside the hole (inner .. inner + thickness:
//   sub_7EDC80). range_Fill (0x10a6415, off at first) also lays white at 1/8 over what the rings cover
//   (RangeFill): every group carries it to the renderer (RangeGroup::fill, render a8ac1f0).
//
// OURS where it differs, or not read:
// - INFERRED: the toggle caps tested are the unit's as the scripts left them (UnitView::toggleCaps; the
//   original reads them at +0x64 of what the unit's IUnit slot 0x44 gives).
// - "In the camera's view" is the picture's list of the units whose middle lies in the view (WorldSource::
//   UnitsIn; without a picture the camera's rough box, PlaceInCameraView); the original tests each
//   entity's box against the view's frustum. INFERRED: a unit's weapon entries pair with the blueprint's
//   weapons with DummyWeapon left out, in order (the original's index is the blueprint weapon's +4).
// - FAF's skipping in pass 2 takes pass 3's list of this frame, not of the frame before.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
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
  // The script's Inner and Outer pairs: the line's thickness nearest, and farthest before its coefficient and the
  // map's size (Thickness below).
  float inner[2] = {0, 0}, outer[2] = {0, 0};
};

// What the extractors read of a unit blueprint (once per blueprint).
struct BlueprintRanges {
  std::vector<int> weaponCategory;  // the RangeCategory (0..5) of each weapon the unit has (DummyWeapon left out)
  bool overlayMisc = false;         // in OVERLAYMISC
  float shield = 0;                 // half of Defense.Shield.ShieldSize
  float misc = 0;                   // AI.StagingPlatformScanRadius, else AI.GuardScanRadius
  float radar = 0, sonar = 0, omni = 0, radarStealthField = 0;  // (the Range form's: a unit's own come from its intel words)
  float jam = 0, spoof = 0;         // JamRadius.Max, SpoofRadius.Max
  bool sonarStealth = false, cloak = false;
  std::map<std::string, std::pair<float, float>> build;  // filter -> the Range form's inner, outer (outer 0: none)
};

// A ring found: where, its radii, its unit.
struct Found {
  float at[3], inner, outer;
  uint32_t unit;  // 0xffffffff for the building being placed
};
// One draw: the rings of one filter in one colour, of a pass.
struct Pass {
  const char* pass;
  const std::string* name;
  const RangeFilter* filter;
  uint32_t color;
  std::vector<Found> rings;
};

struct RangeStore : UserState::Extra {
  std::map<std::string, RangeFilter> filters;
  std::vector<std::string> active;
  // The console variables (all off until the game's script sets them, as the original's: multifunction.lua
  // switches the three on unless the profile says otherwise); range_Fill off, ren_Ranges on; the
  // thickness coefficients 1/1024.
  bool selected = false, highlighted = false, build = false, fill = false, ranges = true;
  float innerCoeff = 1.0f / 1024, outerCoeff = 1.0f / 1024;
  std::map<std::string, BlueprintRanges> blueprints;
  // The last scene the world view was given (for the test hook): its groups, and per group its pass, its
  // filter's name and each ring's unit; and the camera's numbers it was worked out with.
  std::vector<WorldRangeGroup> lastGroups;
  std::vector<std::pair<std::string, std::string>> lastInfo;
  std::vector<std::vector<uint32_t>> lastUnits;
  float zoomShare = 1, playable = 512;
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
bool Flag(lua_State* L, int t, const char* name) {
  lua_pushstring(L, name);
  lua_rawget(L, t);
  bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}
// t[name] as a table pushed on the stack (true), or nothing pushed (false).
bool SubTable(lua_State* L, int t, const char* name) {
  lua_pushstring(L, name);
  lua_rawget(L, t);
  if (lua_istable(L, -1)) return true;
  lua_pop(L, 1);
  return false;
}

int RangeCategoryOf(const std::string& s) {
  static const char* const kNames[] = {"UWRC_Undefined", "UWRC_DirectFire", "UWRC_IndirectFire", "UWRC_AntiAir",
                                       "UWRC_AntiNavy", "UWRC_Countermeasure"};
  for (int i = 0; i < 6; ++i)
    if (s == kNames[i]) return i;
  return 0;
}

// The weapons of a blueprint's list as the Range form takes them (0x7ec650): every weapon of the category,
// EffectiveRadius where it is above 0, else MaxRadius; the least MinRadius. false: none above 0.
bool BlueprintWeaponRange(lua_State* L, int bp, int category, float* inner, float* outer) {
  float most = 0, least = std::numeric_limits<float>::infinity();
  if (SubTable(L, bp, "Weapon")) {
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
      const int c = RangeCategoryOf(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
      lua_pop(L, 1);
      if (c == category) {
        float reach = Field(L, w, "EffectiveRadius");
        if (!(reach > 0)) reach = Field(L, w, "MaxRadius");
        most = std::max(most, reach);
        least = std::min(least, Field(L, w, "MinRadius"));
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  if (!(most > 0)) return false;
  *outer = most;
  *inner = std::isinf(least) ? 0 : least;
  return true;
}

// What a blueprint holds for the extractors (read once).
const BlueprintRanges& RangesOf(lua_State* L, RangeStore& store, const std::string& blueprint) {
  auto it = store.blueprints.find(blueprint);
  if (it != store.blueprints.end()) return it->second;
  BlueprintRanges& r = store.blueprints[blueprint];
  if (SimBlueprints* bps = SimBlueprints::From(L)) {
    const BlueprintInfo* info = bps->Find(blueprint);
    auto misc = bps->Categories().find("OVERLAYMISC");
    if (info && misc != bps->Categories().end())
      r.overlayMisc = std::find(misc->second.begin(), misc->second.end(), info->entityIndex) != misc->second.end();
  }
  int top = lua_gettop(L);
  lua_getglobal(L, "__blueprints");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, blueprint.c_str());
    lua_rawget(L, -2);
  }
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return r;
  }
  int bp = lua_gettop(L);
  if (SubTable(L, bp, "Weapon")) {
    int list = lua_gettop(L);
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      int w = lua_gettop(L);
      if (!Flag(L, w, "DummyWeapon")) {  // (the unit has no weapon for those: the engine's Unit ctor 0x6a5fb6)
        lua_pushstring(L, "RangeCategory");
        lua_rawget(L, w);
        r.weaponCategory.push_back(RangeCategoryOf(lua_isstring(L, -1) ? lua_tostring(L, -1) : ""));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  if (SubTable(L, bp, "Defense")) {
    if (SubTable(L, lua_gettop(L), "Shield")) {
      r.shield = Field(L, lua_gettop(L), "ShieldSize") * 0.5f;
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  if (SubTable(L, bp, "AI")) {
    int ai = lua_gettop(L);
    r.misc = Field(L, ai, "StagingPlatformScanRadius");
    if (!(r.misc > 0)) r.misc = Field(L, ai, "GuardScanRadius");
    lua_pop(L, 1);
  }
  if (SubTable(L, bp, "Intel")) {
    int intel = lua_gettop(L);
    auto most = [&](const char* name) {  // JamRadius / SpoofRadius = {Min, Max}
      float v = 0;
      if (SubTable(L, intel, name)) {
        v = Field(L, lua_gettop(L), "Max");
        lua_pop(L, 1);
      }
      return v;
    };
    r.radar = Field(L, intel, "RadarRadius");
    r.sonar = Field(L, intel, "SonarRadius");
    r.omni = Field(L, intel, "OmniRadius");
    r.radarStealthField = Field(L, intel, "RadarStealthFieldRadius");
    r.jam = most("JamRadius");
    r.spoof = most("SpoofRadius");
    r.sonarStealth = Flag(L, intel, "SonarStealth");
    r.cloak = Flag(L, intel, "Cloak");
    lua_pop(L, 1);
  }
  // The Range forms (pass 1), by the filter's name.
  static const char* const kWeapons[] = {"", "DirectFire", "IndirectFire", "AntiAir", "AntiNavy"};
  for (int c = 1; c <= 4; ++c) {
    float in = 0, out = 0;
    if (BlueprintWeaponRange(L, bp, c, &in, &out)) r.build[kWeapons[c]] = {in, out};
  }
  {
    float in = 0, out = 0;
    if (r.shield > 0) r.build["Defense"] = {0, r.shield};
    else if (BlueprintWeaponRange(L, bp, 5, &in, &out)) r.build["Defense"] = {in, out};
  }
  if (r.misc > 0) r.build["Miscellaneous"] = {0, r.misc};
  if (r.radar > 0) r.build["Radar"] = {0, r.radar};
  if (r.sonar > 0) r.build["Sonar"] = {0, r.sonar};
  if (r.omni > 0) r.build["Omni"] = {0, r.omni};
  const float counter = std::max({r.jam, r.spoof, r.radarStealthField, r.sonarStealth ? 1.0f : 0.0f, r.cloak ? 1.0f : 0.0f});
  if (counter > 0) r.build["CounterIntel"] = {0, counter};
  lua_settop(L, top);
  return r;
}

// The unit's weapons of a RangeCategory (6: any) as slot 0x44 (0x8bfc50) gives them: the greatest MaxRadius and
// the least MinRadius; false when the greatest is not above 0.
bool WeaponReach(const UnitView& u, const BlueprintRanges& k, int category, float* inner, float* outer) {
  float most = -std::numeric_limits<float>::infinity(), least = std::numeric_limits<float>::infinity();
  const size_t n = std::min(u.weapons.size(), k.weaponCategory.size());
  for (size_t i = 0; i < n; ++i) {
    if (category != 6 && k.weaponCategory[i] != category) continue;
    most = std::max(most, u.weapons[i].maxRadius);
    least = std::min(least, u.weapons[i].minRadius);
  }
  *outer = std::isinf(most) ? 0 : most;
  *inner = std::isinf(least) ? 0 : least;
  return *outer > 0;  // (the original also takes a ring with only an inner radius above 0; none has that)
}

// A filter's ring of a unit as it is now (the extractors' Extract): false when it has none.
bool Extract(lua_State* L, uint32_t id, const UnitView& u, const BlueprintRanges& k, const std::string& filter,
             float* inner, float* outer) {
  *inner = *outer = 0;
  // the intel toggle (8) and the jamming (4) and stealth (0x20) toggles set: the intel's rings are gone
  auto toggled = [&](uint32_t bits) {
    uint32_t on = 0;
    return (u.toggleCaps & bits) != 0 && UserUnitScriptBits(L, id, &on) && (on & u.toggleCaps & bits) != 0;
  };
  if (filter == "DirectFire") return WeaponReach(u, k, 1, inner, outer);
  if (filter == "IndirectFire") return WeaponReach(u, k, 2, inner, outer);
  if (filter == "AntiAir") return WeaponReach(u, k, 3, inner, outer);
  if (filter == "AntiNavy") return WeaponReach(u, k, 4, inner, outer);
  if (filter == "AllMilitary") {
    if (k.overlayMisc && k.misc > 0) return *outer = k.misc, true;
    return WeaponReach(u, k, 6, inner, outer);
  }
  if (filter == "Defense") {
    if (k.shield > 0) return *outer = k.shield, true;
    return WeaponReach(u, k, 5, inner, outer);
  }
  if (filter == "Miscellaneous") return (*outer = k.misc) > 0;
  // (the unit's intel words, UnitView::intel: the radius in the low 31 bits, the enabled bit not looked at)
  auto word = [&](int i) { return static_cast<float>(u.intel[i] & 0x7fffffffu); };
  if (filter == "Radar" || filter == "Sonar" || filter == "Omni" || filter == "AllIntel") {
    if (toggled(8)) return false;  // (UserUnit::GetIntelRanges 0x8bfd70: omni [4], radar [2], sonar [3])
    const float omni = word(4), radar = word(2), sonar = word(3);
    *outer = filter == "Radar" ? radar : filter == "Sonar" ? sonar : filter == "Omni" ? omni : std::max({omni, radar, sonar});
    return *outer > 0;
  }
  if (filter == "CounterIntel") {
    if (toggled(4) || toggled(0x20)) return false;  // (UserUnit::GetMaxCounterIntel 0x8bfe50)
    *outer = std::max({word(7), word(6), word(5), k.jam, k.spoof});
    return *outer > 0;
  }
  return false;  // (no extractor of that name: no rings, 0x7eda40)
}

bool InFilter(lua_State* L, const RangeFilter& f, const std::string& blueprint) {
  SimBlueprints* bps = SimBlueprints::From(L);
  const BlueprintInfo* bp = bps ? bps->Find(blueprint) : nullptr;
  return bp && bp->entityIndex >= 0 && static_cast<size_t>(bp->entityIndex >> 6) < f.categories.size() &&
         CategoryHas(f.categories.data(), bp->entityIndex);
}

// The thickness of a filter's line now (func_RenderRings 0x7ef5a0).
float Thickness(const float pair[2], float coeff, float zoomShare, float playable) {
  return pair[0] + (pair[1] * coeff * playable - pair[0]) * zoomShare;
}

// The rings in the original's passes (see the head of this file), one Pass per draw.
void Passes(lua_State* L, RangeStore& store, const RangeView& view, std::vector<Pass>* out) {
  UserState* user = UserState::From(L);
  if (!store.ranges || store.filters.empty()) return;
  Sim* sim = user->LiveSim("range rings");
  if (!sim) return;
  const int own = sim->focusArmy >= 0 ? sim->focusArmy + 1 : -1;
  WorldSource* world = user->config().world;
  // Where a unit is drawn this frame (the picture's), else where the last beat left it.
  auto placeOf = [&](uint32_t id, const UnitView& u, float at[3]) {
    float q[4];
    if (view.interpolate && world && world->UnitTransform(id, at, q)) return;
    for (int i = 0; i < 3; ++i) at[i] = u.position[i];
  };
  // The filters in the map's order, and per blueprint, worked out once a call, what the extractors read of it
  // and which filters hold it (a selection may hold thousands of one kind: one blueprint, not thousands).
  using Entry = std::pair<const std::string, RangeFilter>;
  std::vector<const Entry*> entries;
  for (const Entry& e : store.filters) entries.push_back(&e);
  struct Kind {
    const BlueprintRanges* ranges;
    std::vector<char> in;  // per entry
  };
  std::unordered_map<std::string, Kind> kinds;
  auto kindOf = [&](const std::string& bp) -> const Kind& {
    auto it = kinds.find(bp);
    if (it != kinds.end()) return it->second;
    Kind k{&RangesOf(L, store, bp), std::vector<char>(entries.size())};
    for (size_t i = 0; i < entries.size(); ++i) k.in[i] = InFilter(L, entries[i]->second, bp);
    return kinds.emplace(bp, std::move(k)).first->second;
  };
  struct Unit {
    uint32_t id;
    const UnitView* view;
    const Kind* kind;
  };
  auto isCombo = [](const std::string& n) { return n == "AllMilitary" || n == "AllIntel"; };
  float at[3], inner = 0, outer = 0;
  // The rings of a filter (entry i) of some units, as one draw (into `to`, when there are any).
  auto ringsOf = [&](const char* pass, size_t i, uint32_t color, const std::vector<Unit>& units, std::set<uint32_t>* ringed,
                     std::vector<Pass>* to) {
    Pass p{pass, &entries[i]->first, &entries[i]->second, color, {}};
    for (const Unit& u : units) {
      if (!u.kind->in[i] || !Extract(L, u.id, *u.view, *u.kind->ranges, entries[i]->first, &inner, &outer)) continue;
      placeOf(u.id, *u.view, at);
      p.rings.push_back({{at[0], at[1], at[2]}, inner, outer, u.id});
      if (ringed) ringed->insert(u.id);
    }
    if (!p.rings.empty()) to->push_back(std::move(p));
  };
  // 1. the building about to be placed
  if (store.build && view.placing && !view.blueprint.empty()) {
    const Kind& k = kindOf(view.blueprint);
    for (size_t i = 0; i < entries.size(); ++i) {
      if (isCombo(entries[i]->first) || !k.in[i]) continue;
      auto r = k.ranges->build.find(entries[i]->first);
      if (r == k.ranges->build.end()) continue;
      Pass p{"build", &entries[i]->first, &entries[i]->second, entries[i]->second.normal, {}};
      p.rings.push_back({{view.mouse[0], view.mouse[1], view.mouse[2]}, r->second.first, r->second.second, 0xffffffffu});
      out->push_back(std::move(p));
    }
  }
  if (own < 0) return;
  // 3. worked out first: pass 2 leaves out the selected units that have a ring here (FAF)
  std::vector<Pass> selected;
  std::set<uint32_t> ringed;
  if (store.selected) {
    std::vector<uint32_t> ids;
    UserUnitIds(L, true, &ids);
    std::vector<Unit> selection;
    for (uint32_t id : ids)
      if (const UnitView* u = UserUnitView(L, id); u && u->army == own) selection.push_back({id, u, &kindOf(u->blueprint)});
    for (size_t i = 0; i < entries.size(); ++i)
      if (!isCombo(entries[i]->first)) ringsOf("selected", i, entries[i]->second.select, selection, &ringed, &selected);
  }
  // 2. the filters switched on
  if (!store.active.empty()) {
    std::vector<uint32_t> all, inView;
    UserUnitIds(L, false, &all);
    const bool listed = view.unitsInView && view.unitsInView(&inView);
    std::sort(inView.begin(), inView.end());
    std::vector<Unit> units;
    for (uint32_t id : all) {
      const UnitView* u = UserUnitView(L, id);
      if (!u || u->army != own || u->beingBuilt || ringed.count(id)) continue;
      if (listed ? !std::binary_search(inView.begin(), inView.end(), id) : !PlaceInCameraView(L, u->position)) continue;
      units.push_back({id, u, &kindOf(u->blueprint)});
    }
    for (const std::string& name : store.active) {
      auto f = store.filters.find(name);
      if (f == store.filters.end()) continue;
      const size_t i = static_cast<size_t>(std::distance(store.filters.begin(), f));
      ringsOf("active", i, f->second.normal, units, nullptr, out);
    }
  }
  for (Pass& p : selected) out->push_back(std::move(p));
  // 4. the unit under the mouse
  if (store.highlighted && view.rollover != 0xffffffffu) {
    const UnitView* u = UserUnitView(L, view.rollover);
    if (u && u->army == own) {
      const std::vector<Unit> one = {{view.rollover, u, &kindOf(u->blueprint)}};
      for (size_t i = 0; i < entries.size(); ++i)
        if (!isCombo(entries[i]->first)) ringsOf("highlighted", i, entries[i]->second.rollover, one, nullptr, out);
    }
  }
  // 5. the focus army's no-rush circle while its timer runs (ExtractFocusArmyNoRushRange 0x7ef1c0, drawn at
  // 0x7eebd7 .. 0x7eec43 with no switch of its own: grey 0.2 with glow 0.2, the Inner pair 0.1 / 1, the Outer 1 / 2)
  static const std::string noRushName = "NoRush";
  static const RangeFilter noRush = [] {
    RangeFilter f;
    f.normal = f.select = f.rollover = 0x33333333u;
    f.inner[0] = 0.1f, f.inner[1] = 1, f.outer[0] = 1, f.outer[1] = 2;
    return f;
  }();
  for (const ArmyView& a : ArmyViews(*sim)) {
    if (a.index != own || a.noRushTicks <= 0) continue;
    Pass p{"norush", &noRushName, &noRush, noRush.normal, {}};
    p.rings.push_back({{a.start[0] + a.noRushOffset[0], 0, a.noRushOffset[1] + a.start[1]}, 0, a.noRushRadius, 0xffffffffu});
    out->push_back(std::move(p));
  }
}

void ToGroups(const RangeStore& store, const RangeView& view, const std::vector<Pass>& passes,
              std::vector<WorldRangeGroup>* out, std::vector<std::pair<std::string, std::string>>* info,
              std::vector<std::vector<uint32_t>>* units) {
  for (const Pass& p : passes) {
    WorldRangeGroup g;
    g.color = 0xff000000u | (p.color & 0x00ffffffu);
    g.glow = static_cast<int>(p.color >> 24);
    // (the thickness as the original works it out for this frame: the same at both ends, so that the
    // renderer's own blend by the camera's distance changes nothing)
    g.outer[0] = g.outer[1] = Thickness(p.filter->outer, store.outerCoeff, view.zoomShare, view.playable);
    g.inner[0] = g.inner[1] = Thickness(p.filter->inner, store.innerCoeff, view.zoomShare, view.playable);
    g.fill = store.fill;  // (range_Fill: every group, as the original; the renderer's RangeGroup::fill, render a8ac1f0)
    std::vector<uint32_t> ids;
    for (const Found& r : p.rings) {
      WorldRangeRing ring;
      for (int i = 0; i < 3; ++i) ring.centre[i] = r.at[i];
      ring.radius = r.outer;
      ring.innerRadius = r.inner > 0 ? r.inner : 0;
      g.rings.push_back(ring);
      ids.push_back(r.unit);
    }
    out->push_back(std::move(g));
    if (info) info->push_back({p.pass, *p.name});
    if (units) units->push_back(std::move(ids));
  }
}

// SetOverlayFilter(name, categories, normalColor, selectColor, rolloverColor, innerNear, innerFar,
// outerNear, outerFar)
int l_SetOverlayFilter(lua_State* L) {
  RangeFilter f;
  if (const uint64_t* bits = ToCategory(L, 2)) f.categories.assign(bits, bits + CategoryWordCount(L));
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
// moho64ui_RangeRings([which]) -> the rings, each {x, z, radius, inner, color ("rrggbb"), glow, group (its
// group's number, in the order drawn), pass ("build", "active", "selected", "highlighted"), filter (its
// filter's name), unit (its unit's entity id; none for the building being placed), outer and innerLine (the
// thickness of the group's outline and of a hole's outline)}, and the switches {selected, highlighted, build,
// fill, ranges, innerCoeff, outerCoeff, zoomShare, playable, active = {names}}: the host's own function for
// tests. which = "scene": the last scene the main world view gave the picture; else worked out now (the unit
// under the mouse, no building being placed, the camera's numbers of the last scene).
int l_RangeRings(lua_State* L) {
  RangeStore& store = Store(L);
  std::vector<WorldRangeGroup> groups;
  std::vector<std::pair<std::string, std::string>> info;
  std::vector<std::vector<uint32_t>> units;
  const char* which = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
  // (the main view's picture made first, as a drawn frame makes it: a headless run draws no frames)
  std::vector<uint32_t> onScreen;
  const bool picture = UnitsOnScreen(L, &onScreen);
  if (std::string(which) == "scene" && picture) {
    groups = store.lastGroups, info = store.lastInfo, units = store.lastUnits;
  } else {
    RangeView view;
    view.rollover = UnitUnderMouse(L);
    view.zoomShare = store.zoomShare, view.playable = store.playable;
    view.unitsInView = [&](std::vector<uint32_t>* ids) { return picture && (*ids = onScreen, true); };
    std::vector<Pass> passes;
    Passes(L, store, view, &passes);
    ToGroups(store, view, passes, &groups, &info, &units);
  }
  lua_newtable(L);
  int n = 0;
  auto number = [&](const char* k, double d) {
    lua_pushstring(L, k);
    lua_pushnumber(L, d);
    lua_rawset(L, -3);
  };
  auto text = [&](const char* k, const char* v) {
    lua_pushstring(L, k);
    lua_pushstring(L, v);
    lua_rawset(L, -3);
  };
  for (size_t g = 0; g < groups.size(); ++g) {
    for (size_t i = 0; i < groups[g].rings.size(); ++i) {
      const WorldRangeRing& r = groups[g].rings[i];
      lua_newtable(L);
      number("x", r.centre[0]), number("z", r.centre[2]), number("radius", r.radius), number("inner", r.innerRadius);
      number("glow", groups[g].glow), number("group", static_cast<double>(g + 1));
      number("outer", groups[g].outer[0]), number("innerLine", groups[g].inner[0]);
      char color[16];
      std::snprintf(color, sizeof color, "%06x", groups[g].color & 0x00ffffffu);
      text("color", color);
      if (g < info.size()) text("pass", info[g].first.c_str()), text("filter", info[g].second.c_str());
      if (g < units.size() && i < units[g].size() && units[g][i] != 0xffffffffu) number("unit", units[g][i]);
      lua_rawseti(L, -2, ++n);
    }
  }
  lua_newtable(L);
  auto flag = [&](const char* k, bool b) {
    lua_pushstring(L, k);
    lua_pushboolean(L, b);
    lua_rawset(L, -3);
  };
  flag("selected", store.selected), flag("highlighted", store.highlighted), flag("build", store.build);
  flag("fill", store.fill), flag("ranges", store.ranges);
  number("innerCoeff", store.innerCoeff), number("outerCoeff", store.outerCoeff);
  number("zoomShare", store.zoomShare), number("playable", store.playable);
  lua_pushstring(L, "active");
  lua_newtable(L);
  for (size_t i = 0; i < store.active.size(); ++i) {
    lua_pushstring(L, store.active[i].c_str());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
  return 2;
}

// The console variables of the range rings: their spelling in the exe's registry, their kind and where they are.
struct RangeVar {
  const char* upper;
  const char* name;
  bool RangeStore::*flag;
  float RangeStore::*number;
};
const RangeVar kRangeVars[] = {
    {"RANGE_RENDERSELECTED", "range_RenderSelected", &RangeStore::selected, nullptr},
    {"RANGE_RENDERHIGHLIGHTED", "range_RenderHighlighted", &RangeStore::highlighted, nullptr},
    {"RANGE_RENDERBUILD", "range_RenderBuild", &RangeStore::build, nullptr},
    {"RANGE_FILL", "range_Fill", &RangeStore::fill, nullptr},
    {"RANGE_INNERTHICKNESSCOEFF", "range_InnerThicknessCoeff", nullptr, &RangeStore::innerCoeff},
    {"RANGE_OUTERTHICKNESSCOEFF", "range_OuterThicknessCoeff", nullptr, &RangeStore::outerCoeff},
    {"REN_RANGES", "ren_Ranges", &RangeStore::ranges, nullptr},
};
const RangeVar* RangeVarOf(const std::string& upperName) {
  for (const RangeVar& v : kRangeVars)
    if (upperName == v.upper) return &v;
  return nullptr;
}

}  // namespace

bool RangeConsoleCommand(lua_State* L, const std::string& upperName, const std::string& value) {
  const RangeVar* var = RangeVarOf(upperName);
  if (!var) return false;
  RangeStore& store = Store(L);
  std::string v;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  if (var->flag) {
    bool& flag = store.*(var->flag);
    if (v.empty()) flag = !flag;  // a console variable without a value turns over
    else flag = v == "true" || v == "1" || v == "on";
  } else if (!v.empty()) {  // (a number; "= n" too)
    if (v[0] == '=') v.erase(0, 1);
    if (!v.empty()) store.*(var->number) = static_cast<float>(std::atof(v.c_str()));
  }
  return true;
}

bool RangeConVar(lua_State* L, const std::string& upperName, const char** name, char* kind, double* value) {
  const RangeVar* var = RangeVarOf(upperName);
  if (!var) return false;
  const RangeStore& store = Store(L);
  *name = var->name;
  *kind = var->flag ? 'b' : 'f';
  *value = var->flag ? (store.*(var->flag) ? 1 : 0) : store.*(var->number);
  return true;
}

void RangeRingsForWorld(lua_State* L, const RangeView& view, std::vector<WorldRangeGroup>* out) {
  if (!UserState::From(L)->ranges) return;
  RangeStore& store = Store(L);
  store.zoomShare = view.zoomShare, store.playable = view.playable;
  std::vector<Pass> passes;
  Passes(L, store, view, &passes);
  store.lastInfo.clear(), store.lastUnits.clear();
  ToGroups(store, view, passes, out, &store.lastInfo, &store.lastUnits);
  store.lastGroups = *out;
}

void RegisterRangeBindings(lua_State* L) {
  if (!UserState::From(L)->HasSim()) return;
  SetGlobal(L, "SetOverlayFilter", l_SetOverlayFilter);
  SetGlobal(L, "SetOverlayFilters", l_SetOverlayFilters);
  lua_register(L, "moho64ui_RangeRings", l_RangeRings);
}

}  // namespace moho::ui
