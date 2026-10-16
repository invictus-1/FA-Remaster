// Engine functions present in every Lua state (the original's "core" set). Behaviour follows
// the original engine; see docs/lua-dialect.md and the binding list for the reference names.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
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
  // The lab's comparison probe (a script in the lab's overlay folder, not part of the game) is switched on for
  // the ORIGINAL by flag files there. This engine goes by its own environment instead (MOHO64_PROBE = watch or
  // on), never by those files: a flag set for another run (an oracle run, a watch run) must not switch the probe
  // on in a game somebody starts meanwhile.
  if (std::strncmp(name, "/moho64_probe_", 14) == 0) {
    const char* mode = getenv("MOHO64_PROBE");
    const std::string what = name + 14;
    const bool watch = mode && std::strcmp(mode, "watch") == 0;
    const bool on = mode && (watch || std::strcmp(mode, "on") == 0);
    if ((what == "on" && on) || (what == "watch" && watch)) {
      lua_newtable(L);
      lua_pushstring(L, "IsFolder");
      lua_pushboolean(L, 0);
      lua_rawset(L, -3);
    } else {
      lua_pushboolean(L, 0);
    }
    return 1;
  }
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

// ---- GetMovieDuration: a movie's length from its Sofdec (.sfd) header, as CRI's library in the original reads it.
// A .sfd is an MPEG-1 program stream: packs (00 00 01 BA); the Sofdec stream header ("SFH": "SofdecStream" at +0x20
// of a pack, the frame count at +0xc0, a stream table at +0x180); the video's MPEG sequence header (00 00 01 B3)
// holds the frame rate code.
namespace sfd {

constexpr size_t kNone = static_cast<size_t>(-1);
// fps * 1000 by MPEG frame rate code 1..8 (table 0xd7f8d0; the same values in getPicRate's switch 0xadd7a0).
constexpr int kFps1000[9] = {0, 23976, 24000, 25000, 29970, 30000, 50000, 59940, 60000};

// The file's bytes. The original memory-maps the file (DISK_MemoryMapFile 0x459d10) and reads a few header bytes
// without bounds checks (sfcre_AnalyMpv): a byte past the end reads as 0 here, as in the mapping's last page.
struct Bytes {
  const unsigned char* p;
  size_t n;
  unsigned operator[](size_t i) const { return i < n ? p[i] : 0u; }
};

// MPS_SearchDelim 0xada450 for a pack header (class 0x10000 of 0xaecc20: exactly 00 00 01 BA): the first at or
// after `from` with 4 bytes left.
size_t FindPack(const Bytes& b, size_t from) {
  for (size_t i = from; i + 4 <= b.n; ++i)
    if (b.p[i] == 0 && b.p[i + 1] == 0 && b.p[i + 2] == 1 && b.p[i + 3] == 0xBA) return i;
  return kNone;
}

// MPV_SearchDelim 0xaea040 for a sequence header (class 0x40 of 0xae9f10: 00 00 01 B3), its state machine as is:
// 00 -> 1, then 00 -> 2 (else 0), then 01 -> 3 (00 stays at 2, else 0), then the code byte; a code that is not B3
// starts again at state 0 with the NEXT byte (the code byte is not taken as a first zero).
size_t FindSequenceHeader(const Bytes& b, size_t from) {
  int state = 0;
  for (size_t i = from; i < b.n;) {
    const unsigned a = b.p[i++];
    switch (state) {
      case 0: if (a == 0) state = 1; break;
      case 1: state = a == 0 ? 2 : 0; break;
      case 2: if (a == 1) state = 3; else if (a != 0) state = 0; break;
      default:
        if (a == 0xB3) return i - 4;
        state = 0;
    }
  }
  return kNone;
}

// sfcre_AnalyMpv 0xada880 over the whole file: the first sequence header with an aspect ratio code that is not 0
// (byte 7 & 0xf0), a frame rate code 1..8 (byte 7 & 0x0f) and the marker bit (byte 10 & 0x20) gives fps * 1000; a
// header failing that test is skipped and the search goes on from its second byte. 0: none.
int MpvFps1000(const Bytes& b) {
  size_t from = 0;
  while (from < b.n) {
    const size_t at = FindSequenceHeader(b, from);
    if (at == kNone) return 0;
    from = at + 1;
    const unsigned code = b[at + 7] & 0xF;
    if ((b[at + 7] & 0xF0) && code >= 1 && code <= 8 && (b[at + 10] & 0x20)) return kFps1000[code];
  }
  return 0;
}

// SFH_IsSfdHeader 0xadc890: at least 0x800 bytes from `at` and "SofdecStream" + 12 spaces at +0x20 (0xf41a80); the
// version (SFH_AnlyHdrToolVer 0xadcb70) is the larger of the bytes +0x38/+0x39 (major * 100 + minor) and
// "Ver.<major>.<minor>" in the 32 bytes at +0x60 (getToolVer 0xadcd80: strstr, convAsciiToDigit 0xadcdc0); without
// "Ver." there it is no header. -1: no header.
int SfhVersion(const Bytes& b, size_t at) {
  if (at >= b.n || b.n - at < 0x800) return -1;
  const unsigned char* h = b.p + at;
  if (std::memcmp(h + 0x20, "SofdecStream            ", 24) != 0) return -1;
  char text[34] = {};  // 32 bytes and the terminator (0xadcb30); one more 0 for a minor read past "Ver.1"
  std::memcpy(text, h + 0x60, 32);
  const char* c = std::strstr(text, "Ver.");
  if (!c) return -1;
  auto digits = [](const char*& s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
  };
  c += 4;
  const int major = digits(c);
  ++c;  // past the character that ended the major number, whichever it is
  const int minor = digits(c);
  const int text_version = major * 100 + minor;
  const int byte_version = h[0x38] * 100 + h[0x39];
  return byte_version >= text_version ? byte_version : text_version;
}

// The version test of every SFH field read (isEffectiveVer 0xadc980): 107, or 110 and later.
bool SfhVersionOk(int v) { return v == 107 || v >= 110; }

// MWSFFRM_AnalyzeSofdecHeader 0xaca8e0: the header is looked for at +0x800, then at +0x1000 only (a file under
// 0x800 bytes: none); the first found gives the frame count, the int at +0xc0 (SFH_AnlyMaxFrmNum 0xadcfd0) when the
// version passes, else -1. No header: -1.
int32_t SfhFrames(const Bytes& b) {
  if (b.n < 0x800) return -1;
  for (size_t at : {size_t{0x800}, size_t{0x1000}}) {
    const int v = SfhVersion(b, at);
    if (v < 0) continue;
    if (!SfhVersionOk(v)) return -1;
    int32_t frames;
    std::memcpy(&frames, b.p + at + 0xc0, 4);
    return frames;
  }
  return -1;
}

// sfcre_AnalySfh 0xada2b0 (before sfcre_AnalyMpv, which overwrites what it finds): the header at 0, then 1, 2 and 3
// packs further while bytes are left; when its version passes, the first video stream in its table (ids 0xE0..0xEF
// in order, sfhds_AnlyUsedStmid 0xae7640; 26 entries of 0x40 bytes at +0x180, the id at +0x18, searchStmId
// 0xadca60) gives fps * 1000 by the code at +0x1f of its entry (0xadd430 -> getPicRate 0xadd7a0). 0: none.
int SfhFps1000(const Bytes& b, size_t pack) {
  size_t at = 0;
  int v = SfhVersion(b, at);
  for (int tries = 0; v < 0 && tries < 3; ++tries) {
    at += pack;
    if (at >= b.n) return 0;
    v = SfhVersion(b, at);
  }
  if (v < 0 || !SfhVersionOk(v)) return 0;
  const unsigned char* table = b.p + at + 0x180;
  for (unsigned id = 0xE0; id <= 0xEF; ++id)
    for (int k = 0; k < 26; ++k)
      if (table[0x40 * k + 0x18] == id) {
        const unsigned code = table[0x40 * k + 0x1f];
        return code >= 1 && code <= 8 ? kFps1000[code] : 0;
      }
  return 0;
}

// mwPlyGetHdrInf 0xac8df0 -> SFD_AnalyCreInf 0xad9fc0 as MOV_GetDuration needs it: false when the original finds no
// program stream with video. sfcre_AnalyMps 0xada110 / sfcre_AnalyPackSiz 0xada190: three pack headers, equally
// spaced, the first at a multiple of that spacing from the start (fewer than three: not a program stream; unequal:
// not valid). Then the video: the SFH's frame rate, overwritten by the sequence header's (above).
// Not built: the MPEG-2 transport stream branch (sfcre_AnalyM2ts 0xada0c0, tried first: 0x47 sync bytes 188 bytes
// apart in the first 0x758 bytes; none of the game's 1004 movies has three), and the audio analysis
// (sfcre_AnalyAudio 0xada3a0), which decides validity only for a program stream WITHOUT video: there the original
// goes on to divide by a frame rate of 0 when the stream has ADX or MPEG audio. Such a file reads as "not a valid
// SFD file" (0) here; every one of the game's movies has video.
bool Analyze(const Bytes& b, int32_t& frames, int& fps1000) {
  const size_t p1 = FindPack(b, 0);
  const size_t p2 = p1 == kNone ? kNone : FindPack(b, p1 + 1);
  const size_t p3 = p2 == kNone ? kNone : FindPack(b, p2 + 1);
  if (p3 == kNone) return false;
  const size_t pack = p2 - p1;
  if (p3 - p2 != pack || p1 % pack != 0) return false;
  fps1000 = SfhFps1000(b, pack);
  if (const int mpv = MpvFps1000(b)) fps1000 = mpv;
  if (fps1000 == 0) return false;
  frames = SfhFrames(b);  // MWSFFRM_AnalyzeSofdecHeader, called by mwPlyGetHdrInf after SFD_AnalyCreInf
  return true;
}

}  // namespace sfd

