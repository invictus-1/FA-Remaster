// The replay command stream: decoding (CDecoder 0x6e40f0..) and applying each message to the sim (the Sim's
// command sink, 0x748650..0x74b100). Spec: engine-ref command_stream.md.
#include "sim/command_stream.h"

#include "core/fpenv.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <strings.h>

#include "core/dmath.h"
#include "core/log.h"
#include "core/profile.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/formation.h"
#include "sim/replay.h"
#include "sim/script_bits.h"
#include "sim/sim.h"
#include "sim/transport.h"
#include "sim/units.h"

namespace moho {
namespace {

const char* const kMsgNames[] = {"Advance", "SetCommandSource", "CommandSourceTerminated", "VerifyChecksum",
                                 "RequestPause", "Resume", "SingleStep", "CreateUnit", "CreateProp",
                                 "DestroyEntity", "WarpEntity", "ProcessInfoPair", "IssueCommand",
                                 "IssueFactoryCommand", "IncreaseCommandCount", "DecreaseCommandCount",
                                 "SetCommandTarget", "SetCommandType", "SetCommandCells", "RemoveCommandFromQueue",
                                 "DebugCommand", "ExecuteLuaInSim", "LuaSimCallback", "EndGame"};

// Payload reader (BinaryReader): little-endian, NUL-terminated strings; reads past the end set !ok.
struct In {
  const std::string& d;
  size_t p = 0;
  bool ok = true;
  bool Need(size_t n) {
    if (p + n > d.size()) ok = false;
    return ok;
  }
  uint8_t U8() { return Need(1) ? static_cast<uint8_t>(d[p++]) : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    uint16_t v;
    memcpy(&v, d.data() + p, 2);
    p += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    uint32_t v;
    memcpy(&v, d.data() + p, 4);
    p += 4;
    return v;
  }
  float F32() {
    uint32_t u = U32();
    float f;
    memcpy(&f, &u, 4);
    return f;
  }
  std::string Str() {
    size_t e = d.find('\0', p);
    if (e == std::string::npos) e = d.size();
    std::string s = d.substr(p, e - p);
    p = std::min(d.size(), e + 1);
    return s;
  }
  std::vector<uint32_t> IdSet() {  // EntIdSet: a bit set, so ascending and unique
    uint32_t n = U32();
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < n && ok; ++i) v.push_back(U32());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  }
  StreamTarget Target() {
    StreamTarget t;
    t.type = U8();
    if (t.type == 1) t.entity = U32();
    else if (t.type == 2) t.pos = {F32(), F32(), F32()};
    else if (t.type != 0) ok = false;
    return t;
  }
  std::vector<std::pair<int16_t, int16_t>> Cells() {
    uint32_t n = U32();
    std::vector<std::pair<int16_t, int16_t>> v;
    for (uint32_t i = 0; i < n && ok; ++i) {
      int16_t x = static_cast<int16_t>(U16());
      int16_t z = static_cast<int16_t>(U16());
      v.push_back({x, z});
    }
    return v;
  }
  // a serialized Lua value (SCR_FromByteStream): returns its bytes (PushSerializedLua reads them)
  std::string Lua() {
    size_t start = p;
    SkipLua(0);
    return d.substr(start, p - start);
  }
  void SkipLua(int depth) {
    if (depth > 64) {
      ok = false;
      return;
    }
    uint8_t t = U8();
    switch (t) {
      case 0: U32(); break;
      case 1: Str(); break;
      case 2: break;
      case 3: U8(); break;
      case 4:
        while (ok && p < d.size() && d[p] != 5) {
          SkipLua(depth + 1);
          SkipLua(depth + 1);
        }
        U8();
        break;
      default: break;  // 5 outside a table / unknown: a warning and nil in the original
    }
  }
  StreamIssueData CmdData() {
    StreamIssueData c;
    c.id = U32();
    c.coordWith = static_cast<int32_t>(U32());
    c.type = U8();
    c.insertBefore = static_cast<int32_t>(U32());
    c.target = Target();
    c.target2 = Target();
    c.formIndex = static_cast<int32_t>(U32());
    if (c.formIndex != -1) {
      c.qw = F32();
      c.qx = F32();
      c.qy = F32();
      c.qz = F32();
      c.scale = F32();
    }
    c.blueprint = Str();
    c.cells = Cells();
    c.maxCount = static_cast<int32_t>(U32());
    c.count = static_cast<int32_t>(U32());
    c.lua = Lua();
    if (c.type >= 40) ok = false;
    return c;
  }
};

bool HasSource(const Army* a, int src) {
  return std::find(a->sources.begin(), a->sources.end(), src) != a->sources.end();
}

// OkayToMessWith(command) 0x7473b0: every unit holding it (empty -> true)
bool OkayCommand(Sim& sim, const UnitCommand& c) {
  for (Unit* u : c.units)
    if (!sim.OkayToMessWith(u->army)) return false;
  return true;
}

}  // namespace

