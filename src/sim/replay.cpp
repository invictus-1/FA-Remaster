#include "sim/replay.h"

#include <cstring>

namespace moho {
namespace {

struct Reader {
  const std::string& d;
  size_t p = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (p + n > d.size()) ok = false;
    return ok;
  }
  std::string CStr() {
    size_t e = d.find('\0', p);
    if (e == std::string::npos) {
      ok = false;
      return {};
    }
    std::string s = d.substr(p, e - p);
    p = e + 1;
    return s;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    uint32_t v;
    std::memcpy(&v, d.data() + p, 4);
    p += 4;
    return v;
  }
  uint8_t U8() {
    if (!Need(1)) return 0;
    return static_cast<uint8_t>(d[p++]);
  }
  std::string Bytes(size_t n) {
    if (!Need(n)) return {};
    std::string s = d.substr(p, n);
    p += n;
    return s;
  }
};

bool PushValue(lua_State* L, Reader& r, int depth) {
  if (depth > 64) return false;
  uint8_t t = r.U8();
  if (!r.ok) return false;
  switch (t) {
    case 0: {
      if (!r.Need(4)) return false;
      float f;
      std::memcpy(&f, r.d.data() + r.p, 4);
      r.p += 4;
      lua_pushnumber(L, f);
      return true;
    }
    case 1: {
      std::string s = r.CStr();
      lua_pushlstring(L, s.data(), s.size());
      return r.ok;
    }
    case 2: lua_pushnil(L); return true;
    case 3: lua_pushboolean(L, r.U8() != 0); return r.ok;
    case 4: {
      lua_newtable(L);
      while (r.ok && r.p < r.d.size() && r.d[r.p] != 5) {
        if (!PushValue(L, r, depth + 1)) return false;
        if (!PushValue(L, r, depth + 1)) return false;
        if (lua_isnil(L, -2)) lua_pop(L, 2);
        else lua_rawset(L, -3);
      }
      if (!r.Need(1)) return false;
      ++r.p;  // table end
      return true;
    }
    default: return false;
  }
}

}  // namespace

bool PushSerializedLua(lua_State* L, const std::string& blob) {
  Reader r{blob};
  int top = lua_gettop(L);
  if (!PushValue(L, r, 0)) {
    lua_settop(L, top);
    return false;
  }
  return true;
}

std::optional<ReplayHeader> ReadReplayHeader(const std::string& data, std::string* error) {
  Reader r{data};
  ReplayHeader h;
  auto fail = [&](const char* what) -> std::optional<ReplayHeader> {
    if (error) *error = std::string("replay header: ") + what + " at byte " + std::to_string(r.p);
    return std::nullopt;
  };
  h.version = r.CStr();
  if (!r.ok || h.version.rfind("Supreme Commander", 0) != 0) return fail("not a replay");
  r.CStr();  // "\r\n"
  std::string vm = r.CStr();  // "Replay v1.9\r\n/maps/x/x.scmap"
  size_t nl = vm.find("\r\n");
  if (nl == std::string::npos) return fail("bad version line");
  h.replayVersion = vm.substr(0, nl);
  h.map = vm.substr(nl + 2);
  r.CStr();  // "\r\n\x1a"
  uint32_t n = r.U32();
  h.mods = r.Bytes(n);
  n = r.U32();
  h.scenario = r.Bytes(n);
  int ns = r.U8();
  for (int i = 0; i < ns && r.ok; ++i) {
    ReplayHeader::Source s;
    s.name = r.CStr();
    s.id = r.U32();
    h.sources.push_back(s);
  }
  h.cheats = r.U8() != 0;
  int na = r.U8();
  for (int i = 0; i < na && r.ok; ++i) {
    ReplayHeader::Army a;
    n = r.U32();
    a.options = r.Bytes(n);
    a.source = r.U8();
    if (a.source != 255) r.U8();
    h.armies.push_back(a);
  }
  h.seed = r.U32();
  if (!r.ok) return fail("truncated");
  h.streamOffset = r.p;
  return h;
}

}  // namespace moho
