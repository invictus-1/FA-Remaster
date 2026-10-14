// moho64 -- milestone 1 host: mount the game data the way the original does, then run the
// rules state (blueprint loading) or compile-check every script.
//
//   moho64 --init <init.lua> [--drive c=/host/dir] [--folder NAME=path] [--mods uids.txt]
//          [--log out.log] [--check-lua] [--rules] [--quiet]
//          [--sim <replay> | --session <game configuration .lua> [--seed N]] [--ticks N] [--view] [--orders file] [--sim-lua file]
#include <chrono>
#ifdef MOHO64_CALLGRIND
#include <valgrind/callgrind.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/mount.h"
#include "script/script_state.h"
#include "sim/script_object.h"
#include "sim/command_stream.h"
#include "sim/command_writer.h"
#include "sim/session.h"
#include "sim/sim_view.h"
#include "sim/sim.h"

using namespace moho;

namespace {

struct Options {
  std::string init;
  std::string modsFile;
  std::string logFile;
  std::map<std::string, std::string> folders;  // SHGetFolderPath answers
  bool checkLua = false;
  bool rules = false;
  bool quiet = false;
  std::string replay;   // --sim <replay>: start that session headless
  std::string session;  // --session <file>: start the session a lobby's game configuration describes
  uint32_t seed = 0;    // --seed N: that session's random seed (default: from the clock)
  bool hasSeed = false;
  std::string orders;   // --orders file: orders issued through the command writer at given ticks (testing)
  bool view = false;    // --view: collect what a host reads (sync tables, armies, units) and log a summary
  int ticks = 0;        // --ticks N: run the sim this many ticks after start-up
  std::string simLua;   // --sim-lua file: a host Lua file run in the sim state after the ticks (debugging)
  std::vector<std::string> exec;  // --exec <lua>: run in a fresh rules state after mounting
};

// Compile every .lua file the game can see; report failures.
int CheckLua(Vfs& vfs) {
  ScriptState st(ScriptState::Kind::Rules, &vfs);
  lua_State* L = st.L();
  auto files = vfs.FindFiles("/", "*.lua");
  auto bps = vfs.FindFiles("/", "*.bp");
  files.insert(files.end(), bps.begin(), bps.end());
  int bad = 0;
  for (const auto& f : files) {
    auto src = vfs.ReadFile(f);
    if (!src) continue;
    if (luaL_loadbuffer(L, src->data(), src->size(), vfs.ChunkName(f).c_str()) != 0) {
      ++bad;
      Logf(LogLevel::Warning, "compile error: %s", lua_tostring(L, -1));
    }
    lua_settop(L, 0);
  }
  Logf(LogLevel::Info, "check-lua: %zu files, %d failed to compile", files.size(), bad);
  return bad;
}

int RunRules(Vfs& vfs, const std::vector<std::string>& hookDirs, const std::vector<std::string>& uids) {
  ScriptState st(ScriptState::Kind::Rules, &vfs);
  st.SetHookDirs(hookDirs);
  std::map<std::string, int> counts;
  st.onRegisterBlueprint = [&](lua_State*, const char* kind) { counts[kind]++; };
  SetActiveMods(st, uids);
  bool ok = st.DoScript("/lua/ruleinit.lua");
  std::string summary;
  for (auto& [k, v] : counts) summary += " " + k + "=" + std::to_string(v);
  Logf(LogLevel::Info, "moho64: rules state %s; registered:%s", ok ? "ok" : "FAILED", summary.c_str());
  return ok ? 0 : 1;
}

// --view: what a host that shows the game would have read (sim/sim_view.h, Sim::TakeSync), as log lines.
void LogView(Sim& sim, size_t syncBeats, size_t syncBytes, const std::string& lastSync) {
  Logf(LogLevel::Info, "view: %zu sync tables, %zu bytes in all, the last %zu bytes", syncBeats, syncBytes, lastSync.size());
  {  // factories and their rally queues, as UnitStateView has them at the end of the run
    std::vector<UnitView> units;
    UnitViews(sim, &units);
    size_t factories = 0, withRally = 0;
    std::string sample;
    for (const UnitView& uv : units) {
      UnitStateView s;
      if (!UnitStateOf(sim, uv.id, &s) || !s.hasFactoryQueue) continue;
      ++factories;
      if (s.factoryQueue.empty()) continue;
      ++withRally;
      if (sample.empty()) {
        const CommandView& last = s.factoryQueue.back();
        char b[300];
        std::snprintf(b, sizeof b, "unit %#x %s at %.1f %.1f %.1f: rally queue of %zu, the last type %d at %.1f %.1f %.1f; own queue %zu",
                      uv.id, uv.blueprint.c_str(), uv.position[0], uv.position[1], uv.position[2], s.factoryQueue.size(),
                      last.type, last.position[0], last.position[1], last.position[2], s.queue.size());
        sample = b;
      }
    }
    Logf(LogLevel::Info, "view: %zu units keep a rally queue, %zu of them with something in it", factories, withRally);
    if (!sample.empty()) Logf(LogLevel::Info, "view: %s", sample.c_str());
    std::vector<UnitHiddenBones> hidden;
    HiddenBones(sim, &hidden);
    size_t hiddenBones = 0;
    for (const UnitHiddenBones& h : hidden) hiddenBones += h.bones.size();
    Logf(LogLevel::Info, "view: %zu of %zu units have hidden bones, %zu in all", hidden.size(), units.size(), hiddenBones);
    if (!hidden.empty()) {
      const UnitHiddenBones& h = hidden.front();
      std::string list, bp;
      for (size_t i = 0; i < h.bones.size() && i < 12; ++i) list += " " + std::to_string(h.bones[i]);
      for (const UnitView& uv : units)
        if (uv.id == h.id) bp = uv.blueprint;
      Logf(LogLevel::Info, "view: unit %#x %s hides %zu bones:%s%s", h.id, bp.c_str(), h.bones.size(), list.c_str(),
           h.bones.size() > 12 ? " ..." : "");
    }
  }
  if (!lastSync.empty()) {
    ScriptState st(ScriptState::Kind::User, nullptr);
    lua_State* L = st.L();
    std::string keys;
    if (PushSerializedLua(L, lastSync) && lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2)) {
        if (lua_type(L, -2) == LUA_TSTRING) keys += std::string(" ") + lua_tostring(L, -2);
        lua_pop(L, 1);
      }
      Logf(LogLevel::Info, "view: the last sync table's keys:%s", keys.c_str());
    } else {
      Logf(LogLevel::Error, "view: the last sync table does not read back");
    }
  }
  Logf(LogLevel::Info, "view: scenario info %zu bytes, tick %u", sim.sessionScenario.size(), sim.tick());
  for (const ArmyView& a : ArmyViews(sim)) {
    EconomyTotals e = EconomyTotalsOf(sim, a.index);
    Logf(LogLevel::Info,
         "view: army %d %s \"%s\" faction %d%s%s colours %d/%d rgb %d,%d,%d; mass %.1f of %.0f (+%.2f -%.2f), "
         "energy %.1f of %.0f (+%.2f -%.2f)",
         a.index, a.name.c_str(), a.nickname.c_str(), a.faction, a.human ? " human" : "", a.civilian ? " civilian" : "",
         a.armyColor, a.playerColor, a.rgb[0], a.rgb[1], a.rgb[2], e.stored[1], e.maxStorage[1], e.income[1],
         e.lastUseActual[1], e.stored[0], e.maxStorage[0], e.income[0], e.lastUseActual[0]);
  }
  std::vector<UnitView> units;
  UnitViews(sim, &units);
  Logf(LogLevel::Info, "view: %zu units", units.size());
  for (size_t i = 0; i < units.size() && i < 8; ++i) {
    const UnitView& u = units[i];
    Logf(LogLevel::Info, "view: unit %#x %s army %d at %.1f %.1f %.1f health %.0f/%.0f built %.2f queue %d%s%s", u.id,
         u.blueprint.c_str(), u.army, u.position[0], u.position[1], u.position[2], u.health, u.maxHealth,
         u.fractionComplete, u.queueLength, u.selectable ? "" : " unselectable", u.dead ? " dead" : "");
  }
  // the panels' state of the first three units that have orders, and each army's build restrictions
  int shown = 0;
  for (const UnitView& u : units) {
    if (!u.queueLength || shown >= 3) continue;
    UnitStateView s;
    if (!UnitStateOf(sim, u.id, &s)) continue;
    ++shown;
    std::string q;
    for (const CommandView& c : s.queue) {
      char b[160];
      std::snprintf(b, sizeof b, " [%#x type %d%s%s%s x%d]", c.id, c.type, c.hasPosition ? " pos" : "",
                    c.entity != 0xffffffff ? " entity" : "", c.blueprint.empty() ? "" : (" " + c.blueprint).c_str(), c.count);
      q += b;
    }
    Logf(LogLevel::Info,
         "view: state %#x focus %#x guarded %#x creator %#x fire %d bits %#x%s%s%s%s caps %#x toggles %#x shield %.2f fuel %.2f "
         "silo %d/%d E +%.1f -%.1f M +%.2f -%.2f build rate %.1f queue%s",
         s.id, s.focus, s.guarded, s.creator, s.fireState, s.scriptBits, s.paused ? " paused" : "", s.repeatQueue ? " repeat" : "",
         s.autoMode ? " auto" : "", s.autoSurfaceMode ? " autosurface" : "", s.commandCaps, s.toggleCaps, s.shieldRatio,
         s.fuelRatio, s.tacticalSiloCount, s.nukeSiloCount, s.produced[0], s.consumed[0], s.produced[1], s.consumed[1],
         s.buildRate, q.c_str());
  }
  for (const ArmyView& a : ArmyViews(sim)) {
    const std::vector<std::string> r = RestrictedBlueprints(sim, a.index);
    if (!r.empty()) Logf(LogLevel::Info, "view: army %d may not build %zu blueprints (first %s)", a.index, r.size(), r.front().c_str());
  }
  {  // a unit's own build restrictions (a commander before its upgrades, a factory before its own)
    size_t with = 0;
    std::string sample;
    for (const UnitView& u : units) {
      const std::vector<std::string> r = UnitRestrictedBlueprints(sim, u.id);
      if (r.empty()) continue;
      if (++with <= 3) {
        char buf[200];
        std::snprintf(buf, sizeof buf, " unit %#x %s: %zu (first %s, may build it %d)", u.id, u.blueprint.c_str(), r.size(),
                      r.front().c_str(), UnitMayBuild(sim, u.id, r.front()) ? 1 : 0);
        sample += buf;
      }
    }
    Logf(LogLevel::Info, "view: %zu units have build restrictions of their own;%s", with, sample.c_str());
  }
  {  // what each army knows of the others' units
    std::vector<UnitView> uv;
    UnitViews(sim, &uv);
    for (const ArmyView& a : ArmyViews(sim)) {
      size_t others = 0, held = 0, seen = 0, radar = 0;
      for (const UnitView& u : uv) {
        if (u.army == a.index) continue;
        const uint32_t r = ReconOf(sim, a.index, u.id);
        if (r == 0xF) continue;  // (an ally's)
        ++others;
        if (r & kReconHeld) ++held;
        if (r & kReconSeenNow) ++seen;
        if (r & (kReconRadar | kReconSonar | kReconOmni)) ++radar;
      }
      if (others)
        Logf(LogLevel::Info, "view: army %d knows of %zu of %zu other units (%zu seen now, %zu on radar, sonar or omni)", a.index,
             held, others, seen, radar);
    }
  }
  {  // the display part of the blueprints
    std::vector<BlueprintDisplay> bd;
    BlueprintDisplays(sim, &bd);
    size_t lods = 0, kinds[3] = {0, 0, 0};
    for (const BlueprintDisplay& d : bd) {
      lods += d.lods.size();
      ++kinds[d.kind];
    }
    Logf(LogLevel::Info, "view: %zu blueprint displays (%zu units, %zu projectiles, %zu props), %zu levels", bd.size(), kinds[0],
         kinds[1], kinds[2], lods);
    for (const BlueprintDisplay& d : bd)
      if (d.kind == 0 && !d.lods.empty()) {
        Logf(LogLevel::Info, "view: %s scale %.3f, %zu levels, first %s / %s / %s cutoff %.0f; icon %s priority %d fade %.0f, selection %.2f x %.2f",
             d.id.c_str(), d.uniformScale, d.lods.size(), d.lods[0].mesh.c_str(), d.lods[0].albedo.c_str(), d.lods[0].shader.c_str(),
             d.lods[0].cutoff, d.icon.c_str(), d.iconSortPriority, d.iconFadeInZoom, d.selectionSizeX, d.selectionSizeZ);
        break;
      }
  }
  // a build preview's question, asked around the first unit: a land factory 10 and 15 to its east
  std::vector<UnitView> uv;
  UnitViews(sim, &uv);
  if (!uv.empty()) {
    for (float dx : {10.1f, 15.1f}) {
      const float at[3] = {uv[0].position[0] + dx, uv[0].position[1], uv[0].position[2] + 5.3f};
      float site[3];
      bool ok = false;
      if (BuildSiteOf(sim, "ueb0101", at, site, &ok))
        Logf(LogLevel::Info, "view: ueb0101 ordered at %.1f %.1f would stand at %.1f %.2f %.1f, can build %d", at[0], at[2],
             site[0], site[1], site[2], ok ? 1 : 0);
    }
  }
}

