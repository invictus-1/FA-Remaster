// The lobby object (CLobby, moho.lobby_methods): what the game's lobby script talks to when it
// sets up a game. The original's is a network object; a skirmish goes through it too, with the
// protocol "None" and nobody else in it.
//
// This is its local form: it can host, has no peers, and sends nothing. Joining a game and
// starting one are NOT BUILT: joining reports a failed connection; LaunchGame writes the game
// configuration the script hands over (to the log, or to the file the host names) and tells the
// script the game launched, but no session starts.
//
// Read from the original: InternalCreateLobby takes exactly 7 arguments and returns nil when no
// connection can be made (FA exe 0x7c0d40); a new lobby has the id -1 unless one was given, and
// HostGame gives it the next free id, 0 for the first (0x7c0970, 0x7c1b20), then calls
// self:Hosting(); GetLocalPlayerID returns the id as a string (0x7c2fc0).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/script_object.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

class Lobby : public ScriptObject {
 public:
  std::string protocol, name;
  int port = 0;
  int uid = -1;
  bool joined = false;  // HostGame or JoinGame was called
  bool host = false;

  // self:<fn>(nargs values on the stack)
  void Call(lua_State* L, const char* fn, int nargs) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, ScriptTraceback);
    lua_insert(L, base + 1);
    PushObject(L, this);
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
      lua_settop(L, base);
      return;
    }
    lua_insert(L, base + 2);
    lua_insert(L, base + 3);
    if (lua_pcall(L, nargs + 1, 0, base + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    lua_settop(L, base);
  }
};

Lobby* Self(lua_State* L) { return CheckObject<Lobby>(L, 1); }

// InternalCreateLobby(class, protocol, localPort, maxConnections, playerName, playerUID, natTraversalProvider)
int l_InternalCreateLobby(lua_State* L) {
  if (lua_gettop(L) != 7)
    return luaL_error(L, "%s\n  expected %d args, but got %d",
                      "InternalCreateLobby(class, string protocol, int localPort, int maxConnections, string playerName, "
                      "string playerUID, natTraversalProvider)", 7, lua_gettop(L));
  luaL_checktype(L, 1, LUA_TTABLE);
  Lobby* lobby = new Lobby;
  lobby->protocol = luaL_checkstring(L, 2);
  lobby->port = static_cast<int>(luaL_checknumber(L, 3));
  luaL_checknumber(L, 4);
  lobby->name = luaL_checkstring(L, 5);
  if (!lua_isnil(L, 6)) lobby->uid = std::atoi(luaL_checkstring(L, 6));
  // Inferred: the script object is a new table with the given class as its metatable.
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setmetatable(L, -2);
  BindObject(L, -1, lobby);
  Logf(LogLevel::Info, "LOBBY: starting with local uid of %d [%s]", lobby->uid, lobby->name.c_str());
  return 1;
}

int l_HostGame(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->joined) return luaL_error(L, "Attempting to host or join after already having done so.");
  lobby->joined = lobby->host = true;
  if (lobby->uid == -1) {
    lobby->uid = 0;
    Logf(LogLevel::Info, "LOBBY: assigning ourselves the uid of %d", lobby->uid);
  }
  lobby->Call(L, "Hosting", 0);
  return 0;
}

// Not built: there is no network here, so the connection fails at once.
int l_JoinGame(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->joined) return luaL_error(L, "Attempting to host or join after already having done so.");
  lobby->joined = true;
  Logf(LogLevel::Warning, "moho64ui: CLobby:JoinGame: joining a game is not built yet");
  lua_pushstring(L, "TimedOutToHost");
  lobby->Call(L, "ConnectionFailed", 1);
  return 0;
}

