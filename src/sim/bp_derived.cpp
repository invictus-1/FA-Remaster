// Blueprint fields the original engine works out itself when it initialises a blueprint.
//
// REntityBlueprint::OnInitBlueprint (units, props, projectiles):
//   - Footprint/AltFootprint sizes default to the entity size rounded up;
//   - InertiaTensorX/Y/Z default to a solid box: (b^2 + c^2) / 12.
// RUnitBlueprintPhysics::ComputeDerivedQuantities (units):
//   - no MaxSpeed means RULEUMT_None; a negative MaxSpeedReverse means MaxSpeed;
//   - AttackElevation defaults to Elevation;
//   - a moving unit's footprint is the nearest named footprint (SpecFootprints, /lua/footprints.lua)
//     with the occupancy of its motion type; the alternate footprint likewise, or the main one;
//   - a structure's footprint occupancy is its BuildOnLayerCaps;
//   - skirt offsets are clamped to <= 0 and the skirt is at least the footprint;
//   - CatchUpAcc defaults to max(MaxAcceleration, MaxBrake); BackUpDistance to 3 * SizeZ.
// RUnitBlueprint::OnInitBlueprint (units): Air.CanFly for RULEUMT_Air, MaxAirspeed = MaxSpeed for
//   flyers, MinAirspeed = MaxAirspeed, StartTurnDistance = 3 * SizeZ.
// RMeshBlueprint::Init (each mesh LOD): empty texture/mesh names become <base>_lod<n>.scm,
//   _albedo.dds, _normalsTS.dds, _SpecTeam.dds, _lookup.dds when that file exists (base: the
//   blueprint file's name up to its last '_'); given names
//   are completed relative to the blueprint's file.
//
// The arithmetic is done in single precision, as the engine stores these fields as floats.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "sim/blueprints.h"

namespace moho {
namespace {

float GetF(lua_State* L, int t, const char* k, float def) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  float v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L, -1));
  else if (lua_type(L, -1) == LUA_TSTRING) v = static_cast<float>(std::atof(lua_tostring(L, -1)));
  lua_pop(L, 1);
  return v;
}

bool GetB(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}

std::string GetS(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return v;
}

void SetF(lua_State* L, int t, const char* k, float v) {
  lua_pushstring(L, k);
  lua_pushnumber(L, static_cast<lua_Number>(v));
  lua_rawset(L, t);
}

void SetB(lua_State* L, int t, const char* k, bool v) {
  lua_pushstring(L, k);
  lua_pushboolean(L, v);
  lua_rawset(L, t);
}

void SetS(lua_State* L, int t, const char* k, const std::string& v) {
  lua_pushstring(L, k);
  lua_pushstring(L, v.c_str());
  lua_rawset(L, t);
}

// Push t[k] as a table (created if missing) and return its index.
int SubTable(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  if (lua_istable(L, -1)) return lua_gettop(L);
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushstring(L, k);
  lua_pushvalue(L, -2);
  lua_rawset(L, t);
  return lua_gettop(L);
}

// Push t[k] if it is a table and return its index, else 0 (nothing pushed).
int OptTable(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  if (lua_istable(L, -1)) return lua_gettop(L);
  lua_pop(L, 1);
  return 0;
}

// The engine's SFootprint as the sim table shows it (MaxWaterDepth is not shown).
NamedFootprint ReadFootprint(lua_State* L, int t) {
  NamedFootprint f;
  f.sizeX = static_cast<uint8_t>(static_cast<int>(GetF(L, t, "SizeX", 0)));
  f.sizeZ = static_cast<uint8_t>(static_cast<int>(GetF(L, t, "SizeZ", 0)));
  f.caps = static_cast<uint8_t>(static_cast<int>(GetF(L, t, "OccupancyCaps", 0)));
  f.flags = static_cast<uint8_t>(static_cast<int>(GetF(L, t, "Flags", 0)));
  f.maxSlope = GetF(L, t, "MaxSlope", 0);
  f.minWaterDepth = GetF(L, t, "MinWaterDepth", 0);
  return f;
}

