// Engine functions present in every Lua state (the original's "core" set). Behaviour follows
// the original engine; see docs/lua-dialect.md and the binding list for the reference names.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/dmath.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace moho {
void PushVector(lua_State* L, float x, float y, float z);
namespace {

Vfs* GetVfs(lua_State* L) { return ScriptState::From(L)->vfs(); }

// Concatenate all arguments with tostring(), as LOG/WARN/SPEW do.
std::string JoinArgs(lua_State* L) {
  std::string out;
  int n = lua_gettop(L);
  lua_getglobal(L, "tostring");
  for (int i = 1; i <= n; ++i) {
    lua_pushvalue(L, -1);
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
    const char* s = lua_tostring(L, -1);
    if (i > 1) out += '\t';  // the original separates arguments with tabs, like print
    if (s) out += s;
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return out;
}

int l_LOG(lua_State* L) {
  LogWrite(LogLevel::Info, JoinArgs(L));
  return 0;
}
int l_WARN(lua_State* L) {
  LogWrite(LogLevel::Warning, JoinArgs(L));
  return 0;
}
int l_SPEW(lua_State* L) {
  LogWrite(LogLevel::Debug, JoinArgs(L));
  return 0;
}

int l_doscript(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  bool hasEnv = lua_istable(L, 2);
  int rc = LoadScriptWithHooks(L, name);
  if (rc != 0) return lua_error(L);
  if (hasEnv) {
    lua_pushvalue(L, 2);
    lua_setfenv(L, -2);
  }
  lua_call(L, 0, 0);
  return 0;
}

int l_DiskFindFiles(lua_State* L) {
  const char* dir = luaL_checkstring(L, 1);
  const char* pattern = luaL_checkstring(L, 2);
  auto files = GetVfs(L)->FindFiles(dir, pattern);
  lua_newtable(L);
  int i = 1;
  for (const auto& f : files) {
    lua_pushstring(L, f.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

int l_DiskGetFileInfo(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  auto info = GetVfs(L)->GetFileInfo(name);
  if (!info) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, "IsFolder");
  lua_pushboolean(L, info->isFolder);
  lua_rawset(L, -3);
  lua_pushstring(L, "IsReadOnly");
  lua_pushboolean(L, info->isReadOnly);
  lua_rawset(L, -3);
  lua_pushstring(L, "SizeBytes");
  lua_pushnumber(L, static_cast<lua_Number>(info->size));
  lua_rawset(L, -3);
  lua_pushstring(L, "LastWriteTime");
  lua_pushnumber(L, static_cast<lua_Number>(info->writeTime));
  lua_rawset(L, -3);
  return 1;
}

int l_DiskToLocal(lua_State* L) {
  std::string p = luaL_checkstring(L, 1);
  lua_pushstring(L, GetVfs(L)->ToLocal(p).c_str());
  return 1;
}

int l_Basename(lua_State* L) {
  std::string p = luaL_checkstring(L, 1);
  bool strip = lua_toboolean(L, 2);
  size_t s = p.find_last_of("/\\");
  std::string base = s == std::string::npos ? p : p.substr(s + 1);
  if (strip) {
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base.erase(dot);
  }
  lua_pushstring(L, base.c_str());
  return 1;
}

int l_Dirname(lua_State* L) {
  std::string p = luaL_checkstring(L, 1);
  size_t s = p.find_last_of("/\\");
  lua_pushstring(L, s == std::string::npos ? "" : p.substr(0, s).c_str());
  return 1;
}

int l_FileCollapsePath(lua_State* L) {
  std::string p = luaL_checkstring(L, 1);
  std::string c = hostfs::CleanPath(p);
  if (!p.empty() && (p[0] == '/' || p[0] == '\\') && (c.empty() || c[0] != '/')) c.insert(c.begin(), '/');
  lua_pushstring(L, c.c_str());
  return 1;
}

int l_exists(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_pushboolean(L, GetVfs(L)->Exists(name));
  return 1;
}

int l_STR_xtoi(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  lua_pushnumber(L, static_cast<lua_Number>(static_cast<int>(std::strtoul(s, nullptr, 16))));
  return 1;
}

int l_STR_itox(lua_State* L) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%x", static_cast<unsigned>(static_cast<int>(luaL_checknumber(L, 1))));
  lua_pushstring(L, buf);
  return 1;
}

size_t Utf8Next(const std::string& s, size_t i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  return std::min(s.size(), i + n);
}

int l_STR_Utf8Len(lua_State* L) {
  std::string s = luaL_checkstring(L, 1);
  int n = 0;
  for (size_t i = 0; i < s.size(); i = Utf8Next(s, i)) ++n;
  lua_pushnumber(L, static_cast<lua_Number>(n));
  return 1;
}

int l_STR_Utf8SubString(lua_State* L) {
  std::string s = luaL_checkstring(L, 1);
  int start = static_cast<int>(luaL_checknumber(L, 2));
  int count = static_cast<int>(luaL_checknumber(L, 3));
  size_t i = 0;
  for (int k = 1; k < start && i < s.size(); ++k) i = Utf8Next(s, i);
  size_t j = i;
  for (int k = 0; k < count && j < s.size(); ++k) j = Utf8Next(s, j);
  lua_pushstring(L, s.substr(i, j - i).c_str());
  return 1;
}

int l_STR_GetTokens(lua_State* L) {
  std::string s = luaL_checkstring(L, 1);
  std::string delim = luaL_checkstring(L, 2);
  lua_newtable(L);
  int idx = 1;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find_first_of(delim, i);
    if (j == std::string::npos) j = s.size();
    if (j > i) {
      lua_pushstring(L, s.substr(i, j - i).c_str());
      lua_rawseti(L, -2, idx++);
    }
    i = j + 1;
  }
  return 1;
}

int l_GetVersion(lua_State* L) {
  lua_pushstring(L, "1.5.3599");
  return 1;
}

int l_MATH_IRound(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(std::nearbyint(luaL_checknumber(L, 1))));
  return 1;
}

int l_MATH_Lerp(lua_State* L) {
  float s = luaL_checknumber(L, 1);
  if (lua_gettop(L) >= 5) {
    float smin = luaL_checknumber(L, 2), smax = luaL_checknumber(L, 3);
    float a = luaL_checknumber(L, 4), b = luaL_checknumber(L, 5);
    float t = (s - smin) / (smax - smin);
    lua_pushnumber(L, a + (b - a) * t);
  } else {
    float a = luaL_checknumber(L, 2), b = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * s);
  }
  return 1;
}

