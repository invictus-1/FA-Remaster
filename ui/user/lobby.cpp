// The lobby object (CLobby, moho.lobby_methods): what the game's lobby script talks to when it
// sets up a game. The original's is a network object; a skirmish goes through it too, with the
// protocol "None" and nobody else in it.
//
// The network side is ui/net/lobby_core.h (the original's messages 100..107 between the host and
// the joiners, every pair of players connected directly; the connector ui/net/connector.h): here
// the script's calls go to it, and what it hears comes back as the script's callbacks (Hosting,
// ConnectionFailed, ConnectionToHostEstablished, DataReceived, PeerDisconnected, Ejected,
// SystemMessage, EstablishedPeers), each frame (PumpLobbies, from UserState::Frame, as the
// original's lobby tasks run on the user thread: CLobby::PullTask 0x7c5490 / PushTask 0x7c56b0).
// LaunchGame writes the game configuration and starts this machine's session; in a lobby with
// peers the lobby's connector and connections go to the game (net/lobby_core.h GameLink), whose
// beats then go through the engine's Lockstep over net/beat_exchange_net.h (app/game_session.h).
//
// Read from the original: InternalCreateLobby takes exactly 7 arguments and returns nil when no
// connection can be made (FA exe 0x7c0d40); a new lobby has the id -1 unless one was given, and
// HostGame gives it the next free id, 0 for the first (0x7c0970, 0x7c1b20), then calls
// self:Hosting(); GetLocalPlayerID returns the id as a string (0x7c2fc0).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "net/discovery.h"
#include "net/lobby_core.h"
#include "script/script_state.h"
#include "sim/replay.h"
#include "sim/script_object.h"
#include "sim/session.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

class Lobby;
std::vector<Lobby*> g_lobbies;  // the live ones (PumpLobbies), each with the Lua state it belongs to

class Lobby : public ScriptObject {
 public:
  std::string protocol;
  UserState* owner = nullptr;  // (the interface state it belongs to: made in any of its script threads, pumped by it)
  bool joined = false;  // HostGame or JoinGame was called
  std::unique_ptr<net::LobbyCore> core;
  std::unique_ptr<net::DiscoveryListener> discovery;  // (a networked host: its LAN listener on UDP 15000)
  // What the network heard during a pump, for the script once the pump is through (a callback may destroy us).
  std::vector<std::function<void(lua_State*)>> heard;

  Lobby() { g_lobbies.push_back(this); }
  ~Lobby() override { g_lobbies.erase(std::remove(g_lobbies.begin(), g_lobbies.end(), this), g_lobbies.end()); }

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

  // self:<fn>() with one result left on the stack (nil when there is no such function or it fails)
  void CallForResult(lua_State* L, const char* fn) {
    const int base = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    PushObject(L, this);
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
      lua_settop(L, base);
      lua_pushnil(L);
      return;
    }
    lua_insert(L, -2);  // (fn, self)
    if (lua_pcall(L, 1, 1, base + 1) != 0) {
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
      lua_settop(L, base);
      lua_pushnil(L);
      return;
    }
    lua_remove(L, base + 1);  // (the traceback function)
  }

  void Hook();  // the core's hooks: queued as the script's callbacks
};

bool Alive(Lobby* l) { return std::find(g_lobbies.begin(), g_lobbies.end(), l) != g_lobbies.end(); }