// Run a session headless: `stream` is the replay's bytes (its command stream starts at the header's
// streamOffset), or null for a session launched from a game configuration, which has no commands.
int RunSession(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs,
               const std::vector<std::string>& uids, const SessionDescription& session, const std::string* stream_) {
  const ReplayHeader* header = &session.header;
  static const std::string none;
  const std::string* data = stream_ ? stream_ : &none;
  Sim sim(&vfs, hookDirs, uids);
  sim.sessionMods = header->mods;
  auto t0 = std::chrono::steady_clock::now();
  if (!sim.LoadRules()) return 1;
  auto t1 = std::chrono::steady_clock::now();
  bool ok = sim.Start(session);
  auto t2 = std::chrono::steady_clock::now();
  Logf(LogLevel::Info, "moho64: sim start-up %s", ok ? "ok" : "FAILED");
#ifdef MOHO64_CALLGRIND
  CALLGRIND_START_INSTRUMENTATION;  // profile build: only the ticks are measured
#endif
  // debugging aid: MOHO64_START_LUA=<host file> runs that Lua in the sim state right after start-up (e.g. to wrap
  // a script function with logging); it changes nothing unless the file does
  if (ok && getenv("MOHO64_START_LUA")) {
    if (auto code = hostfs::ReadFile(getenv("MOHO64_START_LUA"))) sim.RunString(*code, "@moho64_start_lua");
    else Logf(LogLevel::Error, "cannot read %s", getenv("MOHO64_START_LUA"));
  }
  // The command stream (sim/command_stream.cpp): the messages before each Advance are applied, then the beat
  // runs (or, while paused / after EndGame, only its end-of-beat part). --ticks counts game ticks.
  CommandStream stream(*data, header->streamOffset);
  bool playStream = stream_ && !getenv("MOHO64_NO_STREAM");
  int done = 0, paused = 0;
  sim.collectSync = o.view;
  sim.collectEffects = o.view;
  sim.collectSounds = o.view;
  sim.collectProps = o.view;
  size_t propCount[3] = {0, 0, 0}, propMapOnes[3] = {0, 0, 0}, entMax = 0, entShieldMax = 0, unitMeshMax = 0;
  std::string propSample[3], entSample[2], unitMeshSample;
  std::map<std::string, size_t> entMeshes;
  double propSeconds = 0;
  size_t sndCount[3] = {0, 0, 0};
  std::string sndSample[2];
  size_t syncBeats = 0, syncBytes = 0;
  size_t fxCount[3] = {0, 0, 0};
  std::map<std::string, size_t> fxNames;
  std::string fxSample;
  size_t viewMax[4] = {0, 0, 0, 0};
  std::string viewSample[3];
  std::string lastSync;
  // --orders: lines "<tick> issue <command type number> <unit id>[,<unit id>...] [<x> <y> <z>] [queue]",
  // "<tick> pause", "<tick> resume", "<tick> endgame", "<tick> callback <name>", "<tick> lua <one line of Lua run in
  // the sim state: a test building its case, not an order>"; <tick> counts beats, paused ones included, and
  // the order is applied before the beat that follows; all as source 0. '#' starts a comment.
  std::multimap<uint32_t, std::string> orders;
  if (!o.orders.empty()) {
    std::ifstream f(o.orders);
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      size_t sp = line.find(' ');
      if (sp != std::string::npos) orders.insert({static_cast<uint32_t>(std::atoi(line.c_str())), line.substr(sp + 1)});
    }
  }
  CommandWriter writer(0);
  std::map<std::string, int> ordersUnhandled;
  auto giveOrders = [&] {
    auto range = orders.equal_range(static_cast<uint32_t>(done + paused));
    for (auto it = range.first; it != range.second; ++it) {
      char what[32] = "", ids[256] = "", tail[32] = "";
      int type = 0;
      float x = 0, y = 0, z = 0;
      const char* text = it->second.c_str();
      if (std::sscanf(text, "%31s", what) != 1) continue;
      if (!std::strcmp(what, "pause")) writer.RequestPause();
      else if (!std::strcmp(what, "resume")) writer.Resume();
      else if (!std::strcmp(what, "endgame")) writer.EndGame();  // one more beat runs, then the game stands
      else if (!std::strcmp(what, "callback") && std::sscanf(text, "%*s %255s", ids) == 1) writer.LuaSimCallback(ids, {}, {});
      else if (!std::strcmp(what, "lua")) sim.RunString(text + 3, "@orders");  // a test's own Lua, straight into the sim state
      else if (!std::strcmp(what, "issue")) {
        int n = std::sscanf(text, "%*s %d %255s %f %f %f %31s", &type, ids, &x, &y, &z, tail);
        if (n < 2) continue;
        std::vector<uint32_t> units;
        for (char* tok = std::strtok(ids, ","); tok; tok = std::strtok(nullptr, ","))
          units.push_back(static_cast<uint32_t>(std::strtoul(tok, nullptr, 0)));
        StreamIssueData d;
        d.type = type;
        if (n >= 5) {
          d.target.type = 2;
          d.target.pos = {x, y, z};
        }
        uint32_t id = writer.IssueCommand(units, d, std::strcmp(tail, "queue") != 0);
        Logf(LogLevel::Info, "orders: tick %u: command %#x type %d for %zu unit(s)", sim.tick(), id, type, units.size());
      }
    }
    if (!writer.empty() && !ApplyCommands(sim, writer.Take(), &ordersUnhandled)) Logf(LogLevel::Error, "orders: a message was malformed");
  };
  auto takeSync = [&] {
    for (auto& b : sim.TakeSync()) {
      ++syncBeats;
      syncBytes += b.size();
      // the final score (FAF's score.lua, sent by its game-over listener): which sync table carried it
      if (o.view && b.find("ScoreAccum") != std::string::npos)
        Logf(LogLevel::Info, "view: sync table %zu (tick %u, %zu bytes) carries ScoreAccum%s", syncBeats, sim.tick(), b.size(),
             b.find("StatsToSend") != std::string::npos ? " and StatsToSend" : "");
      lastSync = std::move(b);
    }
  };
  while (ok && done < o.ticks) {
    if (playStream && !stream.AtEnd()) {
      if (!stream.ApplyUntilAdvance(sim)) {
        Logf(LogLevel::Info, "moho64: the replay's command stream ended after %u beats (tick %u)", stream.beats(),
             sim.tick());
        if (!getenv("MOHO64_PAST_END")) break;
      }
    }
    giveOrders();
    if (sim.Paused()) {
      sim.PausedBeat();
      takeSync();
      ++paused;
      if (sim.frozen && (stream.AtEnd() || !playStream)) break;
      continue;
    }
    sim.Tick();
    takeSync();
    ++done;
    if (o.view) {  // the live frame's extras: the most seen at one beat, and one sample of each
      std::vector<UnitPose> poses;
      std::vector<ProjectileView> shots;
      std::vector<BeamView> beamViews;
      UnitPoses(sim, &poses);
      ProjectileViews(sim, &shots);
      BeamViews(sim, &beamViews);
      std::vector<EffectEvent> fx;
      EffectEvents(sim, &fx);
      for (const EffectEvent& ev : fx) {
        ++fxCount[ev.kind];
        ++fxNames[ev.name];
        if (fxSample.empty() && ev.kind == EffectEvent::Create && ev.entity != 0xffffffff && ev.bone >= 0) {
          char fb[320];
          std::snprintf(fb, sizeof fb, "tick %u effect %u %s %s army %d on %#x bone %d at %.2f %.2f %.2f", sim.tick(), ev.id,
                        ev.name.c_str(), ev.blueprint.c_str(), ev.army, ev.entity, ev.bone, ev.position[0], ev.position[1],
                        ev.position[2]);
          fxSample = fb;
        }
      }
      std::vector<SoundEvent> snd;
      SoundEvents(sim, &snd);
      for (const SoundEvent& ev : snd) {
        ++sndCount[ev.kind == SoundEvent::Play ? 0 : (ev.cue.empty() && ev.rumbleCue.empty() ? 2 : 1)];
        if (sndSample[ev.kind].empty() && !ev.cue.empty()) {
          char sb[320];
          std::snprintf(sb, sizeof sb, "tick %u %s %s / %s on %#x (army %d) at %.1f %.1f %.1f", sim.tick(),
                        ev.kind == SoundEvent::Play ? "sound" : "loop", ev.bank.c_str(), ev.cue.c_str(), ev.entity, ev.army,
                        ev.position[0], ev.position[1], ev.position[2]);
          sndSample[ev.kind] = sb;
        }
      }
      {  // scenery and script-made entities
        std::vector<PropEvent> pe;
        const auto t0 = std::chrono::steady_clock::now();
        PropEvents(sim, &pe);
        propSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (const PropEvent& ev : pe) {
          ++propCount[ev.kind];
          if (ev.mapIndex >= 0) ++propMapOnes[ev.kind];
          if (propSample[ev.kind].empty()) {
            char pb[400];
            static const char* const kNames[] = {"made", "changed", "gone"};
            std::snprintf(pb, sizeof pb, "tick %u prop %#x %s (map index %d) %s mesh '%s' at %.2f %.2f %.2f quat %.3f %.3f %.3f %.3f scale %.2f",
                          sim.tick(), ev.id, kNames[ev.kind], ev.mapIndex, ev.blueprint.c_str(), ev.mesh.c_str(),
                          ev.position[0], ev.position[1], ev.position[2], ev.orientation[0], ev.orientation[1],
                          ev.orientation[2], ev.orientation[3], ev.scale[0]);
            propSample[ev.kind] = pb;
          }
        }
        std::vector<EntityView> ents;
        EntityViews(sim, &ents);
        size_t sh = 0;
        for (const EntityView& ev : ents) {
          if (ev.shield) ++sh;
          if (entMeshes.size() < 40 || entMeshes.count(ev.mesh)) ++entMeshes[ev.mesh];
          std::string& sample = entSample[ev.shield ? 1 : 0];
          if (sample.empty()) {
            char eb[400];
            std::snprintf(eb, sizeof eb, "tick %u %s %#x mesh '%s' army %d at %.2f %.2f %.2f scale %.2f draw scale %.2f on %#x bone %d viz %d %d %d %d",
                          sim.tick(), ev.shield ? "shield" : "entity", ev.id, ev.mesh.c_str(), ev.army, ev.position[0],
                          ev.position[1], ev.position[2], ev.scale[0], ev.drawScale, ev.parent, ev.parentBone, ev.vizFocus,
                          ev.vizAllies, ev.vizEnemies, ev.vizNeutrals);
            sample = eb;
          }
        }
        entMax = std::max(entMax, ents.size());
        entShieldMax = std::max(entShieldMax, sh);
        std::vector<UnitView> uv;
        UnitViews(sim, &uv);
        size_t withMesh = 0;
        for (const UnitView& v : uv)
          if (!v.mesh.empty()) {
            ++withMesh;
            if (unitMeshSample.empty()) {
              char ub[300];
              std::snprintf(ub, sizeof ub, "tick %u unit %#x %s has mesh '%s' set", sim.tick(), v.id, v.blueprint.c_str(),
                            v.mesh.c_str());
              unitMeshSample = ub;
            }
          }
        unitMeshMax = std::max(unitMeshMax, withMesh);
      }
      size_t bones = 0;
      for (const auto& up : poses) bones += up.bones.size();
      char buf[256];
      if (poses.size() > viewMax[0]) {
        viewMax[0] = poses.size();
        viewMax[1] = bones;
        const UnitPose& up = poses.front();
        const BonePose& b = up.bones.front();
        std::snprintf(buf, sizeof buf, "tick %u unit %#x: %zu bones, bone %d at %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f",
                      sim.tick(), up.id, up.bones.size(), b.bone, b.position[0], b.position[1], b.position[2],
                      b.orientation[0], b.orientation[1], b.orientation[2], b.orientation[3]);
        viewSample[0] = buf;
      }
      if (shots.size() > viewMax[2]) {
        viewMax[2] = shots.size();
        const ProjectileView& pv = shots.front();
        std::snprintf(buf, sizeof buf, "tick %u projectile %#x %s army %d at %.1f %.1f %.1f speed %.1f", sim.tick(), pv.id,
                      pv.blueprint.c_str(), pv.army, pv.position[0], pv.position[1], pv.position[2],
                      std::sqrt(pv.velocity[0] * pv.velocity[0] + pv.velocity[1] * pv.velocity[1] +
                                pv.velocity[2] * pv.velocity[2]));
        viewSample[1] = buf;
      }
      if (beamViews.size() > viewMax[3]) {
        viewMax[3] = beamViews.size();
        const BeamView& bv = beamViews.front();
        std::snprintf(buf, sizeof buf, "tick %u beam %#x %s unit %#x from %.1f %.1f %.1f to %.1f %.1f %.1f", sim.tick(),
                      bv.id, bv.blueprint.c_str(), bv.unit, bv.start[0], bv.start[1], bv.start[2], bv.end[0], bv.end[1],
                      bv.end[2]);
        viewSample[2] = buf;
      }
    }
  }
  if (ok && o.view) {
    Logf(LogLevel::Info, "view: most at one beat: %zu posed units (%zu bones), %zu projectiles, %zu beams", viewMax[0],
         viewMax[1], viewMax[2], viewMax[3]);
    for (const std::string& line : viewSample)
      if (!line.empty()) Logf(LogLevel::Info, "view: %s", line.c_str());
    Logf(LogLevel::Info, "view: effect events: %zu made, %zu calls on them, %zu ended", fxCount[0], fxCount[1], fxCount[2]);
    for (const auto& [name, n] : fxNames) Logf(LogLevel::Info, "view:   %s x%zu", name.c_str(), n);
    if (!fxSample.empty()) Logf(LogLevel::Info, "view: %s", fxSample.c_str());
    Logf(LogLevel::Info, "view: sound events: %zu played once, %zu loops set, %zu loops ended", sndCount[0], sndCount[1],
         sndCount[2]);
    for (const std::string& line : sndSample)
      if (!line.empty()) Logf(LogLevel::Info, "view: %s", line.c_str());
    Logf(LogLevel::Info, "view: prop events: %zu made, %zu changed (%zu of the map's), %zu gone (%zu of the map's); "
         "%zu props now; the list took %.3f ms a beat",
         propCount[0], propCount[1], propMapOnes[1], propCount[2], propMapOnes[2], sim.props.size(),
         done ? propSeconds * 1000.0 / done : 0.0);
    for (const std::string& line : propSample)
      if (!line.empty()) Logf(LogLevel::Info, "view: %s", line.c_str());
    Logf(LogLevel::Info, "view: script entities with a mesh: most at one beat %zu (%zu shields); units with a mesh set: %zu",
         entMax, entShieldMax, unitMeshMax);
    for (const auto& [name, n] : entMeshes) Logf(LogLevel::Info, "view:   %s (entity beats %zu)", name.c_str(), n);
    for (const std::string& line : entSample)
      if (!line.empty()) Logf(LogLevel::Info, "view: %s", line.c_str());
    if (!unitMeshSample.empty()) Logf(LogLevel::Info, "view: %s", unitMeshSample.c_str());
  }
  for (auto& [what, n] : ordersUnhandled) Logf(LogLevel::Warning, "orders: %s not carried out (x%d)", what.c_str(), n);
  if (ok && o.view) LogView(sim, syncBeats, syncBytes, lastSync);
  if (stream_ && playStream)
    Logf(LogLevel::Info, "moho64: command stream: %u messages, %u beats (%d paused)%s", stream.messages(),
         stream.beats(), paused, stream.hadError() ? ", with errors" : "");
  for (auto& [what, n] : stream.unhandled())
    Logf(LogLevel::Warning, "moho64: command stream: %s not carried out (x%d)", what.c_str(), n);