void WriteFootprint(lua_State* L, int t, const NamedFootprint& f) {
  SetF(L, t, "SizeX", f.sizeX);
  SetF(L, t, "SizeZ", f.sizeZ);
  SetF(L, t, "OccupancyCaps", f.caps);
  SetF(L, t, "Flags", f.flags);
  SetF(L, t, "MaxSlope", f.maxSlope);
  SetF(L, t, "MinWaterDepth", f.minWaterDepth);
}

// The entity size rounded up (the original: round to nearest, plus one if that rounded down).
uint8_t CeilSize(float v) {
  float r = std::nearbyint(v);
  return static_cast<uint8_t>(static_cast<int>(r) + (v > r ? 1 : 0));
}

// RULEUMT_* order and the occupancy layers each motion type needs.
const char* const kMotionTypes[] = {"RULEUMT_None",         "RULEUMT_Land",  "RULEUMT_Air",
                                    "RULEUMT_Water",        "RULEUMT_Biped", "RULEUMT_SurfacingSub",
                                    "RULEUMT_Amphibious",   "RULEUMT_Hover", "RULEUMT_AmphibiousFloating",
                                    "RULEUMT_Special"};
const uint8_t kMotionOccupancy[] = {0, 1, 16, 8, 1, 12, 3, 9, 9, 0};
constexpr int kMotionAir = 2;

int MotionIndex(const std::string& s) {
  for (int i = 0; i < 10; ++i)
    if (s == kMotionTypes[i]) return i;
  return 0;
}

const char* const kLayerFlags[] = {"LAYER_Land", "LAYER_Seabed", "LAYER_Sub", "LAYER_Water", "LAYER_Air", "LAYER_Orbit"};
const char* const kLayerNames[] = {"Land", "Seabed", "Sub", "Water", "Air", "Orbit"};

// Physics.BuildOnLayerCaps as a layer mask (a table of LAYER_* flags, a layer name or a number).
uint8_t BuildLayers(lua_State* L, int phys) {
  lua_pushstring(L, "BuildOnLayerCaps");
  lua_rawget(L, phys);
  uint32_t v = 1;  // Land
  if (lua_istable(L, -1)) {
    v = 0;
    for (int i = 0; i < 6; ++i) {
      lua_pushstring(L, kLayerFlags[i]);
      lua_rawget(L, -2);
      if (lua_toboolean(L, -1)) v |= 1u << i;
      lua_pop(L, 1);
    }
  } else if (lua_type(L, -1) == LUA_TSTRING) {
    const char* s = lua_tostring(L, -1);
    v = static_cast<uint32_t>(std::atoi(s));
    for (int i = 0; i < 6; ++i)
      if (!std::strcmp(s, kLayerNames[i])) v = 1u << i;
  } else if (lua_type(L, -1) == LUA_TNUMBER) {
    v = static_cast<uint32_t>(lua_tonumber(L, -1));
  }
  lua_pop(L, 1);
  return static_cast<uint8_t>(v);
}

// RRuleGameRules::FindFootprint: same occupancy, nearest size (first one on a tie).
const NamedFootprint* FindFootprint(const std::vector<NamedFootprint>& list, const NamedFootprint& f,
                                    const std::string& id) {
  const NamedFootprint* best = nullptr;
  int bestD = 0x7fff;
  for (const auto& n : list) {
    if (n.caps != f.caps) continue;
    int d = std::max(std::abs(n.sizeX - f.sizeX), std::abs(n.sizeZ - f.sizeZ));
    if (d < bestD) {
      bestD = d;
      best = &n;
    }
  }
  if (!best) Logf(LogLevel::Warning, "Could not find named footprint for %s", id.c_str());
  return best;
}