// --- vectors: tables {x, y, z} sharing a metatable that maps .x/.y/.z to [1]/[2]/[3] ---
const char* kVectorMeta = "moho.Vector";

float VecComp(lua_State* L, int idx, int k) {
  lua_rawgeti(L, idx, k);
  float v = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  return v;
}

// The original's vector metatable: the table must be a table, the key one of "x", "y", "z"
// (mapped to [1], [2], [3]); anything else is an argument error.
int VectorAxis(lua_State* L) {
  if (!lua_istable(L, 1)) luaL_argerror(L, 1, "Vector expected");
  const char* k = lua_tostring(L, 2);
  if (!k) luaL_argerror(L, 2, "'x', 'y', or 'z' expected");
  int axis = k[0] - 'w';
  if (axis < 1 || axis > 3 || k[1] != 0) luaL_argerror(L, 2, "'x', 'y', or 'z' expected");
  return axis;
}

int l_Vector_index(lua_State* L) {
  lua_rawgeti(L, 1, VectorAxis(L));
  return 1;
}

int l_Vector_newindex(lua_State* L) {
  int axis = VectorAxis(L);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 1, axis);
  return 0;
}

int l_Vector(lua_State* L) {
  PushVector(L, luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3));
  return 1;
}

int l_Vector2(lua_State* L) {
  float x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
  lua_newtable(L);
  lua_pushnumber(L, x);
  lua_rawseti(L, -2, 1);
  lua_pushnumber(L, y);
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, kVectorMeta);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_setmetatable(L, -2);
  return 1;
}

int l_Rect(lua_State* L) {
  static const char* keys[] = {"x0", "y0", "x1", "y1"};
  lua_newtable(L);
  for (int i = 0; i < 4; ++i) {
    lua_pushstring(L, keys[i]);
    lua_pushnumber(L, luaL_checknumber(L, i + 1));
    lua_rawset(L, -3);
  }
  return 1;
}

