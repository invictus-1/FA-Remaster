#include "sim/session.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace moho {
namespace {

int AbsIndex(lua_State* L, int idx) { return idx > 0 || idx <= LUA_REGISTRYINDEX ? idx : lua_gettop(L) + idx + 1; }

// The tables being written, outermost first: a table that contains itself is written as nil (the original's
// writer, LuaObject::ToByteStream 0x4d2c80, has no such test and would not return).
struct PathStep {
  const void* table;
  std::string key;  // the key it was reached by
};
struct Path {
  std::vector<PathStep> steps;
  std::string nextKey;
  int warned = 0;
};
void WriteValue(lua_State* L, int idx, std::string& out, Path& path);

bool Writable(lua_State* L, int idx) {
  int t = lua_type(L, idx);
  return t == LUA_TNUMBER || t == LUA_TSTRING || t == LUA_TBOOLEAN || t == LUA_TTABLE;
}

void WriteValue(lua_State* L, int idx, std::string& out, Path& path) {
  idx = AbsIndex(L, idx);
  switch (lua_type(L, idx)) {
    case LUA_TNUMBER: {
      float f = static_cast<float>(lua_tonumber(L, idx));
      out.push_back(0);
      out.append(reinterpret_cast<const char*>(&f), 4);
      break;
    }
    case LUA_TSTRING:
      out.push_back(1);
      out.append(lua_tostring(L, idx));  // to the first NUL: the format is a C string
      out.push_back(0);
      break;
    case LUA_TBOOLEAN:
      out.push_back(3);
      out.push_back(lua_toboolean(L, idx) ? 1 : 0);
      break;
    case LUA_TTABLE: {
      const void* self = lua_topointer(L, idx);
      bool inside = path.steps.size() > 200 || !lua_checkstack(L, 4);
      for (const PathStep& st : path.steps) inside = inside || st.table == self;
      if (inside) {
        if (path.warned++ < 3) {
          std::string where;
          for (const PathStep& st : path.steps) where += st.key + ".";
          Logf(LogLevel::Warning, "moho64: a table that contains itself was left out of a serialized value: %s%s",
               where.c_str(), path.nextKey.c_str());
        }
        out.push_back(2);
        break;
      }
      path.steps.push_back({self, path.nextKey});
      out.push_back(4);
      lua_pushnil(L);
      while (lua_next(L, idx)) {
        if (Writable(L, -2) && Writable(L, -1)) {
          WriteValue(L, -2, out, path);
          if (lua_type(L, -1) == LUA_TTABLE) {
            if (lua_type(L, -2) == LUA_TSTRING) path.nextKey = lua_tostring(L, -2);
            else path.nextKey = std::to_string(static_cast<long long>(lua_tonumber(L, -2)));
          }
          WriteValue(L, -1, out, path);
        }
        lua_pop(L, 1);
      }
      out.push_back(5);
      path.steps.pop_back();
      break;
    }
    default:
      out.push_back(2);
  }
}

// t[k] for the table at idx, pushed.
void Field(lua_State* L, int idx, const char* k) {
  idx = AbsIndex(L, idx);
  lua_pushstring(L, k);
  lua_gettable(L, idx);
}

// A dotted path below the table at idx ("Configurations.standard.teams"), pushed; nil where a step is no table.
void PathField(lua_State* L, int idx, const char* path) {
  lua_pushvalue(L, idx);
  std::string p = path;
  size_t a = 0;
  while (true) {
    size_t dot = p.find('.', a);
    std::string key = p.substr(a, dot == std::string::npos ? std::string::npos : dot - a);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      return;
    }
    lua_pushstring(L, key.c_str());
    lua_gettable(L, -2);
    lua_remove(L, -2);
    if (dot == std::string::npos) return;
    a = dot + 1;
  }
}

bool IsString(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TSTRING; }

bool EqualNoCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
  return *a == *b;
}