void Lobby::Hook() {
  net::LobbyHooks& h = core->hooks;
  h.hosting = [this] { heard.push_back([this](lua_State* L) { Call(L, "Hosting", 0); }); };
  h.connectionFailed = [this](const std::string& reason) {
    heard.push_back([this, reason](lua_State* L) {
      lua_pushstring(L, reason.c_str());
      Call(L, "ConnectionFailed", 1);
    });
  };
  h.connectionToHostEstablished = [this](int localUid, const std::string& localName, int hostUid) {
    heard.push_back([this, localUid, localName, hostUid](lua_State* L) {  // (all strings: 0x7cbad0)
      lua_pushstring(L, std::to_string(localUid).c_str());
      lua_pushstring(L, localName.c_str());
      lua_pushstring(L, std::to_string(hostUid).c_str());
      Call(L, "ConnectionToHostEstablished", 3);
    });
  };
  h.dataReceived = [this](const std::string& bytes, int sender, const std::string& senderName) {
    heard.push_back([this, bytes, sender, senderName](lua_State* L) {  // (OnScriptData 0x7c6ee0: SenderID, SenderName added)
      if (!PushSerializedLua(L, bytes) || !lua_istable(L, -1)) {
        lua_settop(L, 0);
        Logf(LogLevel::Warning, "LOBBY: ignoring script data from uid %d that is not a table", sender);
        return;
      }
      lua_pushstring(L, "SenderID");
      lua_pushstring(L, std::to_string(sender).c_str());
      lua_rawset(L, -3);
      lua_pushstring(L, "SenderName");
      lua_pushstring(L, senderName.c_str());
      lua_rawset(L, -3);
      Call(L, "DataReceived", 1);
    });
  };
  h.peerDisconnected = [this](const std::string& name, int uid) {
    heard.push_back([this, name, uid](lua_State* L) {
      lua_pushstring(L, name.c_str());
      lua_pushstring(L, std::to_string(uid).c_str());
      Call(L, "PeerDisconnected", 2);
    });
  };
  h.ejected = [this](const std::string& reason) {
    heard.push_back([this, reason](lua_State* L) {
      lua_pushstring(L, reason.c_str());
      Call(L, "Ejected", 1);
    });
  };
  h.systemMessage = [this](const std::string& text) {
    Logf(LogLevel::Info, "LOBBY: %s", text.c_str());
    heard.push_back([this, text](lua_State* L) {
      lua_pushstring(L, text.c_str());
      Call(L, "SystemMessage", 1);
    });
  };
  h.establishedPeers = [this](int uid, const std::vector<int>& list) {
    heard.push_back([this, uid, list](lua_State* L) {
      lua_pushstring(L, std::to_string(uid).c_str());
      lua_newtable(L);
      for (size_t i = 0; i < list.size(); ++i) {
        lua_pushstring(L, std::to_string(list[i]).c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
      }
      Call(L, "EstablishedPeers", 2);
    });
  };
}

// What the core heard, to the script (the lobby may be destroyed by any of them: then the rest are dropped).
void Deliver(Lobby* lobby, lua_State* L) {
  while (Alive(lobby) && !lobby->heard.empty()) {
    auto next = std::move(lobby->heard.front());
    lobby->heard.erase(lobby->heard.begin());
    next(L);
  }
}

Lobby* Self(lua_State* L) { return CheckObject<Lobby>(L, 1); }

int UidArg(lua_State* L, int idx) {  // (the scripts pass uids as strings, sometimes numbers)
  if (lua_type(L, idx) == LUA_TNUMBER) return static_cast<int>(lua_tonumber(L, idx));
  return std::atoi(luaL_checkstring(L, idx));
}

// InternalCreateLobby(class, protocol, localPort, maxConnections, playerName, playerUID, natTraversalProvider)
int l_InternalCreateLobby(lua_State* L) {
  if (lua_gettop(L) != 7)
    return luaL_error(L, "%s\n  expected %d args, but got %d",
                      "InternalCreateLobby(class, string protocol, int localPort, int maxConnections, string playerName, "
                      "string playerUID, natTraversalProvider)", 7, lua_gettop(L));
  luaL_checktype(L, 1, LUA_TTABLE);
  const std::string protocol = luaL_checkstring(L, 2);
  const int port = static_cast<int>(luaL_checknumber(L, 3));
  const int maxConnections = static_cast<int>(luaL_checknumber(L, 4));
  const std::string name = luaL_checkstring(L, 5);
  const int uid = lua_isnil(L, 6) ? -1 : std::atoi(luaL_checkstring(L, 6));
  std::unique_ptr<net::Connector> connector;
  if (protocol != "None") {
    connector = net::MakeConnector(protocol, static_cast<uint16_t>(port));
    if (!connector) {  // (the original: nil when the port cannot be opened)
      Logf(LogLevel::Warning, "LOBBY: cannot open game port %d [%s]", port, protocol.c_str());
      lua_pushnil(L);
      return 1;
    }
    Logf(LogLevel::Info, "LOBBY: Game port %d[%s] opened.", connector->LocalPort(), protocol.c_str());
  }
  Lobby* lobby = new Lobby;
  lobby->protocol = protocol;
  lobby->owner = UserState::From(L);
  lobby->core = std::make_unique<net::LobbyCore>(std::move(connector), name, uid, maxConnections > 0 ? maxConnections : 16);
  lobby->Hook();
  // Inferred: the script object is a new table with the given class as its metatable.
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setmetatable(L, -2);
  BindObject(L, -1, lobby);
  Logf(LogLevel::Info, "LOBBY: starting with local uid of %d [%s]", uid, name.c_str());
  return 1;
}

int l_HostGame(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->joined) return luaL_error(L, "Attempting to host or join after already having done so.");
  lobby->joined = true;
  const int before = lobby->core->Uid();
  if (lobby->protocol != "None") {  // (HostGame 0x7c1bae: the LAN listener; hosting goes on without it)
    lobby->discovery = std::make_unique<net::DiscoveryListener>();
    if (!lobby->discovery->Open()) {
      Logf(LogLevel::Info, "LOBBY: Creating discovery listener failed -- someone else must be hosting a game on this machine.");
      lobby->discovery.reset();
    }
  }
  lobby->core->Host();
  if (before == -1) Logf(LogLevel::Info, "LOBBY: assigning ourselves the uid of %d", lobby->core->Uid());
  Deliver(lobby, L);  // (Hosting at once, as the original)
  return 0;
}