int l_GetLocalPlayerID(lua_State* L) {
  lua_pushstring(L, std::to_string(Self(L)->uid).c_str());
  return 1;
}
int l_GetLocalPlayerName(lua_State* L) {
  lua_pushstring(L, Self(L)->name.c_str());
  return 1;
}
int l_GetLocalPort(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->protocol == "None") lua_pushnil(L);
  else lua_pushnumber(L, static_cast<lua_Number>(lobby->port));
  return 1;
}
int l_IsHost(lua_State* L) {
  lua_pushboolean(L, Self(L)->host);
  return 1;
}

// Nobody else is here.
int l_GetPeers(lua_State* L) {
  Self(L);
  lua_newtable(L);
  return 1;
}
int l_GetPeer(lua_State* L) {
  Self(L);
  lua_pushnil(L);
  return 1;
}
int l_BroadcastData(lua_State* L) {
  Self(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  return 0;
}
int l_SendData(lua_State* L) {
  Self(L);
  Logf(LogLevel::Warning, "CLobby::SendScriptData(): sending to unknown UID %d?", std::atoi(lua_tostring(L, 2) ? lua_tostring(L, 2) : "-1"));
  return 0;
}
int l_Nothing(lua_State* L) {
  Self(L);
  return 0;
}

// The names as they are: with nobody else there is nothing to tell them apart from.
int l_MakeValidGameName(lua_State* L) {
  Self(L);
  lua_pushstring(L, luaL_checkstring(L, 2));
  return 1;
}
int l_MakeValidPlayerName(lua_State* L) {
  Self(L);
  lua_pushstring(L, luaL_checkstring(L, 3));
  return 1;
}

int l_DebugDump(lua_State* L) {
  Lobby* lobby = Self(L);
  Logf(LogLevel::Info, "Lobby: \"%s\" uid=%d protocol=%s host=%d, no peers", lobby->name.c_str(), lobby->uid,
       lobby->protocol.c_str(), lobby->host ? 1 : 0);
  return 0;
}

int l_Destroy(lua_State* L) {
  if (Lobby* lobby = dynamic_cast<Lobby*>(GetObject(L, 1))) delete lobby;
  return 0;
}

// A Lua value as source text that reads back as the same value: tables in full depth with their
// keys in a fixed order (numbers first, then names), strings escaped. Functions and other values
// that cannot be written become nil.
void WriteLiteral(lua_State* L, int idx, int depth, std::string* out) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
      *out += lua_toboolean(L, idx) ? "true" : "false";
      return;
    case LUA_TNUMBER: {
      char buf[40];
      double v = lua_tonumber(L, idx);
      if (v == static_cast<double>(static_cast<long long>(v)) && v > -1e15 && v < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
      else std::snprintf(buf, sizeof buf, "%.9g", v);
      *out += buf;
      return;
    }
    case LUA_TSTRING: {
      size_t n = lua_strlen(L, idx);
      const char* s = lua_tostring(L, idx);
      *out += '"';
      for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
          *out += '\\';
          *out += static_cast<char>(c);
        } else if (c < 32 || c == 127) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03d", c);
          *out += buf;
        } else {
          *out += static_cast<char>(c);
        }
      }
      *out += '"';
      return;
    }
    case LUA_TTABLE: {
      if (depth > 32) break;
      std::vector<double> numbers;
      std::vector<std::string> names;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER) numbers.push_back(lua_tonumber(L, -2));
        else if (lua_type(L, -2) == LUA_TSTRING) names.emplace_back(lua_tostring(L, -2), lua_strlen(L, -2));
        lua_pop(L, 1);
      }
      std::sort(numbers.begin(), numbers.end());
      std::sort(names.begin(), names.end());
      std::string pad(static_cast<size_t>(depth + 1) * 2, ' ');
      *out += "{\n";
      for (double k : numbers) {
        lua_pushnumber(L, static_cast<lua_Number>(k));
        *out += pad + "[";
        WriteLiteral(L, -1, depth + 1, out);
        *out += "] = ";
        lua_rawget(L, idx);
        WriteLiteral(L, -1, depth + 1, out);
        lua_pop(L, 1);
        *out += ",\n";
      }
      for (const std::string& k : names) {
        lua_pushlstring(L, k.data(), k.size());
        *out += pad + "[";
        WriteLiteral(L, -1, depth + 1, out);
        *out += "] = ";
        lua_rawget(L, idx);
        WriteLiteral(L, -1, depth + 1, out);
        lua_pop(L, 1);
        *out += ",\n";
      }
      *out += std::string(static_cast<size_t>(depth) * 2, ' ') + "}";
      return;
    }
    default:
      break;
  }
  *out += "nil";
}