std::shared_ptr<UnitCommand> StreamCommand(Sim& sim, uint32_t id) {
  // (one database in the original, CCommandDb +4, for the stream's ids and the engine's 0x80000000 | index ones)
  if ((id >> 24) == 0x80u) {
    auto e = sim.engineCommands.find(id);
    if (e == sim.engineCommands.end()) return nullptr;
    auto c = e->second.lock();
    if (!c || c->id != id) return nullptr;
    return c;
  }
  auto it = sim.streamCommands.find(id);
  if (it == sim.streamCommands.end()) return nullptr;
  auto c = it->second.lock();
  if (!c) sim.streamCommands.erase(it);
  return c;
}

namespace {

// The units the current source may order (Sim::IssueCommand 0x749290): unknown ids, non-units, armies out of
// the game and armies the source does not command (without cheats) are dropped one by one.
std::vector<Unit*> CommandableUnits(Sim& sim, const std::vector<uint32_t>& ids, bool* abortAll) {
  std::vector<Unit*> out;
  for (uint32_t id : ids) {
    Entity* e = sim.FindEntityById(id);
    if (!e) continue;
    bool okay = true;
    if (Army* a = e->army) {
      if (a->outOfGame) okay = false;
      else if (!(sim.commandSource != 0xff && HasSource(a, sim.commandSource))) okay = sim.CheatsEnabled();
    } else {
      okay = sim.CheatsEnabled();  // 0x7493c2
    }
    if (!okay) {
      if (abortAll) *abortAll = true;
      continue;
    }
    if (e->kind == Entity::Kind::Unit) out.push_back(static_cast<Unit*>(e));
  }
  return out;
}

// Sim::ValidateNewCommandId 0x7491c0
bool ValidateNewCommandId(Sim& sim, uint32_t id, const char* what) {
  if (sim.commandSource == 0xff) {
    Logf(LogLevel::Warning, "%s: ignoring issue of cmd id 0x%08x because there is no command source active.", what,
         id);
    return false;
  }
  if (static_cast<int>(id >> 24) != sim.commandSource) {
    Logf(LogLevel::Warning, "%s: ignoring issue of cmd id 0x%08x because the id's source (%d) is wrong (should be %d)",
         what, id, static_cast<int>(id >> 24), sim.commandSource);
    return false;
  }
  if (auto c = StreamCommand(sim, id)) {
    // The original rejects an id still in use ("... because it is already in use."). But the player's UI only
    // reuses an id after the original's sync said the command was gone, so a clash here means OUR copy of the
    // old command is still alive where the original's had ended (a divergence). Keep playing the player's
    // orders: the old command loses its stream id and the new one is issued.
    std::string held;
    for (Unit* u : c->units)
      held += " " + std::to_string(u->id) + (u->commands.empty() || u->commands.front() != c ? "" : "(head)");
    Logf(LogLevel::Warning, "%s: cmd id 0x%08x is reused by the player while our type-%d command with that id is "
         "still held by%s (tick %u; the original had ended it)", what, id, static_cast<int>(c->type), held.c_str(),
         sim.tick());
    sim.streamCommands.erase(id);
  }
  return true;
}

// Call the engine's own Unit method (the C binding, not a Lua override) with one argument already pushed.
void CallUnitBinding(Sim& sim, Unit* u, const char* method, int nargs) {
  lua_State* L = sim.L();
  int base = lua_gettop(L) - nargs;
  lua_pushcfunction(L, ScriptTraceback);
  PushClassTable(L, "Unit");
  lua_pushstring(L, method);
  lua_gettable(L, -2);
  lua_remove(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, base);
    return;
  }
  PushObject(L, u);
  for (int i = 0; i < nargs; ++i) lua_pushvalue(L, base + 1 + i);
  prof::NextSource(prof::kCallUnit);  // (MOHO64_PROFILE only: the Lua timer's source)
  if (lua_pcall(L, nargs + 1, 0, base + nargs + 1) != 0)
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "ProcessInfoPair failed");
  lua_settop(L, base);
}

}  // namespace