// SCEN_LoadScenarioInfo 0x88bd40: a new environment, /lua/dataInit.lua and then the scenario file run in it,
// its ScenarioInfo returned (nil when the file does not exist). Pushes the result.
bool PushScenarioInfo(lua_State* L, const std::string& file, std::string* error) {
  ScriptState* st = ScriptState::From(L);
  if (!st || !st->vfs() || !st->vfs()->Exists(file)) {
    lua_pushnil(L);
    return true;
  }
  static const char code[] =
      "return function(file)\n"
      "  local env = {}\n"
      "  doscript('/lua/dataInit.lua', env)\n"
      "  doscript(file, env)\n"
      "  return env.ScenarioInfo\n"
      "end\n";
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  if (luaL_loadbuffer(L, code, sizeof code - 1, "=scenario") != 0 || lua_pcall(L, 0, 1, top + 1) != 0) {
    if (error) *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "scenario loader";
    lua_settop(L, top);
    return false;
  }
  lua_pushstring(L, file.c_str());
  if (lua_pcall(L, 1, 1, top + 1) != 0) {
    if (error) *error = std::string("scenario file ") + file + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_settop(L, top);
    return false;
  }
  lua_remove(L, top + 1);
  return true;
}

}  // namespace

void SerializeLua(lua_State* L, int idx, std::string* out) {
  Path path;
  WriteValue(L, idx, *out, path);
}