// CLobby:LaunchGame(gameConfig). The game configuration (GameOptions, PlayerOptions, Observers,
// GameMods, ...) is what a session is started from, so it is kept: written to the log or, when
// one is named, to the host's session file in a form that reads back (--session starts from it).
int l_LaunchGame(lua_State* L) {
  Lobby* lobby = Self(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  std::string text;
  WriteLiteral(L, 2, 0, &text);
  lua_settop(L, 2);
  const std::string& file = UserState::From(L)->config().sessionFile;
  const auto& launch = UserState::From(L)->config().launchSession;
  Logf(LogLevel::Info, "moho64ui: CLobby:LaunchGame: game configuration (%zu bytes)%s%s", text.size(),
       file.empty() ? " follows" : " written to ", file.c_str());
  if (!launch) Logf(LogLevel::Warning, "moho64ui: CLobby:LaunchGame: this host cannot start a session; nothing starts");
  if (file.empty()) LogWrite(LogLevel::Debug, text);
  else if (!hostfs::WriteFile(file, "GameConfig = " + text + "\n")) Logf(LogLevel::Warning, "moho64ui: cannot write %s", file.c_str());
  // The host starts the session (when it can: a host without the simulation only records). As in
  // the original (FA exe 0x7c38c0), the script then hears self:GameLaunched(), on which it closes
  // the lobby and destroys this object, or self:LaunchFailed(reason).
  std::string why;
  if (launch && !launch(L, 2, &why)) {
    Logf(LogLevel::Warning, "moho64ui: CLobby:LaunchGame: launch failed: %s", why.c_str());
    lua_pushstring(L, why.c_str());
    lobby->Call(L, "LaunchFailed", 1);
    return 0;
  }
  lobby->Call(L, "GameLaunched", 0);
  return 0;  // `lobby` may be gone here
}

}  // namespace

void RegisterLobbyBindings(lua_State* L) {
  const char* cls = "CLobby";
  SetGlobal(L, "InternalCreateLobby", l_InternalCreateLobby);
  SetMethod(L, cls, "HostGame", l_HostGame);
  SetMethod(L, cls, "JoinGame", l_JoinGame);
  SetMethod(L, cls, "GetLocalPlayerID", l_GetLocalPlayerID);
  SetMethod(L, cls, "GetLocalPlayerName", l_GetLocalPlayerName);
  SetMethod(L, cls, "GetLocalPort", l_GetLocalPort);
  SetMethod(L, cls, "IsHost", l_IsHost);
  SetMethod(L, cls, "GetPeers", l_GetPeers);
  SetMethod(L, cls, "GetPeer", l_GetPeer);
  SetMethod(L, cls, "BroadcastData", l_BroadcastData);
  SetMethod(L, cls, "SendData", l_SendData);
  SetMethod(L, cls, "ConnectToPeer", l_Nothing);
  SetMethod(L, cls, "DisconnectFromPeer", l_Nothing);
  SetMethod(L, cls, "EjectPeer", l_Nothing);
  SetMethod(L, cls, "MakeValidGameName", l_MakeValidGameName);
  SetMethod(L, cls, "MakeValidPlayerName", l_MakeValidPlayerName);
  SetMethod(L, cls, "DebugDump", l_DebugDump);
  SetMethod(L, cls, "Destroy", l_Destroy);
  SetMethod(L, cls, "LaunchGame", l_LaunchGame);
}

}  // namespace moho::ui