#ifdef MOHO64_CALLGRIND
  CALLGRIND_STOP_INSTRUMENTATION;
#endif
  auto t3 = std::chrono::steady_clock::now();
  if (o.ticks) Logf(LogLevel::Info, "moho64: ran %d ticks", done);
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  Logf(LogLevel::Info, "moho64: timing: rules %.2f s, sim start-up %.2f s, %d ticks %.2f s (%.2f ms/tick)",
       secs(t0, t1), secs(t1, t2), done, secs(t2, t3), done ? secs(t2, t3) * 1000.0 / done : 0.0);
  if (ok && !o.simLua.empty()) {
    if (auto code = hostfs::ReadFile(o.simLua)) sim.RunString(*code, "@" + o.simLua);
    else Logf(LogLevel::Error, "cannot read %s", o.simLua.c_str());
  }
  ReportStubCalls();
  return ok ? 0 : 1;
}

int RunSim(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs, const std::vector<std::string>& uids) {
  auto data = hostfs::ReadFile(o.replay);
  if (!data) {
    Logf(LogLevel::Error, "cannot read replay %s", o.replay.c_str());
    return 1;
  }
  std::string err;
  auto header = ReadReplayHeader(*data, &err);
  if (!header) {
    Logf(LogLevel::Error, "%s", err.c_str());
    return 1;
  }
  SessionDescription session;
  session.header = *header;
  session.replay = true;
  return RunSession(o, vfs, hookDirs, uids, session, &*data);
}