bool SessionFromGameConfig(lua_State* L, int config, const LaunchParams& params, SessionDescription* out,
                           std::string* error) {
  const int top = lua_gettop(L);
  config = AbsIndex(L, config);
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    lua_settop(L, top);
    return false;
  };
  if (!lua_istable(L, config)) return fail("the game configuration is not a table");
  *out = SessionDescription{};
  ReplayHeader& h = out->header;
  h.version = "Supreme Commander v1.50.3839";
  h.replayVersion = "Replay v1.9";
  h.seed = params.seed;

  Field(L, config, "GameOptions");
  const int options = lua_gettop(L);
  if (!lua_istable(L, options)) return fail("the game configuration has no GameOptions");
  Field(L, options, "ScenarioFile");
  if (!IsString(L, -1)) return fail("GameOptions.ScenarioFile is not set");
  const std::string scenarioFile = lua_tostring(L, -1);
  lua_pop(L, 1);

  // the scenario info, with the lobby's options as its Options
  std::string err;
  if (!PushScenarioInfo(L, scenarioFile, &err)) return fail(err);
  const int scenario = lua_gettop(L);
  if (!lua_istable(L, scenario)) return fail("NoConfig");  // LaunchFailed with an empty reason in the original
  lua_pushstring(L, "Options");
  lua_pushvalue(L, options);
  lua_settable(L, scenario);

  Field(L, config, "GameMods");
  SerializeLua(L, -1, &h.mods);
  lua_pop(L, 1);
  SerializeLua(L, scenario, &h.scenario);

  Field(L, options, "CheatsEnabled");
  h.cheats = IsString(L, -1) && std::strcmp(lua_tostring(L, -1), "true") == 0;
  lua_pop(L, 1);

  // the army names: the 'FFA' team of the scenario's standard configuration
  std::vector<std::string> armyNames;
  PathField(L, scenario, "Configurations.standard.teams");
  if (!lua_istable(L, -1)) return fail("NoConfig");
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (lua_istable(L, -1)) {
      Field(L, -1, "name");
      bool ffa = IsString(L, -1) && EqualNoCase(lua_tostring(L, -1), "FFA");
      lua_pop(L, 1);
      if (ffa) {
        Field(L, -1, "armies");
        if (lua_istable(L, -1)) {
          lua_pushnil(L);
          while (lua_next(L, -2)) {
            if (IsString(L, -1)) armyNames.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
          }
        }
        lua_pop(L, 3);  // armies, the team, its key
        break;
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  // the players: PlayerOptions[slot] is army `slot` of that list; in slot order
  struct Entry {
    int slot;
    int index;  // stack index of the options table
  };
  std::vector<Entry> armies;
  Field(L, config, "PlayerOptions");
  if (lua_istable(L, -1)) {
    const int players = lua_gettop(L);
    std::vector<int> slots;
    lua_pushnil(L);
    while (lua_next(L, players)) {
      if (lua_istable(L, -1) && lua_type(L, -2) == LUA_TNUMBER) slots.push_back(static_cast<int>(lua_tonumber(L, -2)));
      lua_pop(L, 1);
    }
    std::sort(slots.begin(), slots.end());
    if (!lua_checkstack(L, static_cast<int>(slots.size()) + 32)) return fail("too many players");
    for (int slot : slots) {
      lua_rawgeti(L, players, slot);
      if (slot >= 1 && slot <= static_cast<int>(armyNames.size())) {
        lua_pushstring(L, "ArmyName");
        lua_pushstring(L, armyNames[slot - 1].c_str());
        lua_settable(L, -3);
      }
      armies.push_back({slot - 1, lua_gettop(L)});
    }
  }
  if (armies.size() > armyNames.size()) return fail("StartSpots");
  if (armies.empty()) {  // nobody: one army named 'default'
    lua_newtable(L);
    lua_pushstring(L, "PlayerName");
    lua_pushstring(L, "default");
    lua_settable(L, -3);
    lua_pushstring(L, "ArmyName");
    lua_pushstring(L, "default");
    lua_settable(L, -3);
    lua_pushstring(L, "Human");
    lua_pushboolean(L, 0);
    lua_settable(L, -3);
    armies.push_back({0, lua_gettop(L)});
  }

  // the scenario's extra armies become civilians, after the players, unless the civilians are switched off
  Field(L, options, "CivilianAlliance");
  bool civilians = IsString(L, -1) && !EqualNoCase(lua_tostring(L, -1), "none");
  lua_pop(L, 1);
  if (civilians) {
    PathField(L, scenario, "Configurations.standard.customprops.ExtraArmies");
    std::string extra = IsString(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    size_t a = 0;
    while (a < extra.size()) {
      size_t e = extra.find(' ', a);
      if (e == std::string::npos) e = extra.size();
      if (e > a) {
        lua_newtable(L);
        lua_pushstring(L, "PlayerName");
        lua_pushstring(L, "civilian");
        lua_settable(L, -3);
        lua_pushstring(L, "ArmyName");
        lua_pushstring(L, extra.substr(a, e - a).c_str());
        lua_settable(L, -3);
        lua_pushstring(L, "Civilian");
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
        lua_pushstring(L, "Human");
        lua_pushboolean(L, 0);
        lua_settable(L, -3);
        armies.push_back({-1, lua_gettop(L)});
      }
      a = e + 1;
    }
  }

  // command sources: one per human owner, in order of first appearance (0x7c4f60); an army's set is its owner's
  int pauses = -1;
  if (params.network) {
    Field(L, options, "Timeouts");
    if (IsString(L, -1)) pauses = std::atoi(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  std::vector<std::pair<int, int>> owners;  // owner id -> source index
  auto sourceOf = [&](int owner, const char* name, int allowed) {
    for (const auto& o : owners)
      if (o.first == owner) return o.second;
    if (h.sources.size() >= 255) return 255;
    owners.push_back({owner, static_cast<int>(h.sources.size())});
    h.sources.push_back({name ? name : "", allowed});
    return owners.back().second;
  };
  int localOwner = params.localOwner;
  for (size_t i = 0; i < armies.size(); ++i) {
    const int opts = armies[i].index;
    ReplayHeader::Army army;
    Field(L, opts, "Human");
    bool human = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (human) {
      Field(L, opts, "OwnerID");
      int owner = lua_isstring(L, -1) ? std::atoi(lua_tostring(L, -1)) : 0;
      lua_pop(L, 1);
      if (localOwner < 0) localOwner = owner;
      if (owner == localOwner) out->focusArmy = static_cast<int>(i);
      Field(L, opts, "PlayerName");
      int src = sourceOf(owner, lua_isstring(L, -1) ? lua_tostring(L, -1) : "", pauses);
      lua_pop(L, 1);
      if (src != 255) army.sources.push_back(src);
    }
    army.source = army.sources.empty() ? 255 : army.sources.front();
    SerializeLua(L, opts, &army.options);
    h.armies.push_back(std::move(army));
  }
  Field(L, config, "Observers");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      if (lua_istable(L, -1)) {
        Field(L, -1, "OwnerID");
        int owner = lua_isstring(L, -1) ? std::atoi(lua_tostring(L, -1)) : 0;
        lua_pop(L, 1);
        Field(L, -1, "PlayerName");
        sourceOf(owner, lua_isstring(L, -1) ? lua_tostring(L, -1) : "", params.network ? 0 : -1);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  Field(L, options, "GameSpeed");
  if (IsString(L, -1)) {
    if (EqualNoCase(lua_tostring(L, -1), "fast")) out->gameSpeed = 4;
    else if (EqualNoCase(lua_tostring(L, -1), "adjustable")) out->adjustableSpeed = true;
  }
  lua_pop(L, 1);

  Field(L, scenario, "map");
  if (!IsString(L, -1)) return fail("the scenario names no map");
  h.map = lua_tostring(L, -1);
  lua_settop(L, top);
  return true;
}

bool SessionFromSinglePlayerInfo(lua_State* L, int info, const SinglePlayerParams& params, SessionDescription* out,
                                 std::string* error) {
  const int top = lua_gettop(L);
  info = AbsIndex(L, info);
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    lua_settop(L, top);
    return false;
  };
  if (!lua_istable(L, info)) return fail("the session info is not a table");
  *out = SessionDescription{};
  ReplayHeader& h = out->header;
  h.version = "Supreme Commander v1.50.3839";
  h.replayVersion = "Replay v1.9";
  // WLD_SetupSessionInfo 0x88cbc0: a new LaunchInfoNew; its focus army (+0x68) 0 (0x88cc2e) and its cheats (+0x88)
  // the debug preference (0x88cc36).
  out->focusArmy = 0;
  h.cheats = params.cheats;
  // the scenario and the mods: the engine's byte form of the values as given (SCR_ToString 0x88cc5b, 0x88cccf)
  Field(L, info, "scenarioInfo");
  if (!lua_istable(L, -1)) return fail("sessionInfo.scenarioInfo is not a table");
  const int scenario = lua_gettop(L);
  SerializeLua(L, scenario, &h.scenario);
  Field(L, info, "scenarioMods");
  SerializeLua(L, -1, &h.mods);
  lua_pop(L, 1);
  // the armies: each value of teamInfo, in the table's order (LuaTableIterator 0x88cd3f), its options string
  // (launch info +0x90) and an empty set of command sources (+0x44, AppendArmyLaunchInfo 0x5439e0)
  Field(L, info, "teamInfo");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      ReplayHeader::Army army;
      SerializeLua(L, -1, &army.options);
      h.armies.push_back(std::move(army));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  // the seed: RandomSeed (LuaObject::GetInteger 0x907910: a number, cvttss2si of the float), else the caller's
  Field(L, info, "RandomSeed");
  if (lua_isnil(L, -1)) {
    h.seed = params.seed;
  } else {
    if (lua_type(L, -1) != LUA_TNUMBER) return fail("sessionInfo.RandomSeed is not a number");
    const float f = static_cast<float>(lua_tonumber(L, -1));
    h.seed = f > -2147483649.0f && f < 2147483648.0f ? static_cast<uint32_t>(static_cast<int32_t>(f)) : 0x80000000u;
  }
  lua_pop(L, 1);
  // the session info (0x30 bytes, 0x88ce86): the map is scenarioInfo.map, createReplay its +0x24
  Field(L, scenario, "map");
  if (!IsString(L, -1)) return fail("the scenario names no map");
  h.map = lua_tostring(L, -1);
  lua_pop(L, 1);
  Field(L, info, "createReplay");
  out->createReplay = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  // 0x88bbb0: one command source, the player (index 0, pauses -1 at 0x88bc06); every army's set becomes {0}
  // (0x88bc90..0x88bcb1, the AI armies too); then the client manager with one client, no connector, speed 0,
  // adjustable (CLIENT_CreateClientManager(1, 0, 0, 1) at 0x88bcb9).
  Field(L, info, "playerName");
  h.sources.push_back({lua_isstring(L, -1) ? lua_tostring(L, -1) : "", -1});
  lua_pop(L, 1);
  for (ReplayHeader::Army& a : h.armies) {
    a.sources.assign(1, 0);
    a.source = 0;
  }
  out->gameSpeed = 0;
  out->adjustableSpeed = true;
  lua_settop(L, top);
  return true;
}

bool PushGameConfigFile(lua_State* L, const std::string& hostPath, std::string* error) {
  auto src = hostfs::ReadFile(hostPath);
  if (!src) {
    if (error) *error = "cannot read " + hostPath;
    return false;
  }
  // drop every "-- table: <hex> (<n> bytes)"
  std::string code;
  code.reserve(src->size());
  static const char mark[] = "-- table: ";
  const size_t markLen = sizeof mark - 1;
  for (size_t i = 0; i < src->size();) {
    if (src->compare(i, markLen, mark) == 0) {
      size_t j = i + markLen;
      while (j < src->size() && std::isxdigit(static_cast<unsigned char>((*src)[j]))) ++j;
      if (src->compare(j, 2, " (") == 0) {
        size_t k = j + 2;
        while (k < src->size() && std::isdigit(static_cast<unsigned char>((*src)[k]))) ++k;
        if (k > j + 2 && src->compare(k, 7, " bytes)") == 0) {
          i = k + 7;
          continue;
        }
      }
    }
    code.push_back((*src)[i++]);
  }
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  if (luaL_loadbuffer(L, code.data(), code.size(), ("@" + hostfs::DisplayPath(hostPath)).c_str()) != 0) {
    if (error) *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
    lua_settop(L, top);
    return false;
  }
  lua_newtable(L);  // the file's environment: it sets GameConfig and nothing else
  lua_pushvalue(L, -1);
  lua_setfenv(L, -3);
  lua_insert(L, top + 1);  // env below the error handler
  if (lua_pcall(L, 0, 0, top + 2) != 0) {
    if (error) *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
    lua_settop(L, top);
    return false;
  }
  lua_settop(L, top + 1);
  lua_pushstring(L, "GameConfig");
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (!lua_istable(L, -1)) {
    if (error) *error = hostPath + " does not set GameConfig";
    lua_settop(L, top);
    return false;
  }
  return true;
}

std::string MapFolderOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string SessionMapFolder(const ReplayHeader& header) {
  std::string scenarioFile;
  if (lua_State* L = lua_open()) {  // (a state of its own, as ScenarioMarksFixes reads the scenario info)
    if (PushSerializedLua(L, header.scenario) && lua_istable(L, -1)) {
      lua_pushstring(L, "Options");
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "ScenarioFile");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) scenarioFile = lua_tostring(L, -1);
      }
    }
    lua_close(L);
  }
  return MapFolderOf(scenarioFile.empty() ? header.map : scenarioFile);
}

std::string PreGameDataPrefs(const std::string& mapFolder) {
  if (mapFolder.empty()) return "PreGameData = { }\n";
  std::string quoted;  // a Lua string in single quotes, as the preferences writer quotes one
  for (char c : mapFolder) {
    if (c == '\\' || c == '\'') quoted += '\\';
    if (c == '\n') quoted += "\\n";
    else if (c == '\r') quoted += "\\r";
    else quoted += c;
  }
  return "PreGameData = {\n    CurrentMapDir = '" + quoted + "'\n}\n";
}

}  // namespace moho