int l_VDist2(lua_State* L) {
  float dx = luaL_checknumber(L, 1) - luaL_checknumber(L, 3);
  float dy = luaL_checknumber(L, 2) - luaL_checknumber(L, 4);
  lua_pushnumber(L, std::sqrt(dx * dx + dy * dy));
  return 1;
}
int l_VDist2Sq(lua_State* L) {
  float dx = luaL_checknumber(L, 1) - luaL_checknumber(L, 3);
  float dy = luaL_checknumber(L, 2) - luaL_checknumber(L, 4);
  lua_pushnumber(L, dx * dx + dy * dy);
  return 1;
}
int l_VDist3Sq(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  float dx = VecComp(L, 1, 1) - VecComp(L, 2, 1), dy = VecComp(L, 1, 2) - VecComp(L, 2, 2),
        dz = VecComp(L, 1, 3) - VecComp(L, 2, 3);
  lua_pushnumber(L, dx * dx + dy * dy + dz * dz);
  return 1;
}
int l_VDist3(lua_State* L) {
  l_VDist3Sq(L);
  lua_pushnumber(L, std::sqrt(lua_tonumber(L, -1)));
  return 1;
}
int l_VDot(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_pushnumber(L, VecComp(L, 1, 1) * VecComp(L, 2, 1) + VecComp(L, 1, 2) * VecComp(L, 2, 2) +
                        VecComp(L, 1, 3) * VecComp(L, 2, 3));
  return 1;
}
int l_VAdd(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  PushVector(L, VecComp(L, 1, 1) + VecComp(L, 2, 1), VecComp(L, 1, 2) + VecComp(L, 2, 2),
             VecComp(L, 1, 3) + VecComp(L, 2, 3));
  return 1;
}
int l_VDiff(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  PushVector(L, VecComp(L, 1, 1) - VecComp(L, 2, 1), VecComp(L, 1, 2) - VecComp(L, 2, 2),
             VecComp(L, 1, 3) - VecComp(L, 2, 3));
  return 1;
}
int l_VMult(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  float s = luaL_checknumber(L, 2);
  PushVector(L, VecComp(L, 1, 1) * s, VecComp(L, 1, 2) * s, VecComp(L, 1, 3) * s);
  return 1;
}
int l_VPerpDot(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_pushnumber(L, VecComp(L, 1, 1) * VecComp(L, 2, 3) - VecComp(L, 1, 3) * VecComp(L, 2, 1));
  return 1;
}

// Quaternions are {x, y, z, w} tables sharing the vector metatable (FAF's utils.lua adds the
// arithmetic on it).
void PushQuat4(lua_State* L, float x, float y, float z, float w) {
  lua_newtable(L);
  float v[4] = {x, y, z, w};
  for (int i = 0; i < 4; ++i) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_pushstring(L, kVectorMeta);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_setmetatable(L, -2);
}

// EulerToQuaternion(roll, pitch, yaw) = qYaw(Y) * qPitch(X) * qRoll(Z); matches the original to
// the last bit (oracle probe 2026-10-08).
int l_EulerToQuaternion(lua_State* L) {
  float roll = luaL_checknumber(L, 1), pitch = luaL_checknumber(L, 2), yaw = luaL_checknumber(L, 3);
  float cy = dmath::Cos(yaw * 0.5f), sy = dmath::Sin(yaw * 0.5f);
  float cp = dmath::Cos(pitch * 0.5f), sp = dmath::Sin(pitch * 0.5f);
  float cr = dmath::Cos(roll * 0.5f), sr = dmath::Sin(roll * 0.5f);
  // qy * qp * qr with qy=(0,sy,0,cy), qp=(sp,0,0,cp), qr=(0,0,sr,cr)
  float x = cy * sp * cr + sy * cp * sr;
  float y = sy * cp * cr - cy * sp * sr;
  float z = cy * cp * sr - sy * sp * cr;
  float w = cy * cp * cr + sy * sp * sr;
  PushQuat4(L, x, y, z, w);
  return 1;
}

