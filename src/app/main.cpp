// moho64 -- milestone 1 host: mount the game data the way the original does, then run the
// rules state (blueprint loading) or compile-check every script.
//
//   moho64 --init <init.lua> [--drive c=/host/dir] [--folder NAME=path] [--mods uids.txt]
//          [--log out.log] [--check-lua] [--rules] [--quiet]
//          [--sim <replay> | --session <game configuration .lua> [--seed N]] [--ticks N] [--view] [--orders file] [--sim-lua file]
//          [--twice | --together [--together-lag N] [--together-threads <n|cores>]] [--focus N]
//          [--then-session <game configuration .lua>]
//          [--net host:<port> | join:<port> [--net-client N] [--net-delay N] [--net-stream-out file]]
//   moho64 --init <init.lua> [--mods uids.txt] [--maps folder] --scan-moments <replay> [--scan-out file] [--ticks N]
//          [--scan-off] [--scan-threads N] [--scan-cancel-at tick] [--scan-mount <folder> <mount point>]...
//          [--scan-skip <folder>]...
#include <algorithm>
#include <atomic>
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
#include <thread>
#include <vector>

#include "core/crash.h"
#include "core/hostfs.h"
#include "core/fpenv.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/workers.h"
#include "core/vfs.h"
#include "net/beat_exchange_net.h"
#include "script/mount.h"
#include "script/script_state.h"
#include "sim/script_object.h"
#include "sim/command_stream.h"
#include "sim/command_writer.h"
#include "sim/fingerprint.h"
#include "sim/lockstep.h"
#include "sim/moments.h"
#include "sim/session.h"
#include "sim/sim_view.h"
#include "sim/sim.h"
#include "sim/terrain.h"

using namespace moho;

namespace {

// MOHO64_LAYOUT_SHIFT=<n> (tools/twocopies.py gives it to one of its two copies; docs/two-copies.md): before anything
// else, take and keep n rounds of odd-sized blocks of the heaps the engine uses: the C runtime's (malloc), new's (the
// engine's own objects: mimalloc's, src/app/new_delete.cpp, or the C runtime's in a build without it) and the Lua
// heap's (mimalloc, reached through a Lua state of its own that is never closed, so the sim state's own byte count
// and collector are not touched). Every later block then lands at another
// address than in a copy without it, so whatever goes by addresses (Lua's hash of table and userdata keys, a
// container ordered by pointer) shows as a difference between the two copies even if Windows happened to lay both
// processes out alike. Nothing the simulation computes may change with it.
void ShiftLayout(int rounds) {
  static std::vector<void*> kept;
  lua_State* L = lua_open();
  lua_newtable(L);
  int k = 0;
  for (int r = 0; r < rounds; ++r)
    for (size_t size = 1 + r; size < 4096; size += 1 + size / 8) {
      kept.push_back(std::malloc(size));
      kept.push_back(::operator new(size));
      lua_newuserdata(L, size);
      lua_rawseti(L, -2, ++k);
    }
  std::fprintf(stderr, "moho64: layout shifted by %d rounds (%zu blocks in each heap)\n", rounds, kept.size());
}

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
  std::string stream;   // --stream file: with --session, a played game's command stream to play back
  std::vector<std::string> moreMaps;  // --maps folder: mounted at /maps beside the game's own
  bool view = false;    // --view: collect what a host reads (sync tables, armies, units) and log a summary
  int ticks = 0;        // --ticks N: run the sim this many ticks after start-up
  std::string simLua;   // --sim-lua file: a host Lua file run in the sim state after the ticks (debugging)
  std::vector<std::string> exec;  // --exec <lua>: run in a fresh rules state after mounting
  bool twice = false;   // --twice: play the session a second time in the same process (RunTimes)
  // --then-session <file>: after the --session game, a second game from this configuration in the same process (same
  // seed, file system, --ticks and --orders), as the interface plays its next game; its record and fingerprint get
  // ".then". For what one game leaves behind for the next, e.g. our own AI's scripts (src/ai/scripts.h) only in the
  // game that has it.
  std::string thenSession;
  bool together = false;  // --together: two copies of the session at once in the same process (RunTimes)
  int togetherLag = 100;  // --together-lag N: the second copy starts when the first has played N beats
  int togetherThreads = 0;  // --together-threads <n|cores>: the second copy's worker pool (0: as MOHO64_THREADS)
  // --focus N: the army the session looks through from its launch on (1-based; 0 or -1: none, an observer's view),
  // in place of the launch's own (a replay: none; a lobby game: the local player's army). Each player's copy of a
  // multiplayer game runs the simulation with its own focus army (the scripts' Sync and NoteFocusArmyChanged go by
  // it): tools/twocopies.py gives each copy its own to see whether the game stays the same.
  bool hasFocus = false;
  int focus = -1;
  // /map <name or scenario path> [/diff N] [/faction N]: a campaign operation started as the original's command
  // line starts it (FAF SinglePlayerLaunch.StartCommandLineSession -> SetupCampaignSession ->
  // LaunchSinglePlayerSession); --seed N stands in for the original's timer seed
  std::string spMap;
  int spDiff = 2;
  int spFaction = 1;
  // --scan-moments <replay>: the replay timeline's scan (sim/moments.h) run as the interface would run it, on a
  // thread of its own, its moments written as JSON lines (to --scan-out <file>, else stdout) as they come in, then
  // the fight buckets. --scan-off plays the scan's copy without looking (a check: its MOHO64_FINGERPRINT + ".scan"
  // must be the same as the looking one's), --scan-threads N its worker pool (default 1), --scan-cancel-at T sets
  // the cancel flag once the scan reports tick T (a check of the cancel); --ticks N stops it after N ticks.
  // --scan-mount / --scan-skip: ScanArgs::mounts / skipMounts, as moho64ui fills them (its M28 variant).
  std::string scanReplay;
  std::string scanOut;
  bool scanOff = false;
  int scanThreads = 1;
  int scanCancelAt = 0;
  std::vector<std::pair<std::string, std::string>> scanMounts;
  std::vector<std::string> scanSkips;
  // --net host:<port> | join:<port>: a test of a network game (sim/lockstep.h) over the interface's transport
  // (ui/net/beat_exchange_net.h, its TCP connector on 127.0.0.1): two copies of moho64 play one --session (the same
  // file and --seed), the host as client 0, the other as client 1 (--net-client 1), each speaking for the command
  // source of its own index. Each copy's --orders are its own: they reach every copy through the lockstep instead of
  // going straight into its sim, and run --net-delay beats later (default 5: the original's net_Lag, 500 ms, at
  // normal speed); its "lua" lines still run in its own sim only (they would desync the copies), its "focus" lines
  // are its own by nature. --net-stream-out <file>: every beat's merged orders and an Advance (what --stream plays
  // back; the original's multiplayer replays hold the same: each client's part led by its SetCommandSource).
  // An --orders line "<tick> claim <source> issuegroup ..." / "... issue ..." writes that order as another command
  // source (a test: the lockstep must drop it). MOHO64_NET_CORRUPT_DIGEST=<beat>: this copy's checksum of that beat
  // is changed (a test: both copies must report the desync). MOHO64_NET_EJECT=<client>:<beat>: that client is left
  // out from that beat on (a test: its command sources are terminated).
  std::string netMode;
  uint16_t netPort = 0;
  int netClient = 0;
  int netDelay = 5;
  std::string netStreamOut;
};

// --net: one copy's side of a network game (Options::netMode).
struct NetGame {
  std::unique_ptr<net::NetBeatExchange> ex;
  std::unique_ptr<Lockstep> lockstep;
  uint8_t local = 0, peer = 1;
  uint32_t delay = 5;
  std::FILE* streamOut = nullptr;
  std::string turn, rogue;  // this turn's orders (and the --orders "claim" test's, written as another source)
  std::map<uint32_t, std::string> sentFor;  // beat -> this copy's orders sent for it (the input delay's check)
  uint32_t delayOk = 0, delayWrong = 0, desyncs = 0, corruptAt = 0;
  uint8_t ejectWho = 0;
  uint32_t ejectFrom = 0;  // (MOHO64_NET_EJECT; the transport stops sending to a client once it is ejected, so the
                           // eject is made when that beat comes)
  double waitSeconds = 0;
  ~NetGame() {
    if (streamOut) std::fclose(streamOut);
  }