// JoinGame(address, remotePlayerName, remotePlayerUID): the connection to the host; ConnectionToHostEstablished or
// ConnectionFailed("HostLeft") follows in a later frame.
int l_JoinGame(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->joined) return luaL_error(L, "Attempting to host or join after already having done so.");
  lobby->joined = true;
  net::Address host;
  if (!net::Address::Parse(luaL_checkstring(L, 2), 0, &host) || host.port == 0) {
    Logf(LogLevel::Warning, "LOBBY: cannot join %s: not an address", lua_tostring(L, 2));
    lua_pushstring(L, "HostLeft");
    lobby->Call(L, "ConnectionFailed", 1);
    return 0;
  }
  lobby->core->Join(host);
  Deliver(lobby, L);
  return 0;
}

// ConnectToPeer(address, name, uid) / DisconnectFromPeer(uid) / EjectPeer(uid, reason)
int l_ConnectToPeer(lua_State* L) {
  Lobby* lobby = Self(L);
  net::Address at;
  if (!net::Address::Parse(luaL_checkstring(L, 2), 0, &at)) return 0;
  lobby->core->ConnectToPeer(at, luaL_checkstring(L, 3), UidArg(L, 4));
  return 0;
}
int l_DisconnectFromPeer(lua_State* L) {
  Lobby* lobby = Self(L);
  lobby->core->DisconnectFromPeer(UidArg(L, 2));
  Deliver(lobby, L);
  return 0;
}
int l_EjectPeer(lua_State* L) {
  Lobby* lobby = Self(L);
  lobby->core->EjectPeer(UidArg(L, 2), lua_isstring(L, 3) ? lua_tostring(L, 3) : "KickedByHost");
  Deliver(lobby, L);
  return 0;
}

int l_GetLocalPlayerID(lua_State* L) {
  lua_pushstring(L, std::to_string(Self(L)->core->Uid()).c_str());
  return 1;
}
int l_GetLocalPlayerName(lua_State* L) {
  lua_pushstring(L, Self(L)->core->Name().c_str());
  return 1;
}
int l_GetLocalPort(lua_State* L) {
  Lobby* lobby = Self(L);
  if (lobby->protocol == "None") lua_pushnil(L);
  else lua_pushnumber(L, static_cast<lua_Number>(lobby->core->Port()));
  return 1;
}
int l_IsHost(lua_State* L) {
  lua_pushboolean(L, Self(L)->core->IsHost());
  return 1;
}