void DeriveEntity(lua_State* L, int t) {
  float sx = GetF(L, t, "SizeX", 1), sy = GetF(L, t, "SizeY", 1), sz = GetF(L, t, "SizeZ", 1);
  for (const char* key : {"Footprint", "AltFootprint"}) {
    int f = SubTable(L, t, key);
    NamedFootprint fp = ReadFootprint(L, f);
    if (!fp.sizeX) fp.sizeX = CeilSize(sx);
    if (!fp.sizeZ) fp.sizeZ = CeilSize(sz);
    WriteFootprint(L, f, fp);
    lua_pop(L, 1);
  }
  float ix = GetF(L, t, "InertiaTensorX", 0), iy = GetF(L, t, "InertiaTensorY", 0),
        iz = GetF(L, t, "InertiaTensorZ", 0);
  if (iy * iz * ix == 0.0f) {
    const float c = 1.0f / 12.0f;
    float x2 = sx * sx, y2 = sy * sy, z2 = sz * sz;
    ix = (z2 + y2) * c;
    iy = (z2 + x2) * c;
    iz = (y2 + x2) * c;
  }
  SetF(L, t, "InertiaTensorX", ix);
  SetF(L, t, "InertiaTensorY", iy);
  SetF(L, t, "InertiaTensorZ", iz);
}

void DeriveUnit(lua_State* L, int t, const BlueprintInfo& bp, const SimBlueprints& bps) {
  float sizeZ = GetF(L, t, "SizeZ", 1);
  int p = SubTable(L, t, "Physics");
  int fpT = SubTable(L, t, "Footprint");
  int altT = SubTable(L, t, "AltFootprint");
  NamedFootprint fp = ReadFootprint(L, fpT), alt = ReadFootprint(L, altT);

  float maxSpeed = GetF(L, p, "MaxSpeed", 0);
  int motion = MotionIndex(GetS(L, p, "MotionType"));
  int altMotion = MotionIndex(GetS(L, p, "AltMotionType"));
  float reverse = GetF(L, p, "MaxSpeedReverse", -1);
  if (maxSpeed == 0.0f) {
    motion = 0;
    SetS(L, p, "MotionType", kMotionTypes[0]);
  } else if (reverse < 0.0f) {
    reverse = maxSpeed;
  }
  SetF(L, p, "MaxSpeedReverse", reverse);
  float attackElevation = GetF(L, p, "AttackElevation", 0);
  if (attackElevation == 0.0f) attackElevation = GetF(L, p, "Elevation", 0);
  SetF(L, p, "AttackElevation", attackElevation);

  const auto& named = bps.Footprints();
  if (motion) {
    fp.caps = kMotionOccupancy[motion];
    fp.flags = 0;
    if (fp.caps & 0xf)
      if (const NamedFootprint* n = FindFootprint(named, fp, bp.id)) fp = *n;
    if (altMotion) {
      alt.caps = kMotionOccupancy[altMotion];
      alt.flags = 0;
      if (fp.caps & 0xf) {
        const NamedFootprint* n = FindFootprint(named, alt, bp.id);
        alt = n ? *n : fp;
      }
    } else {
      alt = fp;
    }
  } else {
    fp.caps = BuildLayers(L, p);
  }
  WriteFootprint(L, fpT, fp);
  WriteFootprint(L, altT, alt);

  SetF(L, p, "SkirtOffsetX", std::min(GetF(L, p, "SkirtOffsetX", 0), 0.0f));
  SetF(L, p, "SkirtOffsetZ", std::min(GetF(L, p, "SkirtOffsetZ", 0), 0.0f));
  SetF(L, p, "SkirtSizeX", std::max(GetF(L, p, "SkirtSizeX", 0), static_cast<float>(fp.sizeX)));
  SetF(L, p, "SkirtSizeZ", std::max(GetF(L, p, "SkirtSizeZ", 0), static_cast<float>(fp.sizeZ)));
  float catchUp = GetF(L, p, "CatchUpAcc", 0);
  if (catchUp == 0.0f) catchUp = std::max(GetF(L, p, "MaxAcceleration", 0), GetF(L, p, "MaxBrake", 0));
  SetF(L, p, "CatchUpAcc", catchUp);
  float backUp = GetF(L, p, "BackUpDistance", -1);
  if (backUp < 0.0f) backUp = sizeZ * 3.0f;
  SetF(L, p, "BackUpDistance", backUp);

  int air = SubTable(L, t, "Air");
  bool canFly = GetB(L, air, "CanFly") || motion == kMotionAir;
  SetB(L, air, "CanFly", canFly);
  float maxAir = GetF(L, air, "MaxAirspeed", 0);
  if (maxAir == 0.0f && canFly) maxAir = maxSpeed;
  SetF(L, air, "MaxAirspeed", maxAir);
  float minAir = GetF(L, air, "MinAirspeed", 0);
  if (minAir == 0.0f) minAir = maxAir;
  SetF(L, air, "MinAirspeed", minAir);
  float turn = GetF(L, air, "StartTurnDistance", 0);
  if (turn == 0.0f) turn = sizeZ * 3.0f;
  SetF(L, air, "StartTurnDistance", turn);
  lua_pop(L, 4);
}

