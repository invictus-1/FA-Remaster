#include "app/game_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"

namespace moho::ui {

GameSession::GameSession(Vfs* vfs, std::vector<std::string> hookDirs) : vfs_(vfs), hookDirs_(std::move(hookDirs)) {}
GameSession::~GameSession() = default;

bool GameSession::Launch(lua_State* L, int config, std::string* why) {
  if (config < 0) config = lua_gettop(L) + config + 1;
  if (sim_) {
    *why = "a session is running already";
    return false;
  }
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  auto t0 = std::chrono::steady_clock::now();
  SessionDescription session;
  LaunchParams params;
  params.seed = static_cast<uint32_t>(t0.time_since_epoch().count());
  if (!SessionFromGameConfig(L, config, params, &session, why)) return false;

  // The session's mods are the configuration's GameMods, by uid, in order.
  std::vector<std::string> uids;
  lua_getfield(L, config, "GameMods");
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
  lua_pop(L, 1);

  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session %s: %zu armies, %zu command sources, focus army %d, seed %u, %zu mods",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, uids.size());
  auto sim = std::make_unique<Sim>(vfs_, hookDirs_, uids);
  sim->sessionMods = h.mods;
  if (!sim->LoadRules()) {
    *why = "the game's rules (blueprints) did not load";
    return false;
  }
  auto t1 = std::chrono::steady_clock::now();
  if (!sim->Start(session)) {
    *why = "the simulation did not start";
    return false;
  }
  auto t2 = std::chrono::steady_clock::now();
  Logf(LogLevel::Info, "moho64ui: session started: rules %.2f s, simulation start-up %.2f s", secs(t0, t1), secs(t1, t2));
  sim_ = std::move(sim);
  modUids_ = uids;
  sessionMods_ = h.mods;
  map_ = h.map;
  armies_ = h.armies.size();
  seed_ = h.seed;
  due_ = 0;
  return true;
}

bool GameSession::LoadInterfaceRules(lua_State* userState) {
  auto t0 = std::chrono::steady_clock::now();
  rules_ = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
  rules_->SetHookDirs(hookDirs_);
  SimBlueprints::StartRecording(*rules_);
  lua_State* R = rules_->L();
  if (!sessionMods_.empty() && PushSerializedLua(R, sessionMods_) && lua_istable(R, -1)) lua_setglobal(R, "__active_mods");
  else SetActiveMods(*rules_, modUids_);
  lua_settop(R, 0);
  if (!rules_->DoScript("/lua/ruleinit.lua")) return false;
  interfaceBlueprints_ = std::make_unique<SimBlueprints>();
  interfaceBlueprints_->fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  interfaceBlueprints_->CopyToSim(R, userState);
  RegisterCategoryBindings(userState, interfaceBlueprints_.get());
  Logf(LogLevel::Info, "moho64ui: session: %zu blueprints in the interface's state, %.2f s", interfaceBlueprints_->Count(),
       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

void GameSession::Advance(double seconds) {
  if (!sim_) return;
  due_ += seconds;
  if (due_ > 1.0) due_ = 1.0;  // after a long stall, catch up by at most a second of beats
  while (due_ >= 0.1) {
    due_ -= 0.1;
    sim_->collectSync = static_cast<bool>(onSync);
    if (sim_->Paused()) sim_->PausedBeat();
    else sim_->Tick();
    if (onSync)
      for (const std::string& sync : sim_->TakeSync()) onSync(sync);
  }
}

uint32_t GameSession::tick() const { return sim_ ? sim_->tick() : 0; }
size_t GameSession::unitCount() const { return sim_ ? sim_->units().size() : 0; }

std::vector<std::string> GameSession::Status() const {
  std::vector<std::string> lines;
  if (!sim_) return lines;
  char buf[200];
  uint32_t t = sim_->tick();
  lines.push_back("The simulation is running. The in-game interface and the world view are not built yet.");
  std::snprintf(buf, sizeof buf, "Map %s, %zu armies, seed %u", map_.c_str(), armies_, seed_);
  lines.push_back(buf);
  std::snprintf(buf, sizeof buf, "Game time %u:%02u (tick %u), %zu units", t / 600, (t / 10) % 60, t, sim_->units().size());
  lines.push_back(buf);
  return lines;
}

void GameSession::DrawStatus(DrawTarget& target) const {
  if (!sim_) return;
  const int size = 18;
  FontMetrics m = GetFontMetrics("Arial", size);
  float line = m.height + 6, x = 40, y = 40;
  auto lines = Status();
  float w = 0;
  for (const auto& s : lines) w = std::max(w, StringAdvance("Arial", size, s));
  target.FillRect(Rect{x - 16, y - 12, x + w + 16, y + line * static_cast<float>(lines.size()) + 12}, 0xc0101820u);
  for (const auto& s : lines) {
    DrawString(target, "Arial", size, s, x, y + m.ascent, 0xffe0e8f0u);
    y += line;
  }
}

}  // namespace moho::ui