// A peer as GetPeer / GetPeers give it (SPeer::ToLua 0x7c2950): name, id, status, ping, quiet, establishedPeers.
void PushPeer(lua_State* L, const net::PeerInfo& p) {
  lua_newtable(L);
  lua_pushstring(L, "name");
  lua_pushstring(L, p.name.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "id");
  lua_pushstring(L, std::to_string(p.uid).c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "status");
  lua_pushstring(L, p.status.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "ping");
  lua_pushnumber(L, static_cast<lua_Number>(p.ping));
  lua_rawset(L, -3);
  lua_pushstring(L, "quiet");
  lua_pushnumber(L, static_cast<lua_Number>(p.quiet));
  lua_rawset(L, -3);
  lua_pushstring(L, "establishedPeers");
  lua_newtable(L);
  for (size_t i = 0; i < p.established.size(); ++i) {
    lua_pushstring(L, std::to_string(p.established[i]).c_str());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
}
int l_GetPeers(lua_State* L) {
  const auto peers = Self(L)->core->Peers();
  lua_newtable(L);
  for (size_t i = 0; i < peers.size(); ++i) {
    PushPeer(L, peers[i]);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  return 1;
}
int l_GetPeer(lua_State* L) {
  const int uid = UidArg(L, 2);
  for (const auto& p : Self(L)->core->Peers())
    if (p.uid == uid) {
      PushPeer(L, p);
      return 1;
    }
  lua_pushnil(L);
  return 1;
}

int l_BroadcastData(lua_State* L) {
  Lobby* lobby = Self(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  std::string bytes;
  SerializeLua(L, 2, &bytes);  // (LuaObject::ToByteStream 0x4d2c80: the engine's form)
  lobby->core->Broadcast(bytes);
  return 0;
}
int l_SendData(lua_State* L) {
  Lobby* lobby = Self(L);
  const int uid = UidArg(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  if (!lobby->core->HasPeer(uid)) return luaL_error(L, "CLobby::SendScriptData(): sending to unknown UID %d?", uid);
  std::string bytes;
  SerializeLua(L, 3, &bytes);
  lobby->core->SendTo(uid, bytes);
  return 0;
}

// MakeValidGameName(name): as it is (INFERRED: the original's 0x7c1510 only trims what a game name may not hold).
int l_MakeValidGameName(lua_State* L) {
  Self(L);
  lua_pushstring(L, luaL_checkstring(L, 2));
  return 1;
}
// MakeValidPlayerName(uid, name): unlike the other players' names (0x7c1720).
int l_MakeValidPlayerName(lua_State* L) {
  Lobby* lobby = Self(L);
  lua_pushstring(L, lobby->core->MakeValidPlayerName(UidArg(L, 2), luaL_checkstring(L, 3)).c_str());
  return 1;
}

int l_DebugDump(lua_State* L) {
  Lobby* lobby = Self(L);
  const auto peers = lobby->core->Peers();
  Logf(LogLevel::Info, "Lobby: \"%s\" uid=%d protocol=%s port=%d host=%d, %zu peer(s)", lobby->core->Name().c_str(),
       lobby->core->Uid(), lobby->protocol.c_str(), lobby->core->Port(), lobby->core->IsHost() ? 1 : 0, peers.size());
  for (const auto& p : peers) Logf(LogLevel::Info, "  \"%s\" uid=%d %s", p.name.c_str(), p.uid, p.status.c_str());
  return 0;
}

int l_Destroy(lua_State* L) {
  if (Lobby* lobby = dynamic_cast<Lobby*>(GetObject(L, 1))) delete lobby;
  return 0;
}

// ValidateIPAddress("host:port") (FA exe 0x7c8360): the address resolved, as "a.b.c.d:port", or nil.
int l_ValidateIPAddress(lua_State* L) {
  net::Address a;
  if (!net::Address::Parse(luaL_checkstring(L, 1), 0, &a) || a.port == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, a.ToString().c_str());
  return 1;
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
      if (depth > 32 || !lua_checkstack(L, 4)) break;  // (each level its room on the stack: prefs.cpp PushCopy)
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
  const auto& config = UserState::From(L)->config();
  const std::string file = config.sessionFileNow ? config.sessionFileNow() : config.sessionFile;
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
  // A lobby with peers: every player's program launches the same game (FAF's lobby.lua: the host broadcasts
  // {Type = 'Launch', GameInfo = ...} and calls LaunchGame, each joiner calls it with that table), and the lobby's
  // network goes to the game (0x7c4a8e: the client manager over the lobby's connector).
  if (lobby->core->Networked()) {
    const auto& launchNet = UserState::From(L)->config().launchNetworkSession;
    if (!launchNet) why = "this host cannot start a network game";
    if (!launchNet || !launchNet(L, 2, lobby->core.get(), &why)) {
      Logf(LogLevel::Warning, "moho64ui: CLobby:LaunchGame: launch failed: %s", why.c_str());
      lua_pushstring(L, why.c_str());
      lobby->Call(L, "LaunchFailed", 1);
      return 0;
    }
    lobby->Call(L, "GameLaunched", 0);
    return 0;
  }
  if (launch && !launch(L, 2, &why)) {
    Logf(LogLevel::Warning, "moho64ui: CLobby:LaunchGame: launch failed: %s", why.c_str());
    lua_pushstring(L, why.c_str());
    lobby->Call(L, "LaunchFailed", 1);
    return 0;
  }
  lobby->Call(L, "GameLaunched", 0);
  return 0;  // `lobby` may be gone here
}

// LaunchSinglePlayerSession(sessionInfo) (FA exe 0x88d3c0): a single-player session, an operation
// of the campaign, from the table FAF's SinglePlayerLaunch.lua makes (playerName, createReplay,
// scenarioInfo, teamInfo, scenarioMods, RandomSeed). The scenario's file goes into its Options as
// ScenarioFile, where the engine reads it (FAF's ScenarioFramework.EndOperation needs it too; the
// table has none). Nothing is returned; a session that cannot start is a script error, as in the
// original (the briefing's pcall shows it: "Error loading map"). The front end then closes, as
// after a lobby's launch.
int l_LaunchSinglePlayerSession(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  const auto& launch = UserState::From(L)->config().launchSinglePlayer;
  if (!launch) return luaL_error(L, "LaunchSinglePlayerSession: this host cannot start a session");
  lua_getfield(L, 1, "scenarioInfo");
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "Options");
    lua_getfield(L, 2, "file");
    if (lua_istable(L, 3) && lua_isstring(L, 4)) {
      lua_pushstring(L, "ScenarioFile");
      lua_pushvalue(L, 4);
      lua_rawset(L, 3);
    }
  }
  lua_settop(L, 1);
  std::string why;
  if (!launch(L, 1, &why)) {
    Logf(LogLevel::Warning, "moho64ui: LaunchSinglePlayerSession: %s", why.c_str());
    return luaL_error(L, "%s", why.c_str());
  }
  return 0;
}

// The LAN browser (CDiscoveryService, moho.discovery_service_methods; InternalCreateDiscoveryService 0x7c00e0): the
// games the hosts on the network answer with, to the script as GameFound(index, config) / GameUpdated(index, config)
// / RemoveGame(index), the config the host's GameConfigRequested table with Hostname, Address ("ip:port") and
// Protocol added (OnDatagram 0x7bfb70). Hostname: the host's IP (INFERRED: the original's may be its name).
class Discovery;
std::vector<Discovery*> g_discoveries;

class Discovery : public ScriptObject {
 public:
  UserState* owner = nullptr;
  net::DiscoveryBrowser browser;
  Discovery() { g_discoveries.push_back(this); }
  ~Discovery() override { g_discoveries.erase(std::remove(g_discoveries.begin(), g_discoveries.end(), this), g_discoveries.end()); }

  void Call(lua_State* L, const char* fn, int nargs) {  // self:<fn>(nargs values on the stack)
    const int base = lua_gettop(L) - nargs;
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

bool Alive(Discovery* d) { return std::find(g_discoveries.begin(), g_discoveries.end(), d) != g_discoveries.end(); }

void PushGameConfig(lua_State* L, const net::DiscoveredGame& g) {
  const int top = lua_gettop(L);
  if (!PushSerializedLua(L, g.table) || !lua_istable(L, -1)) {
    lua_settop(L, top);
    lua_newtable(L);
  }
  lua_pushstring(L, "Hostname");
  lua_pushstring(L, g.address.IpString().c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "Address");
  lua_pushstring(L, g.address.ToString().c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "Protocol");
  lua_pushstring(L, net::ProtocolName(g.protocol).c_str());
  lua_rawset(L, -3);
}

void TellRemoved(Discovery* d, lua_State* L, const std::vector<int>& removed) {
  for (int index : removed) {
    if (!Alive(d)) return;
    lua_pushnumber(L, static_cast<lua_Number>(index));
    d->Call(L, "RemoveGame", 1);
  }
}

void PumpDiscoveries(lua_State* L) {
  const std::vector<Discovery*> now = g_discoveries;
  for (Discovery* d : now) {
    if (!Alive(d) || d->owner != UserState::From(L)) continue;
    std::vector<net::DiscoveredGame> found, updated;
    std::vector<int> removed;
    d->browser.Poll(&found, &updated, &removed);
    for (const auto& g : found) {
      if (!Alive(d)) break;
      lua_pushnumber(L, static_cast<lua_Number>(g.index));
      PushGameConfig(L, g);
      d->Call(L, "GameFound", 2);
    }
    for (const auto& g : updated) {
      if (!Alive(d)) break;
      lua_pushnumber(L, static_cast<lua_Number>(g.index));
      PushGameConfig(L, g);
      d->Call(L, "GameUpdated", 2);
    }
    if (Alive(d)) TellRemoved(d, L, removed);
  }
}

// InternalCreateDiscoveryService(class)
int l_InternalCreateDiscoveryService(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Discovery* d = new Discovery;
  d->owner = UserState::From(L);
  if (!d->browser.Open()) Logf(LogLevel::Warning, "DISCOVERY: cannot open a socket to look for games");
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setmetatable(L, -2);
  BindObject(L, -1, d);
  return 1;
}
Discovery* SelfDiscovery(lua_State* L) { return CheckObject<Discovery>(L, 1); }
int l_DiscoveryGetGameCount(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(SelfDiscovery(L)->browser.Count()));
  return 1;
}
int l_DiscoveryReset(lua_State* L) {
  Discovery* d = SelfDiscovery(L);
  std::vector<int> removed;
  d->browser.Reset(&removed);
  TellRemoved(d, L, removed);
  return 0;
}
int l_DiscoveryDestroy(lua_State* L) {
  if (Discovery* d = dynamic_cast<Discovery*>(GetObject(L, 1))) delete d;
  return 0;
}

}  // namespace

void RegisterLobbyBindings(lua_State* L) {
  SetGlobal(L, "LaunchSinglePlayerSession", l_LaunchSinglePlayerSession);
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
  SetMethod(L, cls, "ConnectToPeer", l_ConnectToPeer);
  SetMethod(L, cls, "DisconnectFromPeer", l_DisconnectFromPeer);
  SetMethod(L, cls, "EjectPeer", l_EjectPeer);
  SetMethod(L, cls, "MakeValidGameName", l_MakeValidGameName);
  SetMethod(L, cls, "MakeValidPlayerName", l_MakeValidPlayerName);
  SetMethod(L, cls, "DebugDump", l_DebugDump);
  SetMethod(L, cls, "Destroy", l_Destroy);
  SetMethod(L, cls, "LaunchGame", l_LaunchGame);
  SetGlobal(L, "ValidateIPAddress", l_ValidateIPAddress);
  SetGlobal(L, "InternalCreateDiscoveryService", l_InternalCreateDiscoveryService);
  SetMethod(L, "CDiscoveryService", "GetGameCount", l_DiscoveryGetGameCount);
  SetMethod(L, "CDiscoveryService", "Reset", l_DiscoveryReset);
  SetMethod(L, "CDiscoveryService", "Destroy", l_DiscoveryDestroy);
  // moho64ui_WallSeconds(): the PC's clock in seconds (ours, for tests: a run without a window steps its frames as
  // fast as it can, so its scripts' clocks say nothing of how long another program has had)
  lua_register(L, "moho64ui_WallSeconds", [](lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(
                          std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    return 1;
  });
}

// Each frame (UserState::Frame): every live lobby of this Lua state reads its network and tells its script.
void PumpLobbies(lua_State* L) {
  const std::vector<Lobby*> now = g_lobbies;  // (a callback may make or destroy lobbies)
  for (Lobby* lobby : now) {
    if (!Alive(lobby) || lobby->owner != UserState::From(L)) continue;
    lobby->core->Pump();
    Deliver(lobby, L);
    if (!Alive(lobby) || !lobby->discovery) continue;
    // LAN browsers asking (OnDatagram 0x7c5840): the script's GameConfigRequested() table is the answer
    std::vector<net::Address> asking;
    lobby->discovery->Poll(&asking);
    for (const net::Address& to : asking) {
      lobby->CallForResult(L, "GameConfigRequested");
      std::string table;
      if (lua_istable(L, -1)) SerializeLua(L, lua_gettop(L), &table);
      lua_pop(L, 1);
      if (!Alive(lobby) || !lobby->discovery) break;
      lobby->discovery->Answer(to, net::ProtocolNumber(lobby->protocol), lobby->core->Port(), table);
    }
  }
  PumpDiscoveries(L);
}

}  // namespace moho::ui