bool CommandStream::ApplyUntilAdvance(Sim& sim) {
  SimFpScope fp;  // (the orders run sim code: core/fpenv.h)
  while (p_ + 3 <= d_.size()) {
    int type = static_cast<uint8_t>(d_[p_]);
    size_t len = static_cast<uint8_t>(d_[p_ + 1]) | (static_cast<size_t>(static_cast<uint8_t>(d_[p_ + 2])) << 8);
    if (len < 3) len = 3;
    if (p_ + len > d_.size()) {
      Logf(LogLevel::Error, "command stream: message at byte %zu runs past the end", p_);
      error_ = true;
      p_ = d_.size();
      return false;
    }
    std::string payload = d_.substr(p_ + 3, len - 3);
    p_ += len;
    ++messages_;
    if (type == 0) {
      uint32_t n = 0;
      if (payload.size() >= 4) memcpy(&n, payload.data(), 4);
      beats_ += n;
      if (n != 1) Logf(LogLevel::Warning, "command stream: Advance(%u) (only 1 is played as one beat)", n);
      return true;
    }
    if (type > 23) {
      Logf(LogLevel::Error, "Unexpected opcode in command stream: 0x%02x", type);
      error_ = true;
      continue;
    }
    Apply(sim, type, payload);
  }
  return false;
}

