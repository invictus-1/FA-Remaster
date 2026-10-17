// Preferences: GetPreference(key, default), SetPreference(key, value), SavePreferences().
//
// The original keeps the preferences on the engine side and hands scripts copies: a key is a
// dotted path ("profile.profiles.1.options"), a value is a number, string, boolean or a table of
// those. Its file (Game.prefs) is a Lua file of top-level assignments. We keep that form but never
// read or write the original game's file: ours is the file the host names, or memory only.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

const char* kPrefsKey = "moho64ui.prefs";

void PushPrefs(lua_State* L) {
  lua_pushstring(L, kPrefsKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushstring(L, kPrefsKey);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

// Push a copy of the value at idx: tables are copied through, everything else is pushed as it is.
void PushCopy(lua_State* L, int idx, int depth = 0) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  if (!lua_istable(L, idx) || depth > 32) {
    lua_pushvalue(L, idx);
    return;
  }
  lua_newtable(L);
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    lua_pushvalue(L, -2);       // key
    PushCopy(L, -2, depth + 1);  // value copy
    lua_rawset(L, -5);
    lua_pop(L, 1);
  }
}

std::vector<std::string> SplitKey(const std::string& key) {
  std::vector<std::string> parts;
  size_t a = 0;
  for (;;) {
    size_t b = key.find('.', a);
    parts.push_back(key.substr(a, b == std::string::npos ? b : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return parts;
}

// A path segment made of digits names an array slot ("profile.profiles.1").
void PushSegment(lua_State* L, const std::string& s) {
  if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos)
    lua_pushnumber(L, static_cast<lua_Number>(std::atoi(s.c_str())));
  else
    lua_pushstring(L, s.c_str());
}

int l_GetPreference(lua_State* L) {
  std::string key = luaL_checkstring(L, 1);
  lua_settop(L, 2);
  PushPrefs(L);
  for (const auto& seg : SplitKey(key)) {
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      break;
    }
    PushSegment(L, seg);
    lua_rawget(L, -2);
    lua_remove(L, -2);
  }
  if (lua_isnil(L, -1)) {
    lua_pushvalue(L, 2);  // the default
    return 1;
  }
  PushCopy(L, -1);
  return 1;
}

// GetOptions() (FA exe 0x8c9490): the current profile's options table; nil without a profile.
// GetOptions(key): one option of it. Not in the original: FAF's patched game takes the key, and
// its preferences script asks every option this way (falling back to the option's default when
// the answer is nil).
int l_GetOptions(lua_State* L) {
  bool one = lua_isstring(L, 1) != 0;
  std::string key = one ? lua_tostring(L, 1) : "";
  lua_settop(L, 0);
  PushPrefs(L);
  // profile.current names the profile: an index into profile.profiles
  lua_pushstring(L, "profile");
  lua_rawget(L, -2);
  if (!lua_istable(L, -1)) return 0;
  lua_pushstring(L, "current");
  lua_rawget(L, -2);
  lua_pushstring(L, "profiles");
  lua_rawget(L, -3);
  if (!lua_istable(L, -1) || lua_isnil(L, -2)) return 0;
  lua_pushvalue(L, -2);
  lua_rawget(L, -2);
  if (!lua_istable(L, -1)) return 0;
  lua_pushstring(L, "options");
  lua_rawget(L, -2);
  if (!lua_istable(L, -1)) return 0;
  if (!one) {
    PushCopy(L, lua_gettop(L));
    return 1;
  }
  lua_pushstring(L, key.c_str());
  lua_rawget(L, -2);
  if (lua_isnil(L, -1)) return 0;
  PushCopy(L, lua_gettop(L));
  return 1;
}

int l_SetPreference(lua_State* L) {
  std::string key = luaL_checkstring(L, 1);
  lua_settop(L, 2);
  auto parts = SplitKey(key);
  PushPrefs(L);
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    PushSegment(L, parts[i]);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {  // make the tables on the way
      lua_pop(L, 1);
      lua_newtable(L);
      PushSegment(L, parts[i]);
      lua_pushvalue(L, -2);
      lua_rawset(L, -4);
    }
    lua_remove(L, -2);
  }
  PushSegment(L, parts.back());
  PushCopy(L, 2);
  lua_rawset(L, -3);
  return 0;
}

// --- writing the file ---

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\0') out += "\\0";
    else out += c;
  }
  return out + "\"";
}

std::string Number(lua_Number v) {
  char buf[40];
  if (v == std::floor(v) && std::fabs(v) < 1e9) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(v));
  else std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
  return buf;
}

bool IsIdentifier(const std::string& s) {
  static const char* const kWords[] = {"and", "break", "continue", "do", "else", "elseif", "end", "false", "for",
                                       "function", "if", "in", "local", "nil", "not", "or", "repeat", "return",
                                       "then", "true", "until", "while"};
  if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  for (const char* w : kWords)
    if (s == w) return false;
  return true;
}

void WriteValue(lua_State* L, int idx, int indent, std::string& out);

