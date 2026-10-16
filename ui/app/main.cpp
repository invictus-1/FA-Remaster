// moho64ui -- the interface track's host: mount the game data the way the original does, boot the
// user Lua state and enter the game's interface scripts. Without --window it runs frames headless
// on its own clock: the log says how far the scripts came and which engine functions they missed,
// and the screen can be logged (--dump-ui) or saved as a picture (--screenshot). With --window the
// interface runs live in a window, with the mouse.
//
//   moho64ui --init <init.lua> [--mods uids.txt] [--prefs <file>] [--stage userinit|setup|frontend]
//            [--size 1920x1080] [--window] [--frames N] [--dump-ui] [--screenshot file.png]
//            [--lua <file>] [--list-bindings] [--log out.log] [--quiet] [-- <the game's own arguments>]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

#include "app/backdrop.h"
#include "app/game_session.h"
#include "app/window.h"
#include "core/crash.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#ifdef MOHO64UI_WORLD_PICTURE
#include "render/remasters.h"
#include "render/view.h"
#endif
#include "script/script_state.h"
#include "sim/session.h"
#include "sound/sound_system.h"
#include "user/bindings.h"
#include "user/user_state.h"

using namespace moho;

namespace {

struct Options {
  std::string init;
  std::string modsFile;
  std::string logFile;
  std::string prefsFile;
  std::string stage = "frontend";
  std::string luaFile;  // --lua file: a host Lua file run in the user state at the end (debugging)
  std::map<std::string, std::string> folders;  // SHGetFolderPath answers
  std::vector<std::string> gameArgs;
  int frames = 0;
  bool listBindings = false;
  bool quiet = false;
};

Options* g_opts = nullptr;
int width = 1920, height = 1080;  // --size: the head's size in pixels
bool dumpUi = false;              // --dump-ui: log the control tree at the end
std::string screenshot;           // --screenshot file.png: draw the head at the end and save the picture
bool windowed = false;            // --window: run live in a window until it is closed
bool noSound = false;             // --no-sound: a live window without the sound device
// The world shown by the renderer in the window itself, the interface laid over it on the card
// (no read-back), and without a game the menus through the renderer's picture without a world
// (a window that has had the renderer's swap chain no longer shows a plain copy). --gdi: the old
// way, the world's picture read back and everything copied to the window.
bool directWindow = true;
// --window-capture <prefix>: what the window shows, read back by the renderer (checking):
// <prefix>_game.png 300 frames into a game, <prefix>_menu.png 300 frames after it is left.
std::string windowCapture;
int capturedFrames = 0, menuFrames = 0;
bool gameSeen = false;
bool fullScreen = false;          // --fullscreen: the live window fills the main screen, without a frame
int wantBackdrop = -1;            // --backdrop / --no-backdrop: the menu's moving background (by itself: in a live window)
// --day-weather (a game starts at nine in the morning) / --day <hour> / --day-length <game seconds,
// 0: the hour stands still> / --weather <kind> (changing, cold, dry, none, clear, cloudy, overcast,
// rain, storm, fog, snow) / --no-weather: the day and the weather in the picture of a game whose
// configuration does not say (a game from the lobby says: its options RemasterDayStart,
// RemasterDayLength, RemasterWeather). Without them such a game has the map's own fixed light.
// --map-edge frame|beyond: likewise the map's edge (RemasterMapEdge); without it, "beyond".
ui::GameSession::Sky sky;
// --maps <folder> (may be given more than once): more map folders, beside the game's own at /maps
// (maps in the making). --remasters <folder>: the remastered units' own files, one folder per
// unit (render/remasters.h). --own <folder>: this program's own files (ui/data); without it they
// are looked for beside the program.
std::vector<std::string> moreMaps;
std::string remastersFolder, ownFolder;
bool noWorld = false;             // --no-world: a launched game gets no picture of the world (no graphics device is opened)
std::string sessionIn;            // --session file: a recorded game configuration to start a game from at once
std::string gameLuaFile;          // --game-lua file: a host Lua file run in a launched game's state once its interface is up
long long session_seed = -1;      // --seed N: the random seed of a launched game (default: from the clock)
std::string replayOut;            // --replay-out <prefix>: each game's orders, beat by beat (GameSession::replayOut)
std::string sessionFile;          // --session-out file: where a launched game's configuration is written

// The init file and its mounting are the same as in the engine's host (src/app/main.cpp), which
// keeps them to itself for now: see ui/REQUESTS.md.
std::string SystemFolder(const std::string& name) {
#ifdef _WIN32
  static const std::map<std::string, int> ids = {
      {"LOCAL_APPDATA", CSIDL_LOCAL_APPDATA}, {"APPDATA", CSIDL_APPDATA}, {"PERSONAL", CSIDL_PERSONAL},
      {"COMMON_APPDATA", CSIDL_COMMON_APPDATA}, {"COMMON_DOCUMENTS", CSIDL_COMMON_DOCUMENTS}};
  auto it = ids.find(name);
  wchar_t buf[MAX_PATH];
  if (it != ids.end() && SUCCEEDED(SHGetFolderPathW(nullptr, it->second, nullptr, 0, buf))) {
    char out[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
  }
#endif
  return "C:/Users/Player/" + name + "/";
}

int l_SHGetFolderPath(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  auto it = g_opts->folders.find(name);
  std::string v = it != g_opts->folders.end() ? it->second : SystemFolder(name);
  if (!v.empty() && v.back() != '/' && v.back() != '\\') v += '\\';
  lua_pushstring(L, v.c_str());
  return 1;
}

// Run the init file in its own state and mount what it lists in `path`.
bool RunInit(const Options& o, Vfs& vfs, std::vector<std::string>& hookDirs) {
  ScriptState init(ScriptState::Kind::Init, &vfs);
  lua_State* L = init.L();
  std::string dir = o.init;
  size_t s = dir.find_last_of("/\\");
  dir = s == std::string::npos ? "." : dir.substr(0, s);
  lua_pushstring(L, dir.c_str());
  lua_setglobal(L, "InitFileDir");
  lua_pushcfunction(L, l_SHGetFolderPath);
  lua_setglobal(L, "SHGetFolderPath");
  auto src = hostfs::ReadFile(o.init);
  if (!src) {
    Logf(LogLevel::Error, "cannot read init file %s", o.init.c_str());
    return false;
  }
  if (!init.DoString(*src, "@" + hostfs::DisplayPath(o.init))) return false;

  lua_getglobal(L, "path");
  if (!lua_istable(L, -1)) {
    Logf(LogLevel::Error, "init file did not define `path'");
    return false;
  }
  for (int i = 1;; ++i) {
    lua_rawgeti(L, -1, i);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_getfield(L, -1, "dir");
    lua_getfield(L, -2, "mountpoint");
    std::string mdir = lua_isstring(L, -2) ? lua_tostring(L, -2) : "";
    std::string mp = lua_isstring(L, -1) ? lua_tostring(L, -1) : "/";
    lua_pop(L, 3);
    if (mdir.find('*') != std::string::npos) {  // "gamedata\*.scd": mount every match
      std::string base = mdir.substr(0, mdir.find_last_of("/\\"));
      for (const auto& n : hostfs::Glob(mdir)) {
        if (n == "." || n == "..") continue;
        vfs.Mount(base + "/" + n, mp);
      }
    } else {
      vfs.Mount(mdir, mp);
    }
  }
  lua_pop(L, 1);
  lua_getglobal(L, "hook");
  if (lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      hookDirs.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  g_opts = &o;
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
    else if (a == "--prefs") o.prefsFile = next();
    else if (a == "--stage") o.stage = next();
    else if (a == "--frames") o.frames = std::atoi(next().c_str());
    else if (a == "--lua") o.luaFile = next();
    else if (a == "--list-bindings") o.listBindings = true;
    else if (a == "--quiet") o.quiet = true;
    else if (a == "--dump-ui") dumpUi = true;
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--window") windowed = true;
    else if (a == "--gdi") directWindow = false;
    else if (a == "--window-capture") windowCapture = next();
    else if (a == "--session-out") sessionFile = next();
    else if (a == "--game-lua") gameLuaFile = next();
    else if (a == "--session") sessionIn = next();
    else if (a == "--no-world") noWorld = true;
    else if (a == "--no-sound") noSound = true;
    else if (a == "--fullscreen") fullScreen = windowed = true;
    else if (a == "--backdrop") wantBackdrop = 1;
    else if (a == "--no-backdrop") wantBackdrop = 0;
    else if (a == "--day-weather") sky.startHour = 9;
    else if (a == "--day") sky.startHour = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--day-length") sky.dayLengthSeconds = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--weather") sky.weather = next();
    else if (a == "--no-weather") sky.weather = "none";
    else if (a == "--map-edge") sky.edge = next();
    else if (a == "--maps") moreMaps.push_back(next());
    else if (a == "--remasters") remastersFolder = next();
    else if (a == "--own") ownFolder = next();
    else if (a == "--seed") session_seed = static_cast<long long>(std::strtoull(next().c_str(), nullptr, 10));
    else if (a == "--replay-out") replayOut = next();
    else if (a == "--size") {
      std::string v = next();  // 1920x1080
      size_t x = v.find('x');
      if (x != std::string::npos) {
        width = std::atoi(v.substr(0, x).c_str());
        height = std::atoi(v.substr(x + 1).c_str());
      }
    } else if (a == "--") {
      while (++i < argc) o.gameArgs.push_back(argv[i]);
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (!o.logFile.empty()) LogOpenFile(o.logFile);
  InstallCrashHandler();  // (a fatal fault writes its place and frames to the log: src/core/crash.h)
  if (o.quiet) LogSetEcho(false);
  if (o.init.empty() || width < 1 || height < 1 ||
      (o.stage != "userinit" && o.stage != "setup" && o.stage != "frontend")) {
    std::fprintf(stderr,
                 "usage: moho64ui --init <init.lua> [--mods uids.txt] [--prefs file] [--stage userinit|setup|frontend]\n"
                 "                [--size 1920x1080] [--window] [--frames N] [--dump-ui] [--screenshot file.png]\n"
                 "                [--lua file] [--list-bindings] [--log out.log] [--quiet] [--no-sound] [--fullscreen] [-- game arguments]\n");
    return 2;
  }
  if (fullScreen && !ui::Window::ScreenSize(&width, &height)) fullScreen = false;  // the head is the screen
  Vfs vfs;
  ui::UserState::Config cfg;
  cfg.width = width;
  cfg.height = height;
  if (!RunInit(o, vfs, cfg.hookDirs)) return 1;
  for (const std::string& folder : moreMaps) {
    if (!vfs.Mount(folder, "/maps")) Logf(LogLevel::Warning, "moho64ui: --maps %s: no such folder", folder.c_str());
  }
  // This program's own files: what it adds to the game's scripts (ui/data/hook, taken as one more
  // hook folder, so the game's files stay as they are). Beside the program, or in its source tree.
  if (ownFolder.empty()) {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t cut = dir.find_last_of("/\\");
    dir = cut == std::string::npos ? "." : dir.substr(0, cut);
    for (const std::string& candidate : {dir + "/data", dir + "/../ui/data"}) {
      if (ownFolder.empty() && hostfs::IsDirectory(candidate + "/hook")) ownFolder = candidate;
    }
  }
  if (!ownFolder.empty() && vfs.Mount(ownFolder, "/moho64ui")) cfg.hookDirs.push_back("/moho64ui/hook");
  else Logf(LogLevel::Warning, "moho64ui: this program's own files were not found (--own <folder>): the lobby has no day and weather options");
#ifdef MOHO64UI_WORLD_PICTURE
  if (!remastersFolder.empty()) {
    Logf(LogLevel::Info, "moho64ui: remastered units: %d from %s", render::MountRemasters(&vfs, remastersFolder), hostfs::DisplayPath(remastersFolder).c_str());
  }
#endif
  Logf(LogLevel::Info, "moho64ui: %zu search paths, %zu files", vfs.MountCount(), vfs.FileCount());
  if (!o.modsFile.empty()) {
    std::ifstream f(o.modsFile);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty() && line[0] != '#') cfg.modUids.push_back(line);
    }
  }
  cfg.gameArgs = o.gameArgs;
  cfg.prefsFile = o.prefsFile;
  // A recorded game needs its launch table kept too: a game launched from the lobby writes it
  // beside the record unless a place of its own was named.
  if (sessionFile.empty() && !replayOut.empty()) sessionFile = replayOut + ".session.lua";
  cfg.sessionFile = sessionFile;

  // The game's sounds, for the front end's state and a game's alike. Only a live window plays
  // them aloud; a run without a window mixes them when a test asks.
  ui::sound::SoundSystem sounds(&vfs);
  cfg.sound = &sounds;
  if (windowed && !noSound && !sounds.OpenDevice()) Logf(LogLevel::Warning, "moho64ui: no sound device: the run is silent");

  // A game launched from the lobby: the simulation starts from the game configuration and the host
  // runs its beats beside the interface's frames.
  ui::GameSession session(&vfs, cfg.hookDirs);
  session.sounds = &sounds;
  if (session_seed >= 0) session.seedGiven = true, session.seed = static_cast<uint32_t>(session_seed);
  session.wantPicture = !noWorld;
  session.waitForWindow = windowed && directWindow;  // (a launched table's picture opens on the window)
  session.sky = sky;
  session.replayOut = replayOut;
  session.replayTable = !sessionIn.empty() ? sessionIn : sessionFile;  // (a lobby game's table is written there)
  session.replayMaps = moreMaps;
  cfg.launchSession = [&session](lua_State* L, int config, std::string* why) { return session.Launch(L, config, why); };
  // The player's own files (replays, saved games) live beside the preferences file (special_files.cpp);
  // every game is written there as LastGame.SCFAReplay, and the Replay dialog plays them back.
  if (!cfg.prefsFile.empty()) {
    const std::string prefs = hostfs::CleanPath(cfg.prefsFile);
    const size_t slash = prefs.find_last_of('/');
    cfg.userDir = slash == std::string::npos ? std::string(".") : prefs.substr(0, slash);
  }
  session.userDir = cfg.userDir;
  cfg.launchReplay = [&session](lua_State* L, const std::string& file, std::string* why) { return session.LaunchReplay(L, file, why); };
  cfg.currentReplay = [&session]() { return session.currentReplay; };

  // The front end's background: the world's picture of a map behind the main menu, on a thread
  // of its own. In a live window unless told otherwise; not when a game starts at once.
  std::unique_ptr<ui::Backdrop> backdrop;
  if ((wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && sessionIn.empty() && !noWorld) {
    backdrop = std::make_unique<ui::Backdrop>(&vfs, "/maps/SCMP_009/SCMP_009.scmap");
    cfg.backdrop = backdrop.get();
  }
  session.releaseWorlds = [&backdrop]() {
    if (backdrop) backdrop->Stop();
  };
  // The scene's samples (the player's Anti-Aliasing) are set before any world is made, and
  // afterwards only while none lives: the background begins once they are known.
  auto beginBackdrop = [&backdrop]() {
#ifdef MOHO64UI_WORLD_PICTURE
    render::gSceneSamples = ui::Looks().sceneSamples;
#endif
    if (backdrop) backdrop->Begin(ui::Looks().vulkan);
  };
  // The front end's state. A new one is made when a game is left (the original makes a new state
  // for every change between front end and game, FA exe 0x885de0).
  auto front = std::make_unique<ui::UserState>(&vfs, cfg);
  bool ok = front->Boot();
  // The player's options take effect at the start, as in the original (its app start-up runs
  // /lua/options/optionslogic.lua's Apply: StartupHelpers::OPTIONS_Apply, FA exe 0x8c6cf0, called
  // at 0x8cf81f): every option's `set` with its saved value (sound volumes, the camera's shake,
  // the program's own looks). INFERRED: the argument (startup = true).
  if (ok && !front->RunString("import('/lua/options/optionslogic.lua').Apply(true)", "=(options at start-up)"))
    Logf(LogLevel::Warning, "moho64ui: the options could not be applied at the start");
  // (The window's frame rate at the start is the player's "Frame Rate" option, set by the Apply
  // above; the game's own Vertical Sync and Frametime skip their `set` at the start.) The game's
  // Anti-Aliasing skips its `set` at the start too, the original taking it when it makes its
  // device: here from the saved options (4 when there are none, as the hook's default).
  if (ok && !front->RunString("local o = import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {} "
                              "ConExecute('SC_AntiAliasingSamples ' .. tostring(o.antialiasing or 4))",
                              "=(anti-aliasing at start-up)"))
    Logf(LogLevel::Warning, "moho64ui: the anti-aliasing option could not be read at the start");
  beginBackdrop();
  if (ok && o.stage != "userinit") ok = front->StartMainScript();
  if (ok && o.stage == "frontend") ok = front->StartFrontEnd();
  if (!o.luaFile.empty()) {  // debugging aid: a Lua file of your own, run before the frames (it may fork threads)
    if (auto code = hostfs::ReadFile(o.luaFile)) front->RunString(*code, "@" + o.luaFile);
    else Logf(LogLevel::Error, "cannot read %s", o.luaFile.c_str());
  }
  if (ok && !sessionIn.empty()) {  // start a game at once from a recorded configuration (what --session-out wrote)
    lua_State* L = front->L();
    std::string why;
    if (!PushGameConfigFile(L, sessionIn, &why) || !session.Launch(L, -1, &why)) {
      Logf(LogLevel::Error, "moho64ui: --session %s: %s", sessionIn.c_str(), why.c_str());
      ok = false;
    }
    lua_settop(L, 0);
  }
  // When a game has been launched, its interface gets a state of its own: the session's start
  // script, the rules, the root frame and the main script again, then the in-game interface.
  // The front end's state stays as it is and is no longer shown.
  std::unique_ptr<ui::UserState> game;
  ui::UserState* shown = front.get();
  auto startGameInterface = [&]() {
    if (game || !session.running()) return;
    if (backdrop) backdrop->Stop();  // the game has its own picture of the world
    // (A new state is made for the size the head has NOW: the window may have been resized.)
    cfg.width = shown->ui().width(), cfg.height = shown->ui().height();
    ui::UserState::Config gameCfg = cfg;
    gameCfg.bootScript = "/lua/SessionInit.lua";
    gameCfg.modUids = session.modUids();
    gameCfg.launchSession = nullptr;
    gameCfg.launchReplay = nullptr;  // (a replay starts from the front end)
    gameCfg.sim = session.sim();
    gameCfg.commandSource = session.commandSource();
    gameCfg.world = session.world();
    gameCfg.pace = &session.pace;
    game = std::make_unique<ui::UserState>(&vfs, gameCfg);
    bool up = session.LoadInterfaceRules(game->L());
    if (up) ui::InstallUnitCategoryFunctions(game->L());
    up = up && game->Boot() && game->StartMainScript() && game->StartGame();
    session.RunFirstBeat();
    // What the simulation told its interface before there was one (the first beats).
    if (up)
      for (const std::string& sync : session.TakePendingSync()) game->DeliverSync(sync);
    Logf(up ? LogLevel::Info : LogLevel::Error, "moho64ui: the in-game interface %s", up ? "is up" : "did not come up");
    shown = game.get();
    if (up) {
      // The simulation is already there when the interface's state comes up, so loading ends at once.
      game->GameLoaded();
      if (!gameLuaFile.empty()) {  // a Lua file of your own, run in the game's state (tests, looking around)
        if (auto code = hostfs::ReadFile(gameLuaFile)) game->RunString(*code, "@" + gameLuaFile);
        else Logf(LogLevel::Error, "cannot read %s", gameLuaFile.c_str());
      }
      session.onSync = [&game](const std::string& sync) { game->DeliverSync(sync); };
      session.takeOrders = [&game]() { return ui::TakeOrders(game->L()); };
    }
  };
  // A script has asked to leave the game (ExitGame) or to start it over (RestartSession): done
  // here, between two frames. Leaving: the game's interface and the session go, and a new front
  // end comes up with its main menu, as after the program's start. Starting over: the same
  // launch again; its interface comes up as it did the first time.
  auto leaveOrRestart = [&]() {
    int request = ui::TakeSessionRequest();
    if (!request || !session.running()) return;
    cfg.width = shown->ui().width(), cfg.height = shown->ui().height();  // (the head's size now)
    shown = front.get();
    game.reset();
    sounds.StopAll();
    if (request == 2) {
      std::string why;
      if (session.Relaunch(&why)) {
        Logf(LogLevel::Info, "moho64ui: the game starts over");
        return;
      }
      Logf(LogLevel::Error, "moho64ui: the game could not be started over: %s", why.c_str());
    }
    session.End();
    Logf(LogLevel::Info, "moho64ui: the game is left: back to the front end");
    if ((wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && !noWorld) {
      backdrop.reset();
      backdrop = std::make_unique<ui::Backdrop>(&vfs, "/maps/SCMP_009/SCMP_009.scmap");
      cfg.backdrop = backdrop.get();
      beginBackdrop();  // (the game's picture is gone: the samples may follow the option again)
    }
    auto fresh = std::make_unique<ui::UserState>(&vfs, cfg);
    if (fresh->Boot() && fresh->StartMainScript() && fresh->StartFrontEnd()) {
      front = std::move(fresh);
      shown = front.get();
    } else {
      Logf(LogLevel::Error, "moho64ui: the front end did not come up again");
    }
  };
  // The world's sounds are heard from where the game's camera is.
  auto listen = [&]() {
    float eye[3], focus[3], right[3], zoom = 0;
    if (game && ui::CameraListener(game->L(), eye, focus, right, &zoom)) sounds.SetListener(eye, focus, right, zoom);
  };
  int done = 0;
  if (windowed) {  // live: real time, the mouse, until the window is closed (or for --frames frames)
    // The world is shown by the renderer in the game's window itself, the interface laid over it
    // on the card (no picture read back and copied again): the renderer track's way, with Chris's
    // go-ahead (2026-10-15). --gdi: the old way (see directWindow).
    ui::WindowHooks hooks;
    if (directWindow) {
      hooks.opened = [&](void* handle) {
        session.window = handle;
        session.OpenPictureIfDue();  // (a launched table's game: its picture opens now, the window still hidden)
      };
      hooks.direct = [&]() { return session.DrawsToWindow(); };
      hooks.present = [&](const ui::SoftwareTarget& layer) { session.PresentToWindow(layer); };
    }
    done = ui::RunWindow([&]() -> ui::UserState& { return *shown; }, width, height, o.frames,
                         [&](double seconds, ui::SoftwareTarget& target) {
                           session.OpenPictureIfDue();
                           session.Advance(seconds);
                           listen();
                           if (!game) session.DrawStatus(target);  // the host's notice, until the game's interface is there
                           leaveOrRestart();
                           startGameInterface();
                           if (!windowCapture.empty() && session.DrawsToWindow()) {
                             if (game && ++capturedFrames == 300) session.CaptureWindow(windowCapture + "_game.png"), gameSeen = true;
                             if (!game && gameSeen && ++menuFrames == 300) session.CaptureWindow(windowCapture + "_menu.png");
                           }
                         },
                         fullScreen, hooks);
    if (done < 0) ok = false;
  } else {
    for (; done < o.frames && !ui::ExitRequested(); ++done) {
      shown->Frame(1.0 / 60.0);
      session.Advance(1.0 / 60.0);
      listen();
      leaveOrRestart();
      startGameInterface();
    }
  }
  if (o.frames) Logf(LogLevel::Info, "moho64ui: ran %d frames", done);
  for (const auto& line : session.Status()) Logf(LogLevel::Info, "moho64ui: session: %s", line.c_str());
  if (dumpUi) shown->ui().Dump();
  if (!screenshot.empty()) {  // draw the head once and save the picture
    ui::SoftwareTarget target(width, height);
    if (backdrop && !game) {  // the background's first picture takes a moment: ask for it, then wait
      shown->ui().Render(shown->L(), target, 0);
      backdrop->WaitUntilReady(30.0);
      target.Clear(0xff000000u);
    }
    shown->ui().Render(shown->L(), target, 0);
    if (!game) session.DrawStatus(target);
    {  // the mouse pointer where the mouse is (a live window leaves it to the system)
      std::string texture;
      int hotX = 0, hotY = 0;
      bool hidden = false;
      if (ui::CurrentCursor(shown->L(), &texture, &hotX, &hotY, &hidden) && !hidden && !texture.empty()) {
        const ui::Image& image = ui::LoadTexture(&vfs, texture);
        if (image.valid()) {
          float x = shown->ui().mouseX - static_cast<float>(hotX), y = shown->ui().mouseY - static_cast<float>(hotY);
          target.DrawImage(image, {0, 0, static_cast<float>(image.width), static_cast<float>(image.height)},
                           {x, y, x + static_cast<float>(image.width), y + static_cast<float>(image.height)}, 0xffffffffu);
        }
      }
    }
    if (target.SavePng(screenshot)) Logf(LogLevel::Info, "moho64ui: screenshot %s", screenshot.c_str());
    else Logf(LogLevel::Error, "moho64ui: cannot write %s", screenshot.c_str());
  }
  ui::ReportStubCalls();
  if (o.listBindings) ui::ListBindings();
  return ok ? 0 : 1;
}