void CommandStream::Apply(Sim& sim, int type, const std::string& payload) {
  In in{payload};
  lua_State* L = sim.L();
  auto unhandled = [&](const std::string& what) { ++unhandled_[what]; };
  switch (type) {
    case 1: {  // SetCommandSource 0x748650
      int s = in.U8();
      if (s == 0xff || s < static_cast<int>(sim.cmdSources.size())) {
        sim.commandSource = s;
      } else {
        Logf(LogLevel::Warning, "Sim::SetCommandSource(%d): invalid source -- ignoring following commands.", s);
        sim.commandSource = 0xff;
      }
      break;
    }
    case 2: {  // CommandSourceTerminated 0x7486b0
      int src = sim.commandSource;
      Logf(LogLevel::Info, "Command source %s terminated tick %u",
           src != 0xff && src < static_cast<int>(sim.cmdSources.size()) ? sim.cmdSources[src].name.c_str() : "???",
           sim.tick());
      if (sim.pauseOwner == src) sim.pauseOwner = -1;
      for (const auto& a : sim.armies()) {
        auto& v = a->sources;
        auto it = std::find(v.begin(), v.end(), src);
        if (it == v.end()) continue;
        v.erase(it);
        if (v.empty() && a->brain) sim.CallMethod(L, a->brain, "AbandonedByPlayer", 0);
      }
      break;
    }
    case 3:  // VerifyChecksum: our checksums are not the original's MD5s
      in.Need(20);
      in.p = payload.size();
      break;
    case 4: {       // RequestPause 0x748960
      int src = sim.commandSource;
      if (sim.pauseOwner == -1 && src != 0xff && src < static_cast<int>(sim.cmdSources.size()) &&
          sim.cmdSources[src].pausesLeft != 0) {
        if (sim.cmdSources[src].pausesLeft > 0) --sim.cmdSources[src].pausesLeft;
        sim.pauseOwner = src;
      }
      break;
    }
    case 5:  // Resume 0x7489a0 (any source)
      if (sim.commandSource != 0xff) sim.pauseOwner = -1;
      break;
    case 6:  // SingleStep 0x7489c0
      if (sim.pauseOwner != -1 && sim.commandSource != 0xff) sim.singleStep = true;
      break;
    case 7: {  // CreateUnit 0x748aa0 (cheats; no ownership check)
      int army = in.U8();
      std::string bp = in.Str();
      float x = in.F32(), z = in.F32(), heading = in.F32();
      if (!sim.CheatsEnabled() || army >= static_cast<int>(sim.armies().size())) break;  // 0x748aae
      Army* a = sim.armies()[army].get();
      if (a->outOfGame) break;
      const BlueprintInfo* b = sim.blueprints().Find(bp);
      if (!b) break;
      Quat q{0, dmath::Sin(heading * 0.5f), 0, dmath::Cos(heading * 0.5f)};  // EulerRollToQuat 0x570750 about +Y
      sim.CreateUnit(L, *b, a, {x, 0, z}, q, true);
      unhandled("CreateUnit (unit cap test, height: inferred)");
      break;
    }
    case 8: {  // CreateProp 0x748c00 (cheats)
      std::string bp = in.Str();
      Vec3 p{in.F32(), in.F32(), in.F32()};
      if (!sim.CheatsEnabled()) break;  // 0x748c0c
      if (const BlueprintInfo* b = sim.blueprints().Find(bp)) sim.CreateProp(L, *b, p, Quat{}, {1, 1, 1});
      break;
    }
    case 9: {  // DestroyEntity 0x748c80 (ownership)
      Entity* e = sim.FindEntityById(in.U32());
      if (e && sim.OkayToMessWith(e->army)) sim.QueueDestroy(e);
      break;
    }
    case 10: {  // WarpEntity 0x748cd0 (cheats; no ownership check): the entity takes the transform
      Entity* e = sim.FindEntityById(in.U32());
      float qw = in.F32(), qx = in.F32(), qy = in.F32(), qz = in.F32();
      Vec3 p{in.F32(), in.F32(), in.F32()};
      if (!in.ok || !sim.CheatsEnabled() || !e) break;  // 0x748cd6, before the entity is looked up
      // through the entity's own Warp (0x748d3f: vt+0x44), as Lua's Warp: a unit's is Unit::Warp 0x6aa310, which
      // takes the layer of the new place (Entity::GetStartingLayer) and its height (CalcSpawnElevation). Seen: a
      // tactical missile launcher warped from land into the sea turns Seabed in that beat (Missiles, tick 707).
      if (e->kind == Entity::Kind::Unit) {
        sim.WarpUnit(L, static_cast<Unit*>(e), p, Quat{qx, qy, qz, qw});
        break;
      }
      e->position = p;
      e->orientation = Quat{qx, qy, qz, qw};
      break;
    }
    case 11: {  // ProcessInfoPair 0x748d50
      Entity* e = sim.FindEntityById(in.U32());
      std::string key = in.Str(), value = in.Str();
      if (!e || !sim.OkayToMessWith(e->army) || e->dead || e->kind != Entity::Kind::Unit) break;
      Unit* u = static_cast<Unit*>(e);
      const char* k = key.c_str();
      if (!strcasecmp(k, "ToggleScriptBit")) {
        UnitToggleScriptBit(sim, u, std::atol(value.c_str()));
      } else if (!strcasecmp(k, "SetFireState")) {
        lua_pushstring(L, value.c_str());
        CallUnitBinding(sim, u, "SetFireState", 1);
      } else if (!strcasecmp(k, "SetAutoMode") || !strcasecmp(k, "SetAutoSurfaceMode") ||
                 !strcasecmp(k, "SetRepeatQueue") || !strcasecmp(k, "SetPaused")) {
        if (value != "true" && value != "false") break;
        if (!strcasecmp(k, "SetRepeatQueue")) {
          // Unit::SetRepeatQueue 0x6a7450: the script hears of a change (OnStartRepeatQueue / OnStopRepeatQueue),
          // then the flag (unit+0x2a2) is set; the command dispatcher reads it when a factory's order ends
          const bool on = value == "true";
          if (on != u->repeatQueue && u->HasLuaObject())
            sim.CallMethod(L, u, on ? "OnStartRepeatQueue" : "OnStopRepeatQueue", 0);
          u->repeatQueue = on;
        }
        if (!strcasecmp(k, "SetAutoSurfaceMode")) u->autoSurfaceMode = value == "true";
        lua_pushboolean(L, value == "true");
        CallUnitBinding(sim, u, key.c_str(), 1);
      } else if (!strcasecmp(k, "SiloBuildTactical") || !strcasecmp(k, "SiloBuildNuke")) {
        if (value == "add") SiloQueueAmmo(sim, u, !strcasecmp(k, "SiloBuildNuke") ? 1 : 0);
      } else if (!strcasecmp(k, "PlayNoStagingPlatformsVO") || !strcasecmp(k, "PlayBusyStagingPlatformsVO")) {
        // the army's script object runs On<key>() (inferred: the brain)
        if (u->army && u->army->brain) sim.CallMethod(L, u->army->brain, ("On" + key).c_str(), 0);
      } else if (!strcasecmp(k, "CustomName")) {
        lua_pushstring(L, value.c_str());
        CallUnitBinding(sim, u, "SetCustomName", 1);
      } else {
        unhandled("ProcessInfoPair " + key);
      }
      break;
    }
    case 12: {  // IssueCommand 0x749290
      std::vector<uint32_t> ids = in.IdSet();
      StreamIssueData data = in.CmdData();
      int clear = in.U8();
      if (!in.ok || clear > 1) {
        Logf(LogLevel::Error, "command stream: bad IssueCommand at message %u", messages_);
        error_ = true;
        break;
      }
      if (!ValidateNewCommandId(sim, data.id, "IssueCommand")) break;
      std::vector<Unit*> units = CommandableUnits(sim, ids, nullptr);
      if (data.type == static_cast<int>(CommandType::DestroySelf) && !sim.CheatsEnabled()) break;  // 0x74941c
      IssueFromStream(sim, units, data, clear != 0);
      break;
    }
    case 13: {  // IssueFactoryCommand 0x7494b0: rally orders; one unit the source may not order drops the message
      std::vector<uint32_t> ids = in.IdSet();
      StreamIssueData data = in.CmdData();
      int clear = in.U8();
      if (!in.ok || clear > 1) {
        Logf(LogLevel::Error, "command stream: bad IssueFactoryCommand at message %u", messages_);
        error_ = true;
        break;
      }
      if (!ValidateNewCommandId(sim, data.id, "IssueFactoryCommand")) break;
      bool abortAll = false;
      std::vector<Unit*> units = CommandableUnits(sim, ids, &abortAll);
      if (abortAll) break;
      IssueFactoryFromStream(sim, units, data, clear != 0);
      break;
    }
    case 14:
    case 15: {  // Increase / DecreaseCommandCount 0x749680 / 0x7496e0
      uint32_t id = in.U32();
      int delta = static_cast<int32_t>(in.U32());
      auto c = StreamCommand(sim, id);
      if (!c || !OkayCommand(sim, *c)) break;
      if (type == 14) {  // CUnitCommand::IncreaseCount 0x6f1650
        if (delta > 0 && c->type == CommandType::BuildFactory) {
          c->count += delta;
          c->maxCount = std::max(c->maxCount, c->count);
        }
      } else {  // DecreaseCount 0x6f16a0
        c->count = delta < 0 ? 0 : std::max(0, c->count - delta);
        if (c->count == 0) {
          std::vector<Unit*> holders(c->units.begin(), c->units.end());
          std::sort(holders.begin(), holders.end(), [](Unit* x, Unit* y) { return x->id < y->id; });
          for (Unit* u : holders) {
            bool removed = RemoveCommandFromUnit(sim, u, *c);
            if (removed && c->type == CommandType::Upgrade) unhandled("DecreaseCommandCount of an Upgrade");
          }
        }
      }
      break;
    }
    case 16: {  // SetCommandTarget 0x749740
      uint32_t id = in.U32();
      StreamTarget t = in.Target();
      auto c = StreamCommand(sim, id);
      if (!c || !OkayCommand(sim, *c)) break;
      SetCommandTargetFromStream(sim, *c, t);
      // CUnitCommand::SetTarget 0x6e8820: a command with a formation (+0x118) moves it to the target's point
      // (GetTargetPosGun 0x5e2a90 with the flag clear, then the formation's vt+0x08 SetCentre(x, z)) before it tells
      // its listeners (0x6e94a0)
      // GetTargetPosGun gives a point target's point and an entity's gun point; for no target, or an entity target
      // whose weak pointer is empty (the stream named an entity that is not there: 0x5e2ac5..0x5e2ad3), it gives its
      // static "no target" vector, all NaN (0x5e2ba3..0x5e2bc9), and SetCentre leaves the centre (FormationSetCentre)
      if (c->form) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const Vec3 p = t.type == 2 ? c->pos : c->targetId ? CommandPosGun(sim, *c) : Vec3{nan, nan, nan};
        FormationSetCentre(*c, p.x, p.z);
      }
      RetargetMoveTasks(sim, *c);  // CUnitCommand::SetTarget 0x6e8820 -> its listeners (0x6e94a0, event 0)
      RetargetAttackTasks(sim, *c);
      RetargetPatrolTasks(sim, *c);
      RetargetMobileBuildTasks(sim, *c);
      RefreshGuardTasks(sim, *c);
      GuardRepairChildrenCommandEvent(sim, *c, nullptr);
      GuardWalksCommandEvent(sim, *c);
      RetargetUnloadMoves(sim, *c);
      break;
    }
    case 17: {  // SetCommandType 0x749800
      uint32_t id = in.U32();
      uint32_t t = in.U32();
      auto c = StreamCommand(sim, id);
      if (c && OkayCommand(sim, *c)) c->type = static_cast<CommandType>(t);
      break;
    }
    case 18: {  // SetCommandCells 0x749860
      uint32_t id = in.U32();
      auto cells = in.Cells();
      Vec3 p{in.F32(), in.F32(), in.F32()};
      auto c = StreamCommand(sim, id);
      if (!c || !OkayCommand(sim, *c)) break;
      c->cells = cells;
      c->targetId = 0;
      c->targetPoint = -1;
      c->pos = p;
      c->hasPos = true;
      break;
    }
    case 19: {  // RemoveCommandFromQueue 0x749970
      uint32_t id = in.U32();
      Entity* e = sim.FindEntityById(in.U32());
      if (!e || !sim.OkayToMessWith(e->army)) break;
      auto c = StreamCommand(sim, id);
      if (!c || !OkayCommand(sim, *c) || e->kind != Entity::Kind::Unit || e->dead) break;
      if (!RemoveCommandFromUnit(sim, static_cast<Unit*>(e), *c)) unhandled("RemoveCommandFromQueue (factory queue)");
      break;
    }
    case 20: {  // DebugCommand 0x6e4d80 -> Sim::ExecuteDebugCommand 0x749da0
      std::string text = in.Str();
      Vec3 mouse{in.F32(), in.F32(), in.F32()};
      int army = in.U8();
      std::vector<uint32_t> ids = in.IdSet();
      if (!in.ok) break;
      // the ids that are units, with no permission test (0x749e20..0x749e89)
      std::vector<Unit*> units;
      for (uint32_t id : ids) {
        Entity* e = sim.FindEntityById(id);
        if (e && e->kind == Entity::Kind::Unit) units.push_back(static_cast<Unit*>(e));
      }
      sim.ExecuteDebugCommand(L, text, mouse, army, units);
      break;
    }
    case 21: {  // ExecuteLuaInSim 0x749a70: a global function's NAME and one argument (cheats)
      std::string fn = in.Str();
      std::string arg = in.Lua();
      if (!sim.CheatsEnabled()) break;  // 0x749a93
      int top = lua_gettop(L);
      lua_pushcfunction(L, ScriptTraceback);
      lua_getglobal(L, fn.c_str());
      if (!PushSerializedLua(L, arg)) lua_pushnil(L);
      prof::NextSource(prof::kSimCallback);  // (MOHO64_PROFILE only)
      if (lua_pcall(L, 1, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running global lua function '%s':\n%s", fn.c_str(),
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
      lua_settop(L, top);
      break;
    }
    case 22: {  // LuaSimCallback 0x749b60: SimCallbacks.DoCallback(name, data, units or nil)
      std::string name = in.Str();
      std::string data = in.Lua();
      std::vector<uint32_t> ids = in.IdSet();
      int top = lua_gettop(L);
      lua_pushcfunction(L, ScriptTraceback);
      int handler = lua_gettop(L);
      if (!sim.PushImport(L, "/lua/SimCallbacks.lua")) {
        lua_settop(L, top);
        break;
      }
      lua_pushstring(L, "DoCallback");
      lua_gettable(L, -2);
      lua_remove(L, -2);
      lua_pushstring(L, name.c_str());
      if (!PushSerializedLua(L, data)) lua_pushnil(L);
      if (ids.empty()) {
        lua_pushnil(L);
      } else {
        lua_newtable(L);
        int i = 1;
        for (uint32_t id : ids) {
          Entity* e = sim.FindEntityById(id);
          if (!e || e->kind != Entity::Kind::Unit) continue;
          PushObject(L, e);
          lua_rawseti(L, -2, i++);
        }
      }
      prof::NextSource(prof::kSimCallback);  // (MOHO64_PROFILE only)
      if (lua_pcall(L, 3, 0, handler) != 0)
        Logf(LogLevel::Warning, "Error running sim lua callback function '%s':\n%s", name.c_str(),
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
      lua_settop(L, top);
      break;
    }
    case 23:  // EndGame 0x74b100: the next beat still runs, then the game is frozen
      sim.endGame = true;
      break;
    default:
      unhandled(kMsgNames[type]);
      break;
  }
  if (!in.ok) {
    Logf(LogLevel::Error, "command stream: message %u (%s) is malformed", messages_, kMsgNames[type]);
    error_ = true;
  } else if (in.p != payload.size() && type != 2 && type != 4 && type != 5 && type != 6 && type != 23) {
    Logf(LogLevel::Warning, "command stream: message %u (%s): %zu unread byte(s)", messages_, kMsgNames[type],
         payload.size() - in.p);
  }
}

}  // namespace moho