// GetMovieDuration(localFileName) (FA exe 0x8753a0 -> 0x875420 -> Moho::MOV_GetDuration 0x874d30), in the core set
// (0xf59680): every state has it. FAF's ScenarioFramework (Dialogue) calls it in the sim and plays AllyCom.sfd
// instead when it returns 0.
// 0x875420: lua_gettop must be 1 ("%s\n  expected %d args, but got %d", 0xe0a220, with the help 0xe49538);
// lua_tostring(1), else TypeError "string"; the float result pushed with lua_pushnumber.
// 0x874d30: no movie manager (made by CScApp::CreateDevice 0x8d0ea7, so always there in a game): 0. The VFS's
// FindFile (CVFSImpl 0x466a20: the path lower-cased, the first search path that has it) finds no file: Warnf
// "Movie file \"%s\" doesn't exist." (0xe49428) and 0. The file is memory-mapped whole; no bytes: 0. The header
// read (mwPlyGetHdrInf 0xac8df0) must give a program stream with video (its type 1 or 3), else Warnf "%s is not a
// valid SFD file." (0xe49244) and 0. Then, in floats (0x874ec4, SSE):
//   frames / (fps1000 * 0.001f)        (cvtsi2ss both, mulss by 0.001f at 0xdff0ac, divss)
// e.g. 210 frames at 30000 -> 6.9999995 (FMV_loading02.sfd), not 7: 30000 * 0.001f rounds to 30.000002. A file
// whose header gives no frame count divides -1.
int l_GetMovieDuration(lua_State* L) {
  const int n = lua_gettop(L);
  if (n != 1) luaL_error(L, "%s\n  expected %d args, but got %d", "GetMovieDuration(localFileName)", 1, n);
  const char* name = lua_tostring(L, 1);
  if (!name) luaL_typerror(L, 1, "string");
  float seconds = 0.0f;
  const std::optional<std::string> data = GetVfs(L)->ReadFile(name);
  if (!data) {
    Logf(LogLevel::Warning, "Movie file \"%s\" doesn't exist.", name);
  } else if (!data->empty()) {
    const sfd::Bytes b{reinterpret_cast<const unsigned char*>(data->data()), data->size()};
    int32_t frames = 0;
    int fps1000 = 0;
    if (sfd::Analyze(b, frames, fps1000))
      seconds = static_cast<float>(frames) / (static_cast<float>(fps1000) * 0.001f);
    else
      Logf(LogLevel::Warning, "%s is not a valid SFD file.", name);
  }
  lua_pushnumber(L, seconds);
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
  // (0x4d1776: summed z, y, then x; each product and sum rounded to float)
  const float zz = VecComp(L, 1, 3) * VecComp(L, 2, 3), yy = VecComp(L, 1, 2) * VecComp(L, 2, 2),
              xx = VecComp(L, 1, 1) * VecComp(L, 2, 1);
  lua_pushnumber(L, (zz + yy) + xx);
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
  float pitch = -dmath::Asin(y / len);
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

// The original's colour table (FA exe 0xdff690), in its order: see color_names.inc.
struct ColorName {
  const char* name;
  uint32_t argb;  // 0xAARRGGBB
};
constexpr ColorName kColorNames[] = {
#include "script/color_names.inc"
};
static_assert(sizeof(kColorNames) / sizeof(kColorNames[0]) == 141);

// EnumColorNames() (FA exe 0x4b2f60, core set: every state, also the rules and the session-load one): with any
// argument the usual "expected 0 args" error (lua_gettop 0x90c590, LuaState::Error 0x90c1d0 with 0xe0a220 and the
// binding's help); else a new table (0 array / 0 hash slots, 0x909940) that gets, for each entry of the colour
// table until the null name, t[i] = name with i counted from 1 (LuaObject::SetString(int, const char*) 0x9084e0,
// a number key and a string value). So {"AliceBlue", ..., "transparent"}, 141 strings; the colours are not in it.
int l_EnumColorNames(lua_State* L) {
  if (lua_gettop(L) != 0)
    luaL_error(L, "%s\n  expected %d args, but got %d",
               "table EnumColorNames() - returns a table containing strings of all the color names", 0, lua_gettop(L));
  lua_newtable(L);
  int i = 1;
  for (const ColorName& c : kColorNames) {
    lua_pushstring(L, c.name);
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// IsDestroyed(obj) (FA exe 0x4c7b10): true unless obj (or its _c_object) is the handle of a live
// engine object. Nil, plain tables and other values count as destroyed.
int l_IsDestroyed(lua_State* L) {
  bool destroyed = true;
  if (lua_istable(L, 1)) lua_rawgetcobject(L, 1);
  else lua_pushvalue(L, 1);
  if (lua_type(L, -1) == LUA_TUSERDATA && lua_getmetatable(L, -1)) {
    luaL_getmetatable(L, "moho64.cobject");  // the handle's metatable (script_handle.cpp)
    if (lua_rawequal(L, -1, -2)) {
      void** box = static_cast<void**>(lua_touserdata(L, -3));
      destroyed = !box || !*box;
    }
    lua_pop(L, 2);
  }
  lua_pop(L, 1);
  lua_pushboolean(L, destroyed);
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
  Reg(L, "GetMovieDuration", l_GetMovieDuration);
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

// For the Lua math library (third_party/lua/lmathlib.c): the functions whose results the original's are not the
// correctly rounded ones. Its library (0x919a80...) is compiled to x87 instructions and runs with the precision
// control at 24 bits, where +, -, * and the square root round to float and only the transcendental instructions stay
// wide:
// - asin, acos (0x919b10, 0x919b40 -> _CIasin 0xa8de20, _CIacos 0xa8dcd0): the x87 branch, see dmath.h.
// - exp (0x919d50): x * log2(e) rounded to float, split at the nearest integer, f2xm1, then 1 + that rounded to
//   float again, fscale.
// sin, cos, tan, atan, atan2, log and log10 are one wide instruction each, stored as a float: the correctly
// rounded value. pow goes to the library's double version (0xa8e0e0 -> 0xaa93c0 with exceptions masked), sqrt is
// fsqrt, mod is fprem, deg and rad multiply by a float constant.
extern "C" {
float moho_lua_acos(float x) { return moho::dmath::Acos(x); }
float moho_lua_asin(float x) { return moho::dmath::Asin(x); }
float moho_lua_exp(float x) {
  if (!(x == x)) return x;
  const float t = static_cast<float>(static_cast<double>(x) * 1.4426950408889634074);
  if (t > 200.0f) return HUGE_VALF;
  if (t < -200.0f) return 0.0f;
  const float n = std::nearbyint(t);
  const float f = t - n;
  const float g = static_cast<float>(1.0 + (std::exp2(static_cast<double>(f)) - 1.0));
  return std::ldexp(g, static_cast<int>(n));
}
}