// The table at idx: array slots first, then the other keys in a fixed order, so the file is stable.
void WriteFields(lua_State* L, int idx, int indent, bool topLevel, std::string& out) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  std::string pad(static_cast<size_t>(indent) * 4, ' ');
  int n = topLevel ? 0 : luaL_getn(L, idx);
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, idx, i);
    out += pad;
    WriteValue(L, -1, indent, out);
    out += ",\n";
    lua_pop(L, 1);
  }
  struct Key {
    bool isNumber;
    lua_Number num;
    std::string str;
  };
  std::vector<Key> keys;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    lua_pop(L, 1);
    if (lua_type(L, -1) == LUA_TNUMBER) {
      lua_Number k = lua_tonumber(L, -1);
      if (!(k == std::floor(k) && k >= 1 && k <= n)) keys.push_back({true, k, ""});
    } else if (lua_type(L, -1) == LUA_TSTRING) {
      keys.push_back({false, 0, lua_tostring(L, -1)});
    }
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    if (a.isNumber != b.isNumber) return a.isNumber;
    return a.isNumber ? a.num < b.num : a.str < b.str;
  });
  for (const auto& k : keys) {
    if (k.isNumber) lua_pushnumber(L, k.num);
    else lua_pushstring(L, k.str.c_str());
    lua_rawget(L, idx);
    int t = lua_type(L, -1);
    if (t == LUA_TNUMBER || t == LUA_TSTRING || t == LUA_TBOOLEAN || t == LUA_TTABLE) {
      out += pad;
      if (k.isNumber) out += "[" + Number(k.num) + "]";
      else if (IsIdentifier(k.str)) out += k.str;
      else if (topLevel) out += "_G[" + Quote(k.str) + "]";
      else out += "[" + Quote(k.str) + "]";
      out += " = ";
      WriteValue(L, -1, indent, out);
      out += topLevel ? "\n" : ",\n";
    }
    lua_pop(L, 1);
  }
}

void WriteValue(lua_State* L, int idx, int indent, std::string& out) {
  switch (lua_type(L, idx)) {
    case LUA_TNUMBER: out += Number(lua_tonumber(L, idx)); break;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
    case LUA_TSTRING: out += Quote(std::string(lua_tostring(L, idx), lua_strlen(L, idx))); break;
    case LUA_TTABLE:
      if (indent > 32) {
        out += "{}";
        break;
      }
      out += "{\n";
      WriteFields(L, idx, indent + 1, false, out);
      out += std::string(static_cast<size_t>(indent) * 4, ' ') + "}";
      break;
    default: out += "nil"; break;
  }
}

int l_SavePreferences(lua_State* L) {
  SavePreferencesOf(L);
  return 0;
}

}  // namespace

void SavePreferencesOf(lua_State* L) {
  UserState* us = UserState::From(L);
  const std::string& file = us->config().prefsFile;
  if (file.empty()) {
    static bool said = false;
    if (!said) Logf(LogLevel::Debug, "moho64ui: SavePreferences: no preferences file, kept in memory");
    said = true;
    return;
  }
  std::string out;
  PushPrefs(L);
  WriteFields(L, -1, 0, true, out);
  lua_pop(L, 1);
  if (!hostfs::WriteFile(file, out)) Logf(LogLevel::Warning, "moho64ui: cannot write preferences to %s", file.c_str());
}

// The file is a chunk of top-level assignments: run it with a new table as its environment, and
// that table is the preferences. The chunk can reach nothing else (no globals).
void LoadPrefs(lua_State* L, const std::string& file) {
  auto src = hostfs::ReadFile(file);
  if (!src) {
    Logf(LogLevel::Info, "moho64ui: no preferences file yet (%s)", file.c_str());
    return;
  }
  int top = lua_gettop(L);
  if (luaL_loadbuffer(L, src->data(), src->size(), ("@" + file).c_str()) != 0) {
    Logf(LogLevel::Warning, "moho64ui: preferences not read: %s", lua_tostring(L, -1));
    lua_settop(L, top);
    return;
  }
  lua_newtable(L);
  lua_pushstring(L, "_G");  // keys that are not identifiers are written as _G["key"]
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);
  lua_pushvalue(L, -1);
  lua_setfenv(L, -3);
  lua_insert(L, -2);  // env, chunk
  if (lua_pcall(L, 0, 0, 0) != 0) {
    Logf(LogLevel::Warning, "moho64ui: preferences not read: %s", lua_tostring(L, -1));
    lua_settop(L, top);
    return;
  }
  lua_pushstring(L, "_G");
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pushstring(L, kPrefsKey);
  lua_insert(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_settop(L, top);
}

void RegisterPrefsBindings(lua_State* L) {
  SetGlobal(L, "GetPreference", l_GetPreference);
  SetGlobal(L, "SetPreference", l_SetPreference);
  SetGlobal(L, "GetOptions", l_GetOptions);
  SetGlobal(L, "SavePreferences", l_SavePreferences);
}

}  // namespace moho::ui