// OrientFromDir(v): the orientation whose forward (+Z) axis points along v.
int l_OrientFromDir(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  float x = VecComp(L, 1, 1), y = VecComp(L, 1, 2), z = VecComp(L, 1, 3);
  float len = std::sqrt(x * x + y * y + z * z);
  if (len <= 0) {
    PushQuat4(L, 0, 0, 0, 1);
    return 1;
  }
  float heading = dmath::Atan2(x, z);
  float pitch = -std::asin(y / len);
  float ch = dmath::Cos(heading * 0.5f), sh = dmath::Sin(heading * 0.5f);
  float cp = dmath::Cos(pitch * 0.5f), sp = dmath::Sin(pitch * 0.5f);
  // probe: equal to the original within 1 ulp; + 0.0f turns -0 into 0 as the original prints it
  PushQuat4(L, ch * sp + 0.0f, sh * cp + 0.0f, -sh * sp + 0.0f, ch * cp);
  return 1;
}

int l_SecondsPerTick(lua_State* L) {
  lua_pushnumber(L, 0.1f);
  return 1;
}

int l_Noop(lua_State*) { return 0; }

int l_ReturnFirstArg(lua_State* L) {
  lua_settop(L, 1);
  return 1;
}

// Sound { Bank =, Cue =, LodCutoff = }: a sound parameters object. Blueprints are read in the rules
// state as plain tables; the table is remembered (registry "moho64.sounds", weak keys) so the copy
// of the blueprints in the sim becomes a sound object there (see sim/blueprints.cpp).
int l_Sound(lua_State* L) {
  lua_settop(L, 1);
  if (lua_istable(L, 1)) {
    lua_pushstring(L, "moho64.sounds");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_newtable(L);  // metatable: weak keys
      lua_pushstring(L, "__mode");
      lua_pushstring(L, "k");
      lua_rawset(L, -3);
      lua_setmetatable(L, -2);
      lua_pushstring(L, "moho64.sounds");
      lua_pushvalue(L, -2);
      lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_pushvalue(L, 1);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  return 1;
}

int l_EnumColorNames(lua_State* L) {
  lua_newtable(L);
  return 1;
}

int l_IsDestroyed(lua_State* L) {
  lua_pushboolean(L, lua_isnil(L, 1));
  return 1;
}

int l_BlueprintLoaderUpdateProgress(lua_State* L) {
  ScriptState* st = ScriptState::From(L);
  if (st->onLoaderProgress) st->onLoaderProgress();
  return 0;
}

template <const char* Kind>
int l_RegisterBlueprint(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  ScriptState* st = ScriptState::From(L);
  if (st->onRegisterBlueprint) st->onRegisterBlueprint(L, Kind);
  return 0;
}

constexpr char kUnit[] = "Unit";
constexpr char kProp[] = "Prop";
constexpr char kProjectile[] = "Projectile";
constexpr char kMesh[] = "Mesh";
constexpr char kTrailEmitter[] = "TrailEmitter";
constexpr char kEmitter[] = "Emitter";
constexpr char kBeam[] = "Beam";
constexpr char kFootprints[] = "Footprints";

// io.dir(pattern) -- LuaPlus extension: names matching a Win32 wildcard pattern.
int l_io_dir(lua_State* L) {
  const char* pattern = luaL_checkstring(L, 1);
  auto names = hostfs::Glob(pattern);
  lua_newtable(L);
  int i = 1;
  for (const auto& n : names) {
    lua_pushstring(L, n.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

void Reg(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushstring(L, name);
  lua_pushcfunction(L, f);
  lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace

// The vector metatable as a registry reference (an array slot: no string hashing per vector).
static int VectorMetaRef(lua_State* L) {
  ScriptState* st = ScriptState::From(L);
  if (st && st->vectorMetaRef != LUA_NOREF) return st->vectorMetaRef;
  lua_pushstring(L, kVectorMeta);
  lua_rawget(L, LUA_REGISTRYINDEX);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (st) st->vectorMetaRef = ref;
  return ref;
}

void PushVector(lua_State* L, float x, float y, float z) {
  lua_newtablesized(L, 3, 0);
  lua_pushnumber(L, x);
  lua_rawseti(L, -2, 1);
  lua_pushnumber(L, y);
  lua_rawseti(L, -2, 2);
  lua_pushnumber(L, z);
  lua_rawseti(L, -2, 3);
  lua_rawgeti(L, LUA_REGISTRYINDEX, VectorMetaRef(L));
  lua_setmetatable(L, -2);
}


void RegisterCoreBindings(ScriptState& state) {
  lua_State* L = state.L();
  // Vector metatable
  lua_pushstring(L, kVectorMeta);
  lua_newtable(L);
  lua_pushstring(L, "__index");
  lua_pushcfunction(L, l_Vector_index);
  lua_rawset(L, -3);
  lua_pushstring(L, "__newindex");
  lua_pushcfunction(L, l_Vector_newindex);
  lua_rawset(L, -3);
  lua_rawset(L, LUA_REGISTRYINDEX);

  Reg(L, "LOG", l_LOG);
  Reg(L, "WARN", l_WARN);
  Reg(L, "SPEW", l_SPEW);
  Reg(L, "_ALERT", l_LOG);
  Reg(L, "print", l_LOG);
  Reg(L, "doscript", l_doscript);
  Reg(L, "DiskFindFiles", l_DiskFindFiles);
  Reg(L, "DiskGetFileInfo", l_DiskGetFileInfo);
  Reg(L, "DiskToLocal", l_DiskToLocal);
  Reg(L, "Basename", l_Basename);
  Reg(L, "Dirname", l_Dirname);
  Reg(L, "FileCollapsePath", l_FileCollapsePath);
  Reg(L, "exists", l_exists);
  Reg(L, "STR_xtoi", l_STR_xtoi);
  Reg(L, "STR_itox", l_STR_itox);
  Reg(L, "STR_Utf8Len", l_STR_Utf8Len);
  Reg(L, "STR_Utf8SubString", l_STR_Utf8SubString);
  Reg(L, "STR_GetTokens", l_STR_GetTokens);
  Reg(L, "GetVersion", l_GetVersion);
  Reg(L, "MATH_IRound", l_MATH_IRound);
  Reg(L, "MATH_Lerp", l_MATH_Lerp);
  Reg(L, "Vector", l_Vector);
  Reg(L, "Vector2", l_Vector2);
  Reg(L, "Rect", l_Rect);
  Reg(L, "VDist2", l_VDist2);
  Reg(L, "VDist2Sq", l_VDist2Sq);
  Reg(L, "VDist3", l_VDist3);
  Reg(L, "VDist3Sq", l_VDist3Sq);
  Reg(L, "VDot", l_VDot);
  Reg(L, "VAdd", l_VAdd);
  Reg(L, "VDiff", l_VDiff);
  Reg(L, "VMult", l_VMult);
  Reg(L, "VPerpDot", l_VPerpDot);
  Reg(L, "SecondsPerTick", l_SecondsPerTick);
  Reg(L, "EulerToQuaternion", l_EulerToQuaternion);
  Reg(L, "OrientFromDir", l_OrientFromDir);
  Reg(L, "Trace", l_Noop);
  Reg(L, "BeginLoggingStats", l_Noop);
  Reg(L, "EndLoggingStats", l_Noop);
  Reg(L, "IsDestroyed", l_IsDestroyed);
  Reg(L, "EnumColorNames", l_EnumColorNames);
  Reg(L, "Sound", l_Sound);
  Reg(L, "RPCSound", l_ReturnFirstArg);
  Reg(L, "BlueprintLoaderUpdateProgress", l_BlueprintLoaderUpdateProgress);
  Reg(L, "RegisterUnitBlueprint", l_RegisterBlueprint<kUnit>);
  Reg(L, "RegisterPropBlueprint", l_RegisterBlueprint<kProp>);
  Reg(L, "RegisterProjectileBlueprint", l_RegisterBlueprint<kProjectile>);
  Reg(L, "RegisterMeshBlueprint", l_RegisterBlueprint<kMesh>);
  Reg(L, "RegisterTrailEmitterBlueprint", l_RegisterBlueprint<kTrailEmitter>);
  Reg(L, "RegisterEmitterBlueprint", l_RegisterBlueprint<kEmitter>);
  Reg(L, "RegisterBeamBlueprint", l_RegisterBlueprint<kBeam>);
  Reg(L, "SpecFootprints", l_RegisterBlueprint<kFootprints>);

  // io.dir
  lua_getglobal(L, "io");
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "dir");
    lua_pushcfunction(L, l_io_dir);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}

}  // namespace moho