// RES_CompletePath: a relative name is taken from the directory of `source`; "." and ".." collapse.
std::string CompletePath(const std::string& name, const std::string& source) {
  std::string s = name;
  for (auto& c : s)
    if (c == '\\') c = '/';
  if (s.empty() || s[0] != '/') {
    std::string src = source;
    for (auto& c : src)
      if (c == '\\') c = '/';
    size_t slash = src.rfind('/');
    s = (slash == std::string::npos ? std::string("/") : src.substr(0, slash + 1)) + s;
  }
  std::vector<std::string> parts;
  size_t a = 1;
  while (a <= s.size()) {
    size_t b = s.find('/', a);
    if (b == std::string::npos) b = s.size();
    std::string part = s.substr(a, b - a);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    a = b + 1;
  }
  std::string out;
  for (const auto& part : parts) out += "/" + part;
  return out.empty() ? "/" : out;
}

void DeriveMesh(lua_State* L, int mesh, const SimBlueprints& bps) {
  // Default names start from the blueprint file's name up to its last '_' ("/units/x/x_unit.bp").
  std::string source = GetS(L, mesh, "Source");
  size_t us = source.rfind('_');
  std::string base = us == std::string::npos ? source : source.substr(0, us);
  int lods = OptTable(L, mesh, "LODs");
  if (!lods) return;
  struct Name {
    const char* key;
    const char* suffix;
  };
  static const Name kNames[] = {{"MeshName", nullptr},
                                {"AlbedoName", "_albedo.dds"},
                                {"NormalsName", "_normalsTS.dds"},
                                {"SpecularName", "_SpecTeam.dds"},
                                {"LookupName", "_lookup.dds"}};
  for (int i = 1;; ++i) {
    lua_rawgeti(L, lods, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    int lod = lua_gettop(L);
    for (const auto& n : kNames) {
      std::string v = GetS(L, lod, n.key);
      if (v.empty()) {
        std::string guess = n.suffix ? base + n.suffix : base + "_lod" + std::to_string(i - 1) + ".scm";
        if (bps.fileExists && bps.fileExists(guess)) v = guess;
      } else {
        v = CompletePath(v, source);
      }
      SetS(L, lod, n.key, v);
    }
    if (GetS(L, lod, "SecondaryName").empty()) SetS(L, lod, "SecondaryName", "");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

}  // namespace

void DeriveBlueprint(lua_State* L, int t, const BlueprintInfo& bp, const SimBlueprints& bps) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  lua_checkstack(L, 20);
  if (bp.kind == BpKind::Mesh) {
    DeriveMesh(L, t, bps);
    return;
  }
  if (bp.kind != BpKind::Unit && bp.kind != BpKind::Prop && bp.kind != BpKind::Projectile) return;
  DeriveEntity(L, t);
  if (bp.kind == BpKind::Unit) DeriveUnit(L, t, bp, bps);
  // An entity's Display.Mesh is left alone: when it is a registered mesh blueprint (the same
  // table), that blueprint's own initialisation fills it in.
}

}  // namespace moho