  // Connects to the other copy (the host listens, the other connects). Once the connection is made the host sends a
  // message of this test's own (100) and waits for the other's first beats before sending any of its own, so that no
  // message of the game is among the events polled here, before the exchange owns the connector.
  static std::unique_ptr<NetGame> Connect(const Options& o) {
    auto g = std::make_unique<NetGame>();
    const bool host = o.netMode == "host";
    g->local = static_cast<uint8_t>(o.netClient);
    g->peer = g->local == 0 ? 1 : 0;
    g->delay = static_cast<uint32_t>(o.netDelay);
    if (!net::NetStartup()) return nullptr;
    auto conn = net::MakeConnector("TCP", host ? o.netPort : 0);
    if (!conn) {
      Logf(LogLevel::Error, "net: cannot open a TCP port");
      return nullptr;
    }
    net::Connector* raw = conn.get();
    const net::Address hostAt{0x7f000001u, o.netPort};
    int id = host ? -1 : raw->Connect(hostAt);
    bool made = false, go = host;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    std::vector<net::ConnEvent> ev;
    while (!(made && go) && std::chrono::steady_clock::now() < stop) {
      ev.clear();
      raw->Poll(&ev);
      for (const auto& e : ev) {
        if (e.kind == net::ConnEvent::Made) {
          made = true;
          id = e.conn;
        } else if (e.kind == net::ConnEvent::Failed && !host) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          id = raw->Connect(hostAt);  // (the host is not listening yet)
        } else if (e.kind == net::ConnEvent::Received && e.msg.type == 100) {
          go = true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!made || !go) {
      Logf(LogLevel::Error, "net: no connection to the other copy (port %u)", o.netPort);
      return nullptr;
    }
    g->ex = std::make_unique<net::NetBeatExchange>(std::move(conn), g->local,
                                                   std::vector<net::GameClient>{net::GameClient{g->peer, id}});
    // (client k speaks for command source k: the session's k-th human owner, sim/session.cpp)
    std::vector<LockstepClient> clients{LockstepClient{0, {0}}, LockstepClient{1, {1}}};
    g->lockstep = std::make_unique<Lockstep>(g->ex.get(), g->local, clients, g->delay);
    if (host) {
      raw->Send(id, net::Message{100, std::string()});
      while (g->ex->ReceivedBeats(g->peer) == 0 && std::chrono::steady_clock::now() < stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g->lockstep->Start();
    if (!o.netStreamOut.empty() && !(g->streamOut = std::fopen(o.netStreamOut.c_str(), "wb")))
      Logf(LogLevel::Error, "net: cannot write %s", o.netStreamOut.c_str());
    if (const char* c = getenv("MOHO64_NET_CORRUPT_DIGEST")) g->corruptAt = static_cast<uint32_t>(std::atoi(c));
    // MOHO64_NET_EJECT=<client>:<beat>: that client left out from that beat on (the disconnect dialog's eject; a test
    // of the lockstep terminating its sources)
    if (const char* e = getenv("MOHO64_NET_EJECT"); e && std::strchr(e, ':')) {
      g->ejectWho = static_cast<uint8_t>(std::atoi(e));
      g->ejectFrom = static_cast<uint32_t>(std::atoi(std::strchr(e, ':') + 1));
    }
    Logf(LogLevel::Info, "net: client %d connected to client %d (%s, port %u), input delay %u beats", g->local, g->peer,
         host ? "host" : "joined", o.netPort, g->delay);
    return g;
  }

  // Before a beat: this turn's orders out, then wait for every client's batch of the beat and carry out the merge.
  bool Turn(Sim& sim, std::map<std::string, int>* unhandled) {
    const uint32_t beat = lockstep->nextBeat();
    if (ejectFrom && beat == ejectFrom) {
      ex->Eject(ejectWho, beat);
      Logf(LogLevel::Info, "net: client %d is left out from beat %u on (MOHO64_NET_EJECT)", ejectWho, beat);
    }
    if (!turn.empty()) sentFor[beat + delay] = turn;
    if (!turn.empty() || !rogue.empty()) lockstep->Queue(turn + rogue);
    turn.clear();
    rogue.clear();
    std::string merged;
    const auto t0 = std::chrono::steady_clock::now();
    while (!lockstep->Ready(&merged)) {
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120) || !ex->LostClients().empty()) {
        Logf(LogLevel::Error, "net: beat %u: the other copy's batch never came%s", beat,
             ex->LostClients().empty() ? "" : " (its connection is lost)");
        return false;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    waitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (auto it = sentFor.find(beat); it != sentFor.end()) {
      if (merged.find(it->second) != std::string::npos) {
        ++delayOk;
      } else {
        ++delayWrong;
        Logf(LogLevel::Error, "net: beat %u: the orders queued %u beats before are not in its merge", beat, delay);
      }
      sentFor.erase(it);
    }
    if (streamOut) {
      const std::string adv = CommandWriter::AdvanceMessage(1);
      std::fwrite(merged.data(), 1, merged.size(), streamOut);
      std::fwrite(adv.data(), 1, adv.size(), streamOut);
    }
    if (!merged.empty() && !ApplyCommands(sim, merged, unhandled))
      Logf(LogLevel::Error, "net: beat %u: a merged message was malformed", beat);
    return true;
  }

  // After the beat (paused ones too): the checksum every 50 beats, and the peers' compared.
  void After(const Sim& sim, uint32_t beat) {
    std::string digest;
    if (beat % kLockstepChecksumPeriod == 0) {
      digest = LockstepDigest(sim, beat);
      if (beat == corruptAt) {
        digest[0] = static_cast<char>(digest[0] ^ 0x5a);
        Logf(LogLevel::Info, "net: beat %u: our digest is changed on purpose (MOHO64_NET_CORRUPT_DIGEST)", beat);
      }
    }
    lockstep->AfterBeat(digest);
    for (const LockstepDesync& d : lockstep->desyncs()) {
      ++desyncs;
      Logf(LogLevel::Warning, "net: desync: beat %u, client %d's checksum differs from ours", d.beat, d.client);
    }
    lockstep->desyncs().clear();
  }

  // The end: what is still queued for the other copy goes out (Send only queues, the connector's Poll writes), and a
  // summary.
  void End(uint32_t beats) {
    for (int i = 0; i < 300; ++i) {
      ex->ReceivedBeats(peer);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Logf(LogLevel::Info, "net: client %d: %u beats, %u messages rejected, input delay checks %u ok %u wrong, %u desync "
         "report(s), %.2f s waiting for the other copy", local, beats, lockstep->rejected(), delayOk, delayWrong,
         desyncs, waitSeconds);
  }
};

// The game's file system as main mounts it (--together mounts a second one for its second copy).
bool MountGame(const Options& o, Vfs& vfs, std::vector<std::string>* hookDirs) {
  // The game's preferences file (sim/session.h kGamePrefsPath): this run's own, held in memory and mounted before
  // the init file mounts the original's preferences folder at /preferences, so the rules never read the
  // original's Game.prefs (in the lab: the PreGameData of the last game played in FAF); the folder's other files
  // stay visible. No map folder until a session sets its own (RunTimes).
  vfs.MountMemoryFile(kGamePrefsPath, PreGameDataPrefs(""));
  if (!MountFromInitFile(o.init, o.folders, vfs, *hookDirs)) return false;
  // --maps <folder> (may be given more than once): more map folders beside the game's own at /maps, as the
  // interface host has it (a game played there on such a map can then be played back here)
  for (const std::string& folder : o.moreMaps)
    if (!vfs.Mount(folder, "/maps")) Logf(LogLevel::Warning, "moho64: --maps %s: no such folder", folder.c_str());
  return true;
}

// Whether two files hold the same bytes; *at: the first byte they differ at (or the shorter one's length), *line: its
// line (0-based; a fingerprint's line N is beat N).
bool SameBytes(const std::string& a, const std::string& b, long long* at, long long* line) {
  std::FILE* fa = std::fopen(a.c_str(), "rb");
  std::FILE* fb = std::fopen(b.c_str(), "rb");
  bool same = fa && fb;
  long long n = 0;
  *line = 0;
  std::vector<char> ba(1 << 20), bb(1 << 20);
  while (same) {
    const size_t ra = std::fread(ba.data(), 1, ba.size(), fa), rb = std::fread(bb.data(), 1, bb.size(), fb);
    const size_t m = std::min(ra, rb);
    size_t i = 0;
    while (i < m && ba[i] == bb[i]) {
      if (ba[i] == '\n') ++*line;
      ++i;
    }
    n += static_cast<long long>(i);
    if (i < m || ra != rb) same = false;
    else if (ra == 0) break;
  }
  if (fa) std::fclose(fa);
  if (fb) std::fclose(fb);
  *at = n;
  return same;
}

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
    Logf(LogLevel::Info, "view: army %d start %.1f %.1f no-rush %d beats radius %.1f offset %.1f %.1f", a.index, a.start[0],
         a.start[1], a.noRushTicks, a.noRushRadius, a.noRushOffset[0], a.noRushOffset[1]);
  }
  std::vector<UnitView> units;
  UnitViews(sim, &units);
  Logf(LogLevel::Info, "view: %zu units", units.size());
  for (size_t i = 0; i < units.size() && i < 8; ++i) {
    const UnitView& u = units[i];
    Logf(LogLevel::Info, "view: unit %#x %s army %d at %.1f %.1f %.1f health %.0f/%.0f built %.2f queue %d%s%s", u.id,
         u.blueprint.c_str(), u.army, u.position[0], u.position[1], u.position[2], u.health, u.maxHealth,
         u.fractionComplete, u.queueLength, u.selectable ? "" : " unselectable", u.dead ? " dead" : "");
    Logf(LogLevel::Info, "view: unit %#x intel %#x %#x %#x %#x %#x %#x %#x %#x", u.id, u.intel[0], u.intel[1], u.intel[2],
         u.intel[3], u.intel[4], u.intel[5], u.intel[6], u.intel[7]);
  }
  {  // the units' weapons (ui request 37): how many, the shared category sets, and two armed units' weapons
    auto t0 = std::chrono::steady_clock::now();
    std::vector<UnitView> again;
    UnitViews(sim, &again);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    size_t weapons = 0, armed = 0;
    std::map<int, std::string> idOf;  // category index -> a blueprint id, of the units here
    for (const UnitView& u : again) {
      weapons += u.weapons.size();
      armed += !u.weapons.empty();
      idOf.emplace(u.categoryIndex, u.blueprint);
    }
    std::vector<std::vector<uint64_t>> sets;
    CategorySets(sim, 0, &sets);
    Logf(LogLevel::Info, "view: %zu of %zu units carry %zu weapons; %zu category sets; UnitViews took %.3f ms", armed,
         again.size(), weapons, sets.size(), ms);
    for (size_t s = 0; s < sets.size() && s < 10; ++s) {
      size_t members = 0;
      std::string here;
      for (size_t w = 0; w < sets[s].size(); ++w)
        for (int b = 0; b < 64; ++b)
          if ((sets[s][w] >> b) & 1) {
            ++members;
            const int idx = static_cast<int>(w * 64 + b);
            auto it = idOf.find(idx);
            if (it != idOf.end() && here.size() < 60 && CategorySetHas(sim, static_cast<int>(s), idx)) here += " " + it->second;
          }
      Logf(LogLevel::Info, "view: category set %zu: %zu blueprints; of the units here:%s", s, members, here.c_str());
    }
    int shownArmed = 0;
    for (const UnitView& u : again) {
      if (u.weapons.empty() || shownArmed >= 2) continue;
      ++shownArmed;
      std::string list;
      for (const WeaponView& w : u.weapons) {
        char b[120];
        std::snprintf(b, sizeof b, " [layers %#x radius %g..%g disallow %d onlyAllow %d]", w.layerCaps, w.minRadius,
                      w.maxRadius, w.disallow, w.onlyAllow);
        list += b;
      }
      Logf(LogLevel::Info, "view: unit %#x %s category %d layer %d%s: %zu weapons%s", u.id, u.blueprint.c_str(),
           u.categoryIndex, u.layer, u.autoSurfaceMode ? " autosurface" : "", u.weapons.size(), list.c_str());
    }
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
  {  // the scripts' statistics a unit's panel reads (UnitStateView::stats): the first units with veterancy
    int vetShown = 0;
    size_t withStats = 0;
    for (const UnitView& u : units) {
      UnitStateView s;
      if (!UnitStateOf(sim, u.id, &s) || s.stats.empty()) continue;
      ++withStats;
      bool vet = false;
      std::string list;
      for (const auto& [name, value] : s.stats) {
        char b[96];
        std::snprintf(b, sizeof b, " %s=%g", name.c_str(), value);
        list += b;
        vet |= (name == "VetLevel" || name == "VetExperience") && value > 0;
      }
      if (vet && vetShown++ < 5) Logf(LogLevel::Info, "view: stats %#x %s:%s", u.id, u.blueprint.c_str(), list.c_str());
    }
    Logf(LogLevel::Info, "view: %zu units keep script statistics, %d with veterancy", withStats, vetShown);
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
// outSuffix: --together's name for this copy's record and fingerprint; beatsDone: the beats it has played, for the
// other copy to wait on (RunTimes).
int RunSession(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs,
               const std::vector<std::string>& uids, const SessionDescription& session, const std::string* stream_,
               const std::string& outSuffix = std::string(), std::atomic<int>* beatsDone = nullptr) {
  const ReplayHeader* header = &session.header;
  static const std::string none;
  const std::string* data = stream_ ? stream_ : &none;
  Sim sim(&vfs, hookDirs, uids);
  sim.sessionMods = header->mods;
  sim.recordSuffix = outSuffix;
  auto t0 = std::chrono::steady_clock::now();
  if (!sim.LoadRules()) return 1;
  auto t1 = std::chrono::steady_clock::now();
  sim.collectPrints = o.view;  // the scripts' print lines, as a console host takes them (before Start: its lines too)
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
  // MOHO64_HITS=1 (render REQUESTS 36): keep the hits, impacts and health changes (Sim::collectHits, collectImpacts,
  // collectHealth) and take them after every beat as a host does; the log gets the counts, the most in one beat, a
  // few samples and what taking them cost. =2 also logs every event. (--view takes them too.) The game must play
  // the same with them as without: compare MOHO64_FINGERPRINT / MOHO64_RECORD of a run with and one without.
  const int hitsLog = getenv("MOHO64_HITS") ? std::atoi(getenv("MOHO64_HITS")) : 0;
  sim.collectHits = sim.collectImpacts = sim.collectHealth = o.view || hitsLog > 0;
  struct HitsSeen {
    size_t n[3] = {0, 0, 0}, most[3] = {0, 0, 0};
    uint32_t mostTick[3] = {0, 0, 0};
    size_t shield = 0, credited = 0, beam = 0, miss = 0, crash = 0, beamImpacts = 0;
    std::map<std::string, size_t> impactTypes, damageTypes;
    std::vector<std::string> samples;
    double seconds = 0;
  } hitsSeen;
  std::vector<HitEvent> hitList;
  std::vector<ImpactEvent> impactList;
  std::vector<HealthEvent> healthList;
  auto takeHits = [&] {
    if (!sim.collectHits) return;
    const auto h0 = std::chrono::steady_clock::now();
    hitList = sim.TakeHits();
    impactList = sim.TakeImpacts();
    healthList = sim.TakeHealth();
    hitsSeen.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - h0).count();
    const size_t sizes[3] = {hitList.size(), impactList.size(), healthList.size()};
    for (int k = 0; k < 3; ++k) {
      hitsSeen.n[k] += sizes[k];
      if (sizes[k] > hitsSeen.most[k]) {
        hitsSeen.most[k] = sizes[k];
        hitsSeen.mostTick[k] = sim.tick();
      }
    }
    char b[480];
    for (const HitEvent& h : hitList) {
      hitsSeen.shield += h.shield;
      hitsSeen.credited += !h.weapon.empty();
      hitsSeen.beam += h.beam;
      ++hitsSeen.damageTypes[h.damageType];
      std::snprintf(b, sizeof b, "tick %u hit %#x (kind %d%s %#x) by %#x army %d weapon '%s' shell '%s'%s: %.2f %s at %.2f %.2f %.2f r %.2f dir %.3f %.3f %.3f",
                    sim.tick(), h.target, h.targetKind, h.shield ? ", shield" : "", h.shieldId, h.instigator, h.army,
                    h.weapon.c_str(), h.projectile.c_str(), h.beam ? " beam" : "", h.amount, h.damageType.c_str(),
                    h.position[0], h.position[1], h.position[2], h.radius, h.direction[0], h.direction[1], h.direction[2]);
      if (hitsLog >= 2) Logf(LogLevel::Info, "hits: %s", b);
      if (hitsSeen.samples.size() < 6 || (h.shield && hitsSeen.shield == 1) || (h.beam && hitsSeen.beam == 1))
        hitsSeen.samples.push_back(b);
    }
    for (const ImpactEvent& ev : impactList) {
      ++hitsSeen.impactTypes[ev.type];
      hitsSeen.miss += ev.entity == 0xffffffffu;
      hitsSeen.crash += ev.kind == ImpactEvent::Crash;
      hitsSeen.beamImpacts += ev.kind == ImpactEvent::Beam;
      std::snprintf(b, sizeof b, "tick %u impact %s of %s %#x army %d '%s' weapon '%s' on %#x at %.2f %.2f %.2f dir %.3f %.3f %.3f speed %.2f",
                    sim.tick(), ev.type.c_str(), ev.kind == ImpactEvent::Projectile ? "shell" : ev.kind == ImpactEvent::Beam ? "beam" : "crash",
                    ev.source, ev.army, ev.projectile.c_str(), ev.weapon.c_str(), ev.entity, ev.position[0], ev.position[1],
                    ev.position[2], ev.direction[0], ev.direction[1], ev.direction[2], ev.speed);
      if (hitsLog >= 2) Logf(LogLevel::Info, "hits: %s", b);
      if (hitsSeen.samples.size() < 12 || (ev.kind == ImpactEvent::Crash && hitsSeen.crash == 1))
        hitsSeen.samples.push_back(b);
    }
    for (const HealthEvent& ev : healthList) {
      std::snprintf(b, sizeof b, "tick %u health %#x (kind %d) %.4f -> %.4f", sim.tick(), ev.entity, ev.kind, ev.before,
                    ev.fraction);
      if (hitsLog >= 2) Logf(LogLevel::Info, "hits: %s", b);
      if (hitsSeen.samples.size() < 16) hitsSeen.samples.push_back(b);
    }
  };
  size_t propCount[3] = {0, 0, 0}, propMapOnes[3] = {0, 0, 0}, entMax = 0, entShieldMax = 0, unitMeshMax = 0;
  std::string propSample[3], entSample[2], unitMeshSample, carrySample, creatorSample;
  size_t carryMax = 0;
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
  // --orders: lines "<tick> issue <command type number> <unit id>[,<unit id>...] [<x> <y> <z> | @<entity id>] [queue]",
  // "<tick> pause", "<tick> resume", "<tick> endgame", "<tick> callback <name>", "<tick> lua <one line of Lua run in
  // the sim state: a test building its case, not an order>", "<tick> focus <army 1-based | -1>" (the interface's
  // focus army request: Sim::RequestFocusArmy), "<tick> debug <army 1-based | -1> <sim console command>" (a
  // DebugCommand: Ctrl-Alt-B is "BlingBling"), "<tick> warp <unit id> <x> <y> <z>" (a WarpEntity with the unit's
  // own orientation: Alt-T); <tick> counts beats, paused ones included, and
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
  CommandWriter writer(o.netMode.empty() ? 0 : o.netClient);
  std::map<std::string, int> ordersUnhandled;
  std::unique_ptr<NetGame> net;  // (--net: made below, before the first beat)
  std::vector<std::pair<std::vector<uint32_t>, std::string>> tracked;  // (issuegroup under MOHO64_PROFILE)
  auto giveOrders = [&] {
    CommandWriter& own = writer;
    auto range = orders.equal_range(static_cast<uint32_t>(done + paused));
    for (auto it = range.first; it != range.second; ++it) {
      char what[32] = "", ids[256] = "", tail[32] = "";
      int type = 0;
      float x = 0, y = 0, z = 0;
      const char* text = it->second.c_str();
      if (std::sscanf(text, "%31s", what) != 1) continue;
      // "<tick> claim <source> <an issue or issuegroup line>": that order written as another command source (--net)
      std::unique_ptr<CommandWriter> claimed;
      std::string claimText;
      if (!std::strcmp(what, "claim")) {
        int src = 0, at = 0;
        if (std::sscanf(text, "%*s %d %n", &src, &at) < 1 || at == 0 || !net) continue;
        claimed = std::make_unique<CommandWriter>(src);
        claimText = text + at;
        text = claimText.c_str();
        if (std::sscanf(text, "%31s", what) != 1) continue;
      }
      CommandWriter& writer = claimed ? *claimed : own;
      if (!std::strcmp(what, "pause")) writer.RequestPause();
      else if (!std::strcmp(what, "resume")) writer.Resume();
      else if (!std::strcmp(what, "endgame")) writer.EndGame();  // one more beat runs, then the game stands
      else if (!std::strcmp(what, "callback") && std::sscanf(text, "%*s %255s", ids) == 1) writer.LuaSimCallback(ids, {}, {});
      else if (!std::strcmp(what, "lua")) sim.RunString(text + 3, "@orders");  // a test's own Lua, straight into the sim state
      else if (!std::strcmp(what, "info")) {  // "<tick> info <unit id> <key> <value>": an info pair, as the interface sends it
        char key[64] = "", value[64] = "";
        unsigned long id = 0;
        if (std::sscanf(text, "%*s %lu %63s %63s", &id, key, value) == 3) writer.ProcessInfoPair(static_cast<uint32_t>(id), key, value);
      }
      else if (!std::strcmp(what, "debug")) {  // "<tick> debug <army, 1-based | -1> <text>": DoSimCommand <text>
        int army = 0, at = 0;
        if (std::sscanf(text, "%*s %d %n", &army, &at) >= 1 && at > 0)
          writer.DebugCommand(text + at, 0, 0, 0, army == -1 ? -1 : army - 1, {});
      }
      else if (!std::strcmp(what, "warp")) {  // "<tick> warp <unit id> <x> <y> <z>": Alt-T's WarpEntity for one unit
        unsigned long id = 0;
        if (std::sscanf(text, "%*s %lu %f %f %f", &id, &x, &y, &z) == 4) {
          const Entity* e = sim.FindEntityById(static_cast<uint32_t>(id));
          const Quat q = e ? e->orientation : Quat{};
          writer.WarpEntity(static_cast<uint32_t>(id), q.w, q.x, q.y, q.z, x, y, z);
        }
      }
      else if (!std::strcmp(what, "focus")) {  // "<tick> focus <army, 1-based | -1>": the interface's SetFocusArmy
        int army = 0;
        if (std::sscanf(text, "%*s %d", &army) == 1) sim.RequestFocusArmy(army == -1 ? -1 : army - 1);
      }
      else if (!std::strcmp(what, "issuegroup")) {
        // "<tick> issuegroup <type> <army, 1-based> <blueprint | *> <x> <y> <z> [queue]": one order to every unit of the
        // army (of that blueprint) that stands finished, in id order, as a player's selection of them would give it
        // (docs/benchmarks.md B8; under MOHO64_PROFILE its latency is logged, Sim::ProfileTrackOrder)
        char bp[64] = "";
        int army = 0;
        if (std::sscanf(text, "%*s %d %d %63s %f %f %f %31s", &type, &army, bp, &x, &y, &z, tail) < 6) continue;
        std::vector<uint32_t> units;
        for (const Unit* u : sim.units())
          if (u->army && u->army->index == army && !u->dead && !u->destroyQueued && !u->beingBuilt && u->blueprint &&
              (!std::strcmp(bp, "*") || !strcasecmp(bp, u->blueprint->id.c_str())))
            units.push_back(u->id);
        std::sort(units.begin(), units.end());
        StreamIssueData d;
        d.type = type;
        d.target.type = 2;
        d.target.pos = {x, y, z};
        uint32_t id = writer.IssueCommand(units, d, std::strcmp(tail, "queue") != 0);
        Logf(LogLevel::Info, "orders: tick %u: command %#x type %d for %zu unit(s) of army %d", sim.tick(), id, type,
             units.size(), army);
        char w[96];
        std::snprintf(w, sizeof w, "type %d, %zu units of army %d (%s)", type, units.size(), army, bp);
        tracked.emplace_back(std::move(units), w);
      }
      else if (!std::strcmp(what, "issue")) {
        // (an entity target: "<tick> issue <type> <unit ids> @<entity id> [queue]")
        char at[32] = "";
        const bool onEntity = std::sscanf(text, "%*s %*d %*255s %31s", at) == 1 && at[0] == '@';
        int n = onEntity ? std::sscanf(text, "%*s %d %255s %*s %31s", &type, ids, tail) + 1
                         : std::sscanf(text, "%*s %d %255s %f %f %f %31s", &type, ids, &x, &y, &z, tail);
        if (n < 2) continue;
        std::vector<uint32_t> units;
        for (char* tok = std::strtok(ids, ","); tok; tok = std::strtok(nullptr, ","))
          units.push_back(static_cast<uint32_t>(std::strtoul(tok, nullptr, 0)));
        StreamIssueData d;
        d.type = type;
        if (onEntity) {
          d.target.type = 1;
          d.target.entity = static_cast<uint32_t>(std::strtoul(at + 1, nullptr, 0));
        } else if (n >= 5) {
          d.target.type = 2;
          d.target.pos = {x, y, z};
        }
        uint32_t id = writer.IssueCommand(units, d, std::strcmp(tail, "queue") != 0);
        Logf(LogLevel::Info, "orders: tick %u: command %#x type %d for %zu unit(s)", sim.tick(), id, type, units.size());
      }
      if (claimed) net->rogue += claimed->Take();
    }
    if (net) net->turn += writer.Take();  // (--net: to every copy through the lockstep, NetGame::Turn)
    else if (!writer.empty() && !ApplyCommands(sim, writer.Take(), &ordersUnhandled)) Logf(LogLevel::Error, "orders: a message was malformed");
    for (auto& [ids, w] : tracked) sim.ProfileTrackOrder(ids, w);
    tracked.clear();
  };
  int hostFocus = sim.focusArmy;  // the host's own copy, as the interface keeps one
  auto takeSync = [&] {
    if (sim.focusArmy != hostFocus) {  // the beat's Sync applied a requested focus army
      Logf(LogLevel::Info, "moho64: tick %u: focus army %d -> %d", sim.tick(), hostFocus == -1 ? -1 : hostFocus + 1,
           sim.focusArmy == -1 ? -1 : sim.focusArmy + 1);
      hostFocus = sim.focusArmy;
    }
    for (auto& b : sim.TakeSync()) {
      ++syncBeats;
      syncBytes += b.size();
      // the final score (FAF's score.lua, sent by its game-over listener): which sync table carried it
      if (o.view && b.find("ScoreAccum") != std::string::npos)
        Logf(LogLevel::Info, "view: sync table %zu (tick %u, %zu bytes) carries ScoreAccum%s", syncBeats, sim.tick(), b.size(),
             b.find("StatsToSend") != std::string::npos ? " and StatsToSend" : "");
      // the sources that met a cheat test (Sim::CheatsEnabled): FAF's UserSync.lua prints "<name> is cheating!"
      if (o.view && b.find("Cheaters") != std::string::npos)
        Logf(LogLevel::Info, "view: sync table %zu (tick %u) carries Cheaters", syncBeats, sim.tick());
      lastSync = std::move(b);
    }
    // --view: the print lines since the last take, each as the host's console would get it ([] around it, so that
    // spaces at its ends show)
    if (o.view)
      for (const std::string& line : sim.TakePrints())
        Logf(LogLevel::Info, "view: print tick %u [%s]", sim.tick(), line.c_str());
  };
  // The setup's own Sync (the original's beat 0, made by Start), taken before the first beat as a host does.
  if (ok) {
    takeSync();
    if (o.view) Logf(LogLevel::Info, "view: the setup's sync: %zu table(s), %zu bytes", syncBeats, syncBytes);
  }
  // MOHO64_FINGERPRINT=<file>: a line per beat for comparing two copies of the game (sim/fingerprint.h); beat 0 is
  // the state Start left. With a peer, the first beat the two differ at ends the run.
  std::unique_ptr<Fingerprint> fingerprint = ok ? Fingerprint::FromEnvironment(outSuffix) : nullptr;
  bool diverged = fingerprint && !fingerprint->Beat(sim, 0);
  // MOHO64_VIEW_READS=1 (a check): after every beat the reads of the simulation an interface makes between beats,
  // for every unit at once: UnitStateOf for each, ReclaimableAt at the first units, BuildSiteOf of one more unit
  // blueprint a beat, and once, before the first beat, BlueprintDisplays (the panels, the cursor and the building
  // ghost at their busiest; sim/sim_view.h). The game must play the same with them as without: compare
  // MOHO64_FINGERPRINT, its lua part too, of a run with and one without. The log says in how many beats the reads
  // ran a collection or changed the Lua heap. MOHO64_GC_DUE=<n> makes a collection due after every n-th beat (the
  // collector's threshold set to the heap, lua_gpg_gcdue), in a run with the reads and one without alike, so that
  // the reads meet a due collection whenever wanted.
  struct ViewReads {
    bool on = false;
    int due = 0;
    size_t beats = 0, collected = 0, changed = 0;
    long long bytes = 0;
    std::vector<std::string> unitBps;
    size_t nextBp = 0;
  } vr;
  vr.on = getenv("MOHO64_VIEW_READS") && std::atoi(getenv("MOHO64_VIEW_READS")) > 0;
  vr.due = getenv("MOHO64_GC_DUE") ? std::atoi(getenv("MOHO64_GC_DUE")) : 0;
  auto viewReads = [&](bool first) {
    lua_State* L = sim.L();
    if (!L) return;
    if (vr.due > 0 && !first && sim.tick() % static_cast<uint32_t>(vr.due) == 0) lua_gpg_gcdue(L);
    if (!vr.on) return;
    size_t heap0 = 0, thr0 = 0, heap1 = 0, thr1 = 0;
    lua_gpg_gcbytes(L, &heap0, &thr0);
    if (first) {
      std::vector<BlueprintDisplay> bd;
      BlueprintDisplays(sim, &bd);
      for (const BlueprintDisplay& d : bd)
        if (d.kind == 0) vr.unitBps.push_back(d.id);
    }
    std::vector<UnitView> units;
    UnitViews(sim, &units);
    for (const UnitView& u : units) {
      UnitStateView st;
      UnitStateOf(sim, u.id, &st);
    }
    for (size_t i = 0; i < units.size() && i < 8; ++i) {
      ReclaimableView rv;
      ReclaimableAt(sim, units[i].position, 5.0f, &rv);
    }
    if (!units.empty() && !vr.unitBps.empty()) {
      float site[3];
      bool can = false;
      BuildSiteOf(sim, vr.unitBps[vr.nextBp++ % vr.unitBps.size()], units[0].position, site, &can);
    }
    lua_gpg_gcbytes(L, &heap1, &thr1);
    ++vr.beats;
    if (thr1 != thr0) {
      if (vr.collected++ < 10)
        Logf(LogLevel::Info, "view reads: tick %u: a collection ran in the reads (heap %zu -> %zu, threshold %zu -> %zu)",
             sim.tick(), heap0, heap1, thr0, thr1);
    } else if (heap1 != heap0) {
      if (vr.changed++ < 10)
        Logf(LogLevel::Info, "view reads: tick %u: the reads changed the heap by %lld bytes", sim.tick(),
             static_cast<long long>(heap1) - static_cast<long long>(heap0));
      vr.bytes += static_cast<long long>(heap1) - static_cast<long long>(heap0);
    }
  };
  if (ok) viewReads(true);
  if (ok && !o.netMode.empty() && !(net = NetGame::Connect(o))) ok = false;
  while (ok && !diverged && done < o.ticks) {
    if (playStream && !stream.AtEnd()) {
      if (!stream.ApplyUntilAdvance(sim)) {
        Logf(LogLevel::Info, "moho64: the replay's command stream ended after %u beats (tick %u)", stream.beats(),
             sim.tick());
        if (!getenv("MOHO64_PAST_END")) break;
      }
    }
    giveOrders();
    if (net && !net->Turn(sim, &ordersUnhandled)) break;
    if (sim.Paused()) {
      sim.PausedBeat();
      takeSync();
      takeHits();
      ++paused;
      if (fingerprint && !fingerprint->Beat(sim, static_cast<uint32_t>(done + paused))) diverged = true;
      if (net) net->After(sim, static_cast<uint32_t>(done + paused));
      if (diverged || (sim.frozen && (stream.AtEnd() || !playStream))) break;
      continue;
    }
    sim.Tick();
    takeSync();
    takeHits();
    ++done;
    if (beatsDone) beatsDone->store(done, std::memory_order_release);
    if (fingerprint && !fingerprint->Beat(sim, static_cast<uint32_t>(done + paused))) {
      diverged = true;
      break;
    }
    if (net) net->After(sim, static_cast<uint32_t>(done + paused));
    viewReads(false);
    // MOHO64_DEBUG_RECONGRID=<n> (render REQUESTS 38): at beat 1 and every 600th, for every army and each water
    // kind, ReconGrid's n x n grid against n x n ReconAt calls at the same points (the renderer's sight grid's
    // floats): the cells that differ, and the time of both
    static const int rgCells = getenv("MOHO64_DEBUG_RECONGRID") ? std::atoi(getenv("MOHO64_DEBUG_RECONGRID")) : 0;
    if (rgCells > 0 && sim.map() && (sim.tick() == 1 || sim.tick() % 600 == 0)) {
      const float mw = static_cast<float>(sim.map()->width()), mh = static_cast<float>(sim.map()->height());
      std::vector<uint8_t> grid;
      for (int army = 1; army <= static_cast<int>(sim.armies().size()); ++army)
        for (int water = 0; water <= 2; ++water) {
          const auto t0 = std::chrono::steady_clock::now();
          ReconGrid(sim, army, rgCells, rgCells, &grid, water);
          const auto t1 = std::chrono::steady_clock::now();
          size_t differ = 0, set = 0;
          uint32_t orBits = 0;
          for (int z = 0; z < rgCells; ++z)
            for (int x = 0; x < rgCells; ++x) {
              const float at[3] = {(static_cast<float>(x) + 0.5f) * mw / static_cast<float>(rgCells), 0,
                                   (static_cast<float>(z) + 0.5f) * mh / static_cast<float>(rgCells)};
              const uint32_t r = ReconAt(sim, army, at, water);
              const uint8_t g = grid[static_cast<size_t>(z) * static_cast<size_t>(rgCells) + static_cast<size_t>(x)];
              if (r != g) {
                if (differ++ < 3)
                  Logf(LogLevel::Info, "recongrid: tick %u army %d water %d cell %d,%d: ReconAt %u, ReconGrid %u", sim.tick(),
                       army, water, x, z, r, g);
              }
              set += r != 0;
              orBits |= r;
            }
          const auto t2 = std::chrono::steady_clock::now();
          Logf(LogLevel::Info, "recongrid: tick %u army %d water %d: %d x %d cells, %zu differ (%zu set, bits %#x); ReconGrid %.3f ms, "
               "the calls %.3f ms", sim.tick(), army, water, rgCells, rgCells, differ, set, orBits,
               std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
    }
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
      static const bool soundLog = getenv("MOHO64_SOUND_LOG") != nullptr;
      for (const SoundEvent& ev : snd) {
        // MOHO64_SOUND_LOG=1 (with --view): every world sound, in the interface host's own words (its
        // MOHOUI64_SOUND_LOG), so that a played-back game can be laid beside the live game's log
        if (soundLog && !ev.cue.empty())
          Logf(LogLevel::Info, "sound: tick %u %s entity %u army %d at %.0f %.0f %.0f: %s/%s", sim.tick(),
               ev.kind == SoundEvent::Play ? "play" : "loop", ev.entity, ev.army, ev.position[0], ev.position[1],
               ev.position[2], ev.bank.c_str(), ev.cue.c_str());
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
        // ui 36: the beat with the most units on units (UnitView::attachedUnits, attach order); each child's
        // UnitView::parent must name its carrier
        size_t carried = 0;
        for (const UnitView& v : uv) carried += v.attachedUnits.size();
        if (carried > carryMax) {
          carryMax = carried;
          for (const UnitView& v : uv)
            if (v.attachedUnits.size() > 1 || (carrySample.empty() && !v.attachedUnits.empty())) {
              char cb[200];
              std::snprintf(cb, sizeof cb, "tick %u unit %#x %s carries %zu (of %zu units on units):", sim.tick(), v.id,
                            v.blueprint.c_str(), v.attachedUnits.size(), carried);
              carrySample = cb;
              for (uint32_t c : v.attachedUnits) {
                uint32_t p = 0;
                for (const UnitView& k : uv)
                  if (k.id == c) p = k.parent;
                std::snprintf(cb, sizeof cb, " %#x%s", c, p == v.id ? "" : " (parent differs)");
                carrySample += cb;
              }
              if (v.attachedUnits.size() > 1) break;
            }
        }
        for (const UnitView& v : uv)  // a UEF pod and its station or commander (GetAssistingUnitsList goes by it)
          if (creatorSample.empty() && v.creator != 0xffffffffu &&
              (v.blueprint == "xea3204" || v.blueprint == "uea0001" || v.blueprint == "uea0003")) {
            char cb[160];
            std::snprintf(cb, sizeof cb, "tick %u pod %#x %s: creator %#x, parent %#x", sim.tick(), v.id,
                          v.blueprint.c_str(), v.creator, v.parent);
            creatorSample = cb;
          }
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
    if (!carrySample.empty()) Logf(LogLevel::Info, "view: %s", carrySample.c_str());
    if (!creatorSample.empty()) Logf(LogLevel::Info, "view: %s", creatorSample.c_str());
  }
  if (ok && sim.collectHits) {
    Logf(LogLevel::Info, "hits: %zu hits (%zu on shields, %zu credited to a weapon, %zu by beams), %zu impacts (%zu met "
         "nothing, %zu by beams, %zu crashes), %zu health changes; most in one beat: %zu hits (tick %u), %zu impacts "
         "(tick %u), %zu health (tick %u); taking them took %.4f ms a beat",
         hitsSeen.n[0], hitsSeen.shield, hitsSeen.credited, hitsSeen.beam, hitsSeen.n[1], hitsSeen.miss,
         hitsSeen.beamImpacts, hitsSeen.crash, hitsSeen.n[2], hitsSeen.most[0], hitsSeen.mostTick[0], hitsSeen.most[1],
         hitsSeen.mostTick[1], hitsSeen.most[2], hitsSeen.mostTick[2],
         done + paused ? hitsSeen.seconds * 1000.0 / (done + paused) : 0.0);
    for (const auto& [type, n] : hitsSeen.impactTypes) Logf(LogLevel::Info, "hits:   impact '%s' x%zu", type.c_str(), n);
    for (const auto& [type, n] : hitsSeen.damageTypes) Logf(LogLevel::Info, "hits:   damage '%s' x%zu", type.c_str(), n);
    for (const std::string& line : hitsSeen.samples) Logf(LogLevel::Info, "hits: %s", line.c_str());
  }
  if (fingerprint && !diverged) fingerprint->End(static_cast<uint32_t>(done + paused));
  if (net) net->End(static_cast<uint32_t>(done + paused));
  if (vr.on)
    Logf(LogLevel::Info, "view reads: %zu beats read; a collection ran in the reads of %zu, the heap changed in %zu (%lld bytes)",
         vr.beats, vr.collected, vr.changed, vr.bytes);
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
  return diverged ? 3 : ok ? 0 : 1;  // (3: the fingerprint's peer differed)
}

// RunSession, and with --twice a second time after the first Sim is gone, as the interface's Relaunch does: the
// second game must play exactly as the first (no state of the first sim may reach it). It records beside the first
// record (MOHO64_RECORD + ".2"); both records must be the same bytes. A fingerprint likewise goes to
// MOHO64_FINGERPRINT + ".2".
int RunTimes(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs, const std::vector<std::string>& uids,
             const SessionDescription& launched, const std::string* stream) {
  SessionDescription session = launched;
  // The session's PreGameData into this run's own Game.prefs (mounted in main before the init file), before the
  // rules load: the map folder whose own blueprints they load, from the session (a replay: its own scenario), as
  // the lobby would have written it (sim/session.h SessionMapFolder)
  const std::string mapFolder = SessionMapFolder(session.header);
  vfs.SetMemoryFile(kGamePrefsPath, PreGameDataPrefs(mapFolder));
  Logf(LogLevel::Info, "moho64: PreGameData.CurrentMapDir '%s'", mapFolder.c_str());
  if (o.hasFocus) {
    session.focusArmy = o.focus >= 1 && o.focus <= static_cast<int>(session.header.armies.size()) ? o.focus - 1 : -1;
    Logf(LogLevel::Info, "moho64: --focus: the session looks through army %d", session.focusArmy == -1 ? -1 : session.focusArmy + 1);
  }
  int rc = 0;
  if (o.together) {
    // --together: two copies of the session at once, each on its own thread with its own file system (mounted
    // again as main does), its record and fingerprint beside the first's (MOHO64_RECORD / MOHO64_FINGERPRINT +
    // ".2", as --twice). --twice plays its second sim after the first is gone, so a variable two sims share passes
    // it; here both are alive and beating at once, which multicore step 1 is about (docs/multicore.md). The second
    // copy starts when the first has played --together-lag beats (default 100), so that the two set and read a
    // variable they share at unrelated moments of their games (two copies in step might hide it; a precaution). No
    // fingerprint peers, then (they would wait for each other): the two copies' files are compared at the end, exit
    // code 4 when they differ, with the first beat they differ at (MOHO64_FINGERPRINT_DETAIL=<beat> then lists it).
    // A shared variable shows only where it changes the game, and a race's beat varies: a steering-stage flag
    // planted shared in a scratch build passed the modded Skirmish to 1500 and was caught on MP2 to 6000 in three
    // runs out of three (beats 1920, 2167, 2757). Each copy is also worth comparing with the game played alone.
    Vfs vfs2;
    std::vector<std::string> hookDirs2;
    if (!MountGame(o, vfs2, &hookDirs2)) return 1;
    vfs2.SetMemoryFile(kGamePrefsPath, PreGameDataPrefs(mapFolder));
    Logf(LogLevel::Info, "moho64: --together: two copies at once");
    // (both copies on threads of their own, from the one RunSession call below: a record keeps the call chain of
    // each draw, Sim::RecorderDraw, so the two records are the same bytes only from the same call site)
    int rcs[2] = {0, 0};
    std::atomic<int> firstBeats{0};
    std::atomic<bool> firstEnded{false};
    // (--together-threads: the second copy's sim makes its worker pool with that many threads, the first's with
    // MOHO64_THREADS: one copy at n = 1 and the other at the machine's cores, docs/multicore.md 1.3)
    auto copy = [&](int k) {
      ThreadsOverride threads(k == 1 ? o.togetherThreads : 0);
      if (k == 1) {
        while (firstBeats.load(std::memory_order_acquire) < o.togetherLag && !firstEnded.load())
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Logf(LogLevel::Info, "moho64: --together: the second copy starts (the first at beat %d), %d thread(s)",
             firstBeats.load(), ThreadsFromEnvironment());
      }
      rcs[k] = RunSession(o, k ? vfs2 : vfs, k ? hookDirs2 : hookDirs, uids, session, stream, k ? ".2" : "",
                          k ? nullptr : &firstBeats);
      if (k == 0) firstEnded = true;
    };
    std::thread first(copy, 0), second(copy, 1);
    first.join();
    second.join();
    rc = rcs[0] | rcs[1];
    for (const char* var : {"MOHO64_FINGERPRINT", "MOHO64_RECORD"})
      if (const char* file = getenv(var); file && *file) {
        long long at = 0, line = 0;
        if (SameBytes(file, std::string(file) + ".2", &at, &line)) {
          Logf(LogLevel::Info, "moho64: --together: %s: the two copies' files are the same (%lld bytes)", var, at);
        } else {
          Logf(LogLevel::Warning, "moho64: --together: %s: the two copies' files differ from byte %lld (line %lld)",
               var, at, line);
          std::fprintf(stderr, "moho64: --together: %s: the two copies' files differ from byte %lld (line %lld)\n",
                       var, at, line);
          rc |= 4;
        }
      }
    return rc;
  }
  for (int run = 0; run < (o.twice ? 2 : 1); ++run) {  // (one call site: the record's call chains stay the same)
    if (run == 1) {
      for (const char* var : {"MOHO64_RECORD", "MOHO64_FINGERPRINT"})
        if (const char* rec = getenv(var); rec && *rec) {
          const std::string second = std::string(rec) + ".2";
#ifdef _WIN32
          _putenv_s(var, second.c_str());
#else
          setenv(var, second.c_str(), 1);
#endif
        }
      Logf(LogLevel::Info, "moho64: --twice: the second run");
    }
    rc |= RunSession(o, vfs, hookDirs, uids, session, stream);
  }
  return rc;
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
  return RunTimes(o, vfs, hookDirs, uids, session, &*data);
}

// --session <file>: the game configuration a lobby hands over at Launch (`GameConfig = { ... }`), started the
// way CLobby::LaunchGame starts it (sim/session.h). The mods are the configuration's GameMods unless --mods
// names a list.
// The session a game configuration file describes, and (without --mods) its GameMods' uids into `uids`.
bool LoadGameConfig(const Options& o, const std::string& file, Vfs& vfs, const std::vector<std::string>& hookDirs,
                    SessionDescription* out, std::vector<std::string>* uids) {
  SessionDescription& session = *out;
  {
    ScriptState st(ScriptState::Kind::User, &vfs);
    st.SetHookDirs(hookDirs);
    lua_State* L = st.L();
    std::string err;
    if (!PushGameConfigFile(L, file, &err)) {
      Logf(LogLevel::Error, "moho64: %s", err.c_str());
      return false;
    }
    LaunchParams params;
    params.seed = o.hasSeed ? o.seed
                            : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!SessionFromGameConfig(L, -1, params, &session, &err)) {
      Logf(LogLevel::Error, "moho64: launch failed: %s", err.c_str());
      return false;
    }
    if (o.modsFile.empty()) {
      uids->clear();
      lua_getfield(L, -1, "GameMods");
      for (int i = 1; lua_istable(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        lua_getfield(L, -1, "uid");
        if (lua_isstring(L, -1)) uids->push_back(lua_tostring(L, -1));
        lua_pop(L, 2);
      }
    }
  }
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64: session %s: %zu armies, %zu command sources, focus army %d, seed %u, cheats %s",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, h.cheats ? "on" : "off");
  return true;
}

int RunGameConfig(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs,
                  std::vector<std::string> uids) {
  SessionDescription session;
  if (!LoadGameConfig(o, o.session, vfs, hookDirs, &session, &uids)) return 1;
  // --stream: the orders of a game that was played from this table (the interface host's --replay-out: every
  // batch it applied, an Advance per beat), played back here. The session is still a played one, not a replay.
  if (!o.stream.empty()) {
    const auto bytes = hostfs::ReadFile(o.stream);
    if (!bytes) {
      Logf(LogLevel::Error, "cannot read %s", o.stream.c_str());
      return 1;
    }
    Logf(LogLevel::Info, "moho64: playing the command stream %s (%zu bytes)", o.stream.c_str(), bytes->size());
    return RunTimes(o, vfs, hookDirs, uids, session, &*bytes);
  }
  int rc = RunTimes(o, vfs, hookDirs, uids, session, nullptr);
  if (!o.thenSession.empty()) {  // --then-session: the next game, after the first Sim is gone
    std::vector<std::string> uids2 = uids;
    SessionDescription next;
    if (!LoadGameConfig(o, o.thenSession, vfs, hookDirs, &next, &uids2)) return rc | 1;
    for (const char* var : {"MOHO64_RECORD", "MOHO64_FINGERPRINT"})
      if (const char* rec = getenv(var); rec && *rec) {
        const std::string then = std::string(rec) + ".then";
#ifdef _WIN32
        _putenv_s(var, then.c_str());
#else
        setenv(var, then.c_str(), 1);
#endif
      }
    Logf(LogLevel::Info, "moho64: --then-session: the next game, %s", o.thenSession.c_str());
    rc |= RunTimes(o, vfs, hookDirs, uids2, next, nullptr);
  }
  return rc;
}

// /map: the table FAF's SinglePlayerLaunch.lua builds for a campaign scenario (StartCommandLineSession 365-393 and
// SetupCampaignSession 70-127; the lobby's GetDefaultPlayerOptions per army, lobbycomm.lua 35), here without the
// interface's preferences: the player is 'Player', the factions are the stock four. The interface adds
// Options.ScenarioFile (ui/REQUESTS.md engine 31), and so does this. Without --mods the operation has the mods
// SetupCampaignSession gives it (line 124, mods.lua GetCampaignMods 718-726): the stock game's campaign mod
// "Supreme Commander Campaign Branch" (uid 6AAFE20A-..., mods.scd /mods/CampaignBranch, which FAF's init file
// mounts) when it is there, and no other; the table returns their uids after the session info. (Until
// 2026-10-16 this launch had none, so its blueprints were not the interface host's or the original's.)
const char kCampaignTable[] = R"lua(
return function(mapName, difficulty, faction, haveMods)
  if not string.find(mapName, '/') and not string.find(mapName, '\\') then
    local files = DiskFindFiles('/maps', mapName .. '_scenario.lua')
    if not files[1] then error('Could not find scenario file for map name "' .. mapName .. '"') end
    mapName = files[1]
  end
  -- maputil.lua LoadScenario 259-282 (with LoadScenarioInfoFile, -OptionsFile, -StringsFile)
  local function load(path)
    if not DiskGetFileInfo(path) then return nil end
    local data = {}
    doscript('/lua/dataInit.lua', data)
    doscript(path, data)
    return data
  end
  local data = load(mapName)
  local scenario = data and (data.version == 1 and data or data.ScenarioInfo)
  if not scenario then error('Unable to load map ' .. mapName) end
  local stem = string.gsub(mapName, '[^_]*$', '')
  local ok, options = pcall(load, stem .. 'options.lua')
  if ok and options and options.options then scenario.options = options.options end
  local ok2, strings = pcall(load, stem .. 'strings.lua')
  if ok2 and strings and strings.BriefingData then scenario.hasBriefing = true end
  scenario.file = mapName
  if scenario.type ~= 'campaign' then error(mapName .. ' is no campaign scenario') end
  local factionKeys = {'uef', 'aeon', 'cybran', 'seraphim'}
  local sessionInfo = {playerName = 'Player', createReplay = false, scenarioInfo = scenario, teamInfo = {}}
  for index, name in scenario.Configurations.standard.teams[1].armies do
    local o = {Team = 1, PlayerColor = 1, ArmyColor = 1, StartSpot = 1, Ready = false,
               Faction = table.getn(factionKeys) + 1, PlayerName = sessionInfo.playerName, AIPersonality = '',
               Human = true, Civilian = false}
    if index == 1 then
      o.PlayerName = sessionInfo.playerName
      o.Faction = faction
    else
      o.PlayerName = name
      o.Human = false
      o.Faction = 1
    end
    o.ArmyName = name
    sessionInfo.teamInfo[index] = o
  end
  scenario.Options = {FogOfWar = 'explored', Difficulty = difficulty, DoNotShareUnitCap = true, Timeouts = -1,
                      GameSpeed = 'normal', FACampaignFaction = factionKeys[faction], ScenarioFile = scenario.file}
  -- mods.lua GetCampaignMods: GetGameMods{['6AAFE20A-E851-11DB-B8BE-ECC755D89593'] = true}, the installed mods
  -- (AllMods: every /mods/*/mod_info.lua) of that uid that are not ui_only
  local mods = {}
  if not haveMods then
    for _, file in DiskFindFiles('/mods', 'mod_info.lua') do
      local info = {}
      if pcall(doscript, file, info) and info.uid == '6AAFE20A-E851-11DB-B8BE-ECC755D89593' and not info.ui_only then
        table.insert(mods, info.uid)
        break
      end
    end
    -- none: an empty list; else (as with --mods) the engine makes the session's __active_mods from the uids
    if not mods[1] then sessionInfo.scenarioMods = {} end
  end
  return sessionInfo, mods
end
)lua";

int RunSinglePlayer(const Options& o, Vfs& vfs, const std::vector<std::string>& hookDirs,
                    std::vector<std::string> uids) {
  SessionDescription session;
  {
    ScriptState st(ScriptState::Kind::User, &vfs);
    st.SetHookDirs(hookDirs);
    lua_State* L = st.L();
    const int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    bool ok = luaL_loadbuffer(L, kCampaignTable, sizeof kCampaignTable - 1, "=campaign table") == 0 &&
              lua_pcall(L, 0, 1, top + 1) == 0;
    if (ok) {
      lua_pushstring(L, o.spMap.c_str());
      lua_pushnumber(L, o.spDiff);
      lua_pushnumber(L, o.spFaction);
      lua_pushboolean(L, !o.modsFile.empty());
      ok = lua_pcall(L, 4, 2, top + 1) == 0;
    }
    if (!ok) {
      Logf(LogLevel::Error, "moho64: /map %s: %s", o.spMap.c_str(), lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
      return 1;
    }
    if (o.modsFile.empty()) {  // the campaign's mods (GetCampaignMods), as the table found them
      uids.clear();
      for (int i = 1; lua_istable(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        const bool more = lua_isstring(L, -1) != 0;
        if (more) uids.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
        if (!more) break;
      }
      for (const std::string& uid : uids) Logf(LogLevel::Info, "moho64: the campaign's mod %s", uid.c_str());
    }
    lua_pop(L, 1);
    SinglePlayerParams params;
    params.seed = o.hasSeed ? o.seed
                            : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string err;
    if (!SessionFromSinglePlayerInfo(L, -1, params, &session, &err)) {
      Logf(LogLevel::Error, "moho64: launch failed: %s", err.c_str());
      return 1;
    }
  }
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64: single-player session %s: %zu armies, focus army %d, seed %u, difficulty %d, faction %d",
       h.map.c_str(), h.armies.size(), session.focusArmy + 1, h.seed, o.spDiff, o.spFaction);
  return RunTimes(o, vfs, hookDirs, uids, session, nullptr);
}

// --scan-moments: ScanReplayMoments on a thread of its own, as the interface runs it beside the game it shows; this
// thread reads the moments while the scan goes on (Moments::CopySince: the new ones since the last look) and writes
// each as a JSON line (armies 0-based, as Moment::army; -1 none):
//   {"tick":5123,"kind":"CommanderDied","army":1,"pos":[312.5,18.2,401.0],"unit":"uel0001","id":"0x200000"}
// and at the end one line per fight bucket that saw anything:
//   {"fight":{"army":1,"tick0":5100,"damage":1834.5,"hits":212,"deaths":7}}
// then a summary on stderr.
int RunScan(const Options& o, const std::vector<std::string>& uids) {
  ScanArgs args;
  args.init = o.init;
  args.folders = o.folders;
  args.maps = o.moreMaps;
  args.modUids = uids;
  args.replay = o.scanReplay;
  args.threads = o.scanThreads;
  args.ticks = o.ticks;
  args.observe = !o.scanOff;
  args.mounts = o.scanMounts;
  args.skipMounts = o.scanSkips;
  args.fingerprint = true;
  std::FILE* f = o.scanOut.empty() ? stdout : std::fopen(o.scanOut.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "moho64: cannot write %s\n", o.scanOut.c_str());
    return 1;
  }
  Moments m;
  std::atomic<int> tick{0};
  std::atomic<bool> cancel{false}, running{true};
  bool ok = false;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread scan([&] {
    ok = ScanReplayMoments(args, &m, &tick, &cancel);
    running = false;
  });
  size_t shown = 0, held = 0;
  // the buckets as a reader keeps them, taken while the scan runs: the last one taken is taken again (it may still
  // have been filling); at the end they must be the scan's own
  std::vector<std::vector<FightBucket>> copied;
  auto show = [&] {
    std::vector<Moment> fresh;
    const size_t from = held ? held - 1 : 0;
    for (std::vector<FightBucket>& list : copied)
      if (list.size() > from) list.resize(from);
    held = m.CopySince(shown, from, &fresh, &copied);
    shown += fresh.size();
    for (const Moment& x : fresh) {
      char id[32] = "";
      if (x.entity != 0xffffffffu) std::snprintf(id, sizeof id, ",\"id\":\"%#x\"", x.entity);
      std::fprintf(f, "{\"tick\":%d,\"kind\":\"%s\",\"army\":%d,\"pos\":[%.1f,%.1f,%.1f],\"unit\":\"%s\"%s}\n", x.tick,
                   MomentKindName(x.kind), x.army, x.pos.x, x.pos.y, x.pos.z, x.unit.c_str(), id);
    }
    std::fflush(f);
  };
  while (running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (o.scanCancelAt > 0 && tick.load() >= o.scanCancelAt) cancel = true;
    show();
  }
  scan.join();
  show();
  bool copiesAgree = copied.size() == m.fights.size();
  for (size_t a = 0; copiesAgree && a < copied.size(); ++a) {
    copiesAgree = copied[a].size() == m.fights[a].size();
    for (size_t b = 0; copiesAgree && b < copied[a].size(); ++b)
      copiesAgree = copied[a][b].tick0 == m.fights[a][b].tick0 && copied[a][b].damage == m.fights[a][b].damage &&
                    copied[a][b].hits == m.fights[a][b].hits && copied[a][b].deaths == m.fights[a][b].deaths;
  }
  size_t buckets = 0;
  float damage = 0;
  long long hits = 0, deaths = 0;
  for (size_t a = 0; a < m.fights.size(); ++a)
    for (const FightBucket& b : m.fights[a]) {
      damage += b.damage;
      hits += b.hits;
      deaths += b.deaths;
      if (!b.hits && !b.deaths) continue;
      ++buckets;
      std::fprintf(f, "{\"fight\":{\"army\":%d,\"tick0\":%d,\"damage\":%.1f,\"hits\":%d,\"deaths\":%d}}\n",
                   a < static_cast<size_t>(m.armies) ? static_cast<int>(a) : -1, b.tick0,
                   b.damage, b.hits, b.deaths);
    }
  if (f != stdout) std::fclose(f);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr,
               "moho64: --scan-moments: %s at tick %d in %.1f s (%.1fx game speed); %zu moments, %zu fight buckets with "
               "something (%lld hits, %.0f damage, %lld deaths); the buckets copied while it ran %s%s%s\n",
               ok ? "done" : m.error.empty() ? "cancelled" : "FAILED", m.lastTick, secs,
               secs > 0 ? m.lastTick / 10.0 / secs : 0.0, m.moments.size(), buckets, hits, damage, deaths,
               copiesAgree ? "agree" : "DIFFER", m.error.empty() ? "" : ": ", m.error.c_str());
  return (ok || (o.scanCancelAt > 0 && m.error.empty())) && copiesAgree ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* shift = getenv("MOHO64_LAYOUT_SHIFT"); shift && std::atoi(shift) > 0) ShiftLayout(std::atoi(shift));
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
    else if (a == "--stream") o.stream = next();
    else if (a == "--maps") o.moreMaps.push_back(next());
    else if (a == "--ticks") o.ticks = std::atoi(next().c_str());
    else if (a == "--sim-lua") o.simLua = next();
    else if (a == "--raw-blueprints") SimBlueprints::reflect = false;
    else if (a == "--twice") o.twice = true;
    else if (a == "--then-session") o.thenSession = next();
    else if (a == "--workers-selftest") {  // the worker pool's own check (core/workers.h), with MOHO64_THREADS / _PAR_CHECK
      WorkerPool pool(ThreadsFromEnvironment(), ParCheckFromEnvironment());
      SimFpScope fp;  // (as a sim entry does: the calling thread takes tasks too)
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = pool.SelfTest();
      if (prof::On())  // (the profiler's parallel view of the self-test's calls)
        std::fputs(pool.Stats()->Summary(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()).c_str(),
                   stdout);
      std::printf("workers self-test: %d thread(s): %s\n", pool.Threads(), ok ? "ok" : "FAILED");
      return ok ? 0 : 1;
    }
    else if (a == "--together") o.together = true;
    else if (a == "--together-lag") o.togetherLag = std::atoi(next().c_str());
    else if (a == "--together-threads") {
      const std::string v = next();
      o.togetherThreads = v == "cores" ? static_cast<int>(std::thread::hardware_concurrency()) : std::atoi(v.c_str());
    }
    else if (a == "--focus") {
      o.focus = std::atoi(next().c_str());
      o.hasFocus = true;
    }
    else if (a == "--scan-moments") o.scanReplay = next();
    else if (a == "--scan-out") o.scanOut = next();
    else if (a == "--scan-off") o.scanOff = true;
    else if (a == "--scan-threads") o.scanThreads = std::atoi(next().c_str());
    else if (a == "--scan-cancel-at") o.scanCancelAt = std::atoi(next().c_str());
    else if (a == "--scan-mount") {
      std::string folder = next();
      o.scanMounts.emplace_back(folder, next());
    }
    else if (a == "--scan-skip") o.scanSkips.push_back(next());
    else if (a == "--net") {  // host:<port> | join:<port>
      const std::string v = next();
      const size_t colon = v.find(':');
      o.netMode = v.substr(0, colon);
      o.netPort = static_cast<uint16_t>(colon == std::string::npos ? 0 : std::atoi(v.c_str() + colon + 1));
      if ((o.netMode != "host" && o.netMode != "join") || o.netPort == 0) {
        std::fprintf(stderr, "--net host:<port> | join:<port>\n");
        return 2;
      }
      if (o.netMode == "join" && o.netClient == 0) o.netClient = 1;
    }
    else if (a == "--net-client") o.netClient = std::atoi(next().c_str());
    else if (a == "--net-delay") o.netDelay = std::atoi(next().c_str());
    else if (a == "--net-stream-out") o.netStreamOut = next();
    else if (a == "/map") o.spMap = next();
    else if (a == "/diff") o.spDiff = std::atoi(next().c_str());
    else if (a == "/faction") o.spFaction = std::atoi(next().c_str());
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (!o.logFile.empty()) LogOpenFile(o.logFile);
  if (o.quiet) LogSetEcho(false);
  InstallCrashHandler();  // a crash writes its place and stack to the log (core/crash.h)
  if (o.init.empty()) {
    std::fprintf(stderr, "usage: moho64 --init <init.lua> [--drive c=/dir] [--mods uids.txt] [--check-lua] [--rules]\n");
    return 2;
  }
  Vfs vfs;
  std::vector<std::string> hookDirs;
  if (!MountGame(o, vfs, &hookDirs)) return 1;
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
  if (!o.spMap.empty()) rc |= RunSinglePlayer(o, vfs, hookDirs, uids);
  if (!o.scanReplay.empty()) rc |= RunScan(o, uids);
  return rc;
}