// --session <file>: the game configuration a lobby hands over at Launch (`GameConfig = { ... }`), started the
// way CLobby::LaunchGame starts it (sim/session.h). The mods are the configuration's GameMods unless --mods
// names a list.
int RunGameConfig(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs,
                  std::vector<std::string> uids) {
  SessionDescription session;
  {
    ScriptState st(ScriptState::Kind::User, &vfs);
    st.SetHookDirs(hookDirs);
    lua_State* L = st.L();
    std::string err;
    if (!PushGameConfigFile(L, o.session, &err)) {
      Logf(LogLevel::Error, "moho64: %s", err.c_str());
      return 1;
    }
    LaunchParams params;
    params.seed = o.hasSeed ? o.seed
                            : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!SessionFromGameConfig(L, -1, params, &session, &err)) {
      Logf(LogLevel::Error, "moho64: launch failed: %s", err.c_str());
      return 1;
    }
    if (o.modsFile.empty()) {
      uids.clear();
      lua_getfield(L, -1, "GameMods");
      for (int i = 1; lua_istable(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        lua_getfield(L, -1, "uid");
        if (lua_isstring(L, -1)) uids.push_back(lua_tostring(L, -1));
        lua_pop(L, 2);
      }
    }
  }
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64: session %s: %zu armies, %zu command sources, focus army %d, seed %u, cheats %s",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, h.cheats ? "on" : "off");
  return RunSession(o, vfs, hookDirs, uids, session, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--init") o.init = next();
    else if (a == "--drive") {
      std::string v = next();  // c=/path
      if (v.size() > 2 && v[1] == '=') hostfs::MapDrive(v[0], v.substr(2));
    } else if (a == "--map") {
      std::string v = next();  // "C:/Windows/prefix=/host/dir"
      size_t eq = v.rfind('=');
      if (eq != std::string::npos) hostfs::MapPrefix(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--folder") {
      std::string v = next();
      size_t eq = v.find('=');
      if (eq != std::string::npos) o.folders[v.substr(0, eq)] = v.substr(eq + 1);
    } else if (a == "--mods") o.modsFile = next();
    else if (a == "--log") o.logFile = next();
    else if (a == "--check-lua") o.checkLua = true;
    else if (a == "--rules") o.rules = true;
    else if (a == "--quiet") o.quiet = true;
    else if (a == "--exec") o.exec.push_back(next());
    else if (a == "--sim") o.replay = next();
    else if (a == "--session") o.session = next();
    else if (a == "--seed") {
      o.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 0));
      o.hasSeed = true;
    } else if (a == "--view") o.view = true;
    else if (a == "--orders") o.orders = next();
    else if (a == "--ticks") o.ticks = std::atoi(next().c_str());
    else if (a == "--sim-lua") o.simLua = next();
    else if (a == "--raw-blueprints") SimBlueprints::reflect = false;
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (!o.logFile.empty()) LogOpenFile(o.logFile);
  if (o.quiet) LogSetEcho(false);
  if (o.init.empty()) {
    std::fprintf(stderr, "usage: moho64 --init <init.lua> [--drive c=/dir] [--mods uids.txt] [--check-lua] [--rules]\n");
    return 2;
  }
  Vfs vfs;
  std::vector<std::string> hookDirs;
  if (!MountFromInitFile(o.init, o.folders, vfs, hookDirs)) return 1;
  Logf(LogLevel::Info, "moho64: %zu search paths, %zu files", vfs.MountCount(), vfs.FileCount());
  std::vector<std::string> uids;
  if (!o.modsFile.empty()) {
    std::ifstream f(o.modsFile);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty() && line[0] != '#') uids.push_back(line);
    }
  }
  int rc = 0;
  for (const auto& code : o.exec) {
    ScriptState st(ScriptState::Kind::Rules, &vfs);
    st.SetHookDirs(hookDirs);
    SetActiveMods(st, uids);
    rc |= st.DoString(code, "=exec") ? 0 : 1;
  }
  if (o.checkLua) CheckLua(vfs);  // informational: some mods ship broken files
  if (o.rules) rc |= RunRules(vfs, hookDirs, uids);
  if (!o.replay.empty()) rc |= RunSim(o, vfs, hookDirs, uids);
  if (!o.session.empty()) rc |= RunGameConfig(o, vfs, hookDirs, uids);
  return rc;
}
