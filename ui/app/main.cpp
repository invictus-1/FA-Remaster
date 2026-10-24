// moho64ui -- the interface track's host: mount the game data the way the original does, boot the
// user Lua state and enter the game's interface scripts. Without --window it runs frames headless
// on its own clock: the log says how far the scripts came and which engine functions they missed,
// and the screen can be logged (--dump-ui) or saved as a picture (--screenshot). With --window the
// interface runs live in a window, with the mouse.
//
//   moho64ui --init <init.lua> [--mods uids.txt] [--prefs <file>] [--import-prefs <file>|none]
//            [--stage userinit|setup|frontend]
//            [--size 1920x1080] [--window] [--frames N] [--dump-ui] [--screenshot file.png]
//            [--lua <file>] [--list-bindings] [--log out.log] [--quiet] [-- <the game's own arguments>]
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace moho::ui {
extern const char kBuildCommit[];  // (build_ui.py: the commit the program was built from)
}

namespace {

struct Options {
  std::string init;
  std::string modsFile;
  std::string logFile;
  std::string prefsFile;
  // --import-prefs <file>: the original game's preferences to take over when ours do not exist
  // yet (a test's made-up file, or another place of the original's); "none": nothing is taken
  // over. Unset: the original's own file, for our default file only (see DefaultPrefsFile).
  std::string importPrefs;
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
// <prefix>_game.png 300 frames into a game, <prefix>_menu.png 300 frames after it is left,
// <prefix>_loading.png 90 frames into a launch's loading screen, <prefix>_front.png 300 frames
// into the first front end (MOHO64UI_CAPTURE_FRAME: the game's and the first front end's frame).
std::string windowCapture;
int capturedFrames = 0, menuFrames = 0, loadingFrames = 0, frontFrames = 0;
bool gameSeen = false;
bool fullScreen = false;          // --fullscreen: the live window fills the main screen, without a frame
bool modeGiven = false;           // --fullscreen or --size: the command line chose the display mode (DisplayMode)
int wantBackdrop = -1;            // --backdrop / --no-backdrop: the menu's moving background (by itself: in a live window)
// --day-weather (a game starts at nine in the morning) / --day <hour> / --day-length <game seconds,
// 0: the hour stands still> / --weather <kind> (changing, cold, dry, none, clear, cloudy, overcast,
// rain, storm, fog, snow) / --no-weather: the day and the weather in the picture of a game whose
// configuration does not say (a game from the lobby says: its options RemasterDayStart,
// RemasterDayLength, RemasterWeather). Without them such a game has the map's own fixed light.
// --map-edge frame|beyond: likewise the map's edge (RemasterMapEdge); without it, "beyond".
ui::GameSession::Sky sky;
// --maps <folder> (may be given more than once): more map folders, beside the game's own at /maps
// (maps in the making). --remasters <folder> (may be given more than once): the remastered units' own files, one
// folder per unit (render/remasters.h), each root mounted in turn: the stock units' (visuals/work) and, by the
// launchers of games with a mod, that mod's own (visuals/work_mods/<mod>: Chris, 2026-10-20, item 22: no mod unit
// in what ships). --remasters-faf <folder> (may be given more than once): the same for FAF's own units (DELK002,
// the ZEB95xx / ZEB96xx support factories: visuals/work_mods/faf_base), mounted only when the game's files are
// FAF's (Chris, 2026-10-21: "only appear when its connected to faf"). --own <folder>: this program's own files
// (ui/data); without it they are looked for beside the program.
std::vector<std::string> moreMaps, remastersFolders, fafRemastersFolders;
std::string ownFolder;
bool noWorld = false;             // --no-world: a launched game gets no picture of the world (no graphics device is opened)
std::string sessionIn;            // --session file: a recorded game configuration to start a game from at once
std::string gameLuaFile;          // --game-lua file: a host Lua file run in a launched game's state once its interface is up
long long session_seed = -1;      // --seed N: the random seed of a launched game (default: from the clock)
int scanTicks = 0;              // --scan-ticks N: the replay timeline's scan stops at game tick N (tests; 0: the whole replay)
uint32_t fastForward = 0;         // --fast-forward N: a game runs ahead to beat N as fast as it goes (for measurements)
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
  return "C:/Users/Player/" + name + "/";
#else
  // The same folders' places elsewhere: the documents in the home folder; the program data in the
  // user's data folder (Linux: XDG_DATA_HOME, else ~/.local/share; Mac: ~/Library/Application Support).
  const char* home = std::getenv("HOME");
  const std::string h = home && *home ? home : ".";
  if (name == "PERSONAL" || name == "COMMON_DOCUMENTS") return h + "/Documents/";
#ifdef __APPLE__
  return h + "/Library/Application Support/";
#else
  const char* data = std::getenv("XDG_DATA_HOME");
  return (data && *data ? std::string(data) : h + "/.local/share") + "/";
#endif
#endif
}

int l_SHGetFolderPath(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  auto it = g_opts->folders.find(name);
  std::string v = it != g_opts->folders.end() ? it->second : SystemFolder(name);
  if (!v.empty() && v.back() != '/' && v.back() != '\\') v += '\\';
  lua_pushstring(L, v.c_str());
  return 1;
}

// A path below one of the system's folders (the --folder answers first, as SHGetFolderPath's).
std::string BelowFolder(const std::string& name, const std::string& below) {
  auto it = g_opts->folders.find(name);
  std::string dir = it != g_opts->folders.end() ? it->second : SystemFolder(name);
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
  return hostfs::CleanPath(dir + below);
}

// The original game's preferences file. READ (FA exe): its start-up (CScApp, 0x8cefb3) takes
// "/prefs <name>" from the command line, else "Game.prefs" (0x8cefd6), and hands it to the
// preferences' loader 0x8c89d0, which asks SHGetFolderPathW for CSIDL_LOCAL_APPDATA (0x1c, at
// 0x8c8a3e), appends "\Gas Powered Games" and "\Supreme Commander Forged Alliance" (the company and
// product names set at 0x8ce10f / 0x8ce13b), making each folder (CreateDirectoryW), then "\" and the
// name (a plain append, 0x8c8d49: /prefs names a file in that folder). A file that does not load,
// or whose version.major is not 1 (0x8c8e63), is put aside as ".bad" (the message at 0x8c8f0b;
// INFERRED: the renaming, 0x8c8700); without a good one, "../Installed.Prefs" (0x8c8f7e).
// FAF's client starts the same exe without /prefs (READ: its launch line in client.log) and names
// this same file as its preferencesFile (READ: client.prefs), so FAF's players have it here too.
std::string OriginalPrefsFile() {
  return BelowFolder("LOCAL_APPDATA", "Gas Powered Games/Supreme Commander Forged Alliance/Game.prefs");
}

// Ours when --prefs names none (a live window): in a folder of our own beside the original's, so
// the original's file is never touched. The player's replays and saved games go beside it.
std::string DefaultPrefsFile() { return BelowFolder("LOCAL_APPDATA", "FA Remastered/Game.prefs"); }

// Run the init file in its own state and mount what it lists in `path`.
bool RunInit(const Options& o, Vfs& vfs, std::vector<std::string>& hookDirs, std::vector<std::string>& protocols) {
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
  lua_getglobal(L, "protocols");  // (the schemes OpenURL may open: http, https, mailto, ... in FAF's)
  if (lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      protocols.push_back(lua_tostring(L, -1));
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
    else if (a == "--import-prefs") o.importPrefs = next();
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
    else if (a == "--fullscreen") fullScreen = windowed = modeGiven = true;
    else if (a == "--backdrop") wantBackdrop = 1;
    else if (a == "--no-backdrop") wantBackdrop = 0;
    else if (a == "--day-weather") sky.startHour = 9;
    else if (a == "--day") sky.startHour = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--day-length") sky.dayLengthSeconds = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--weather") sky.weather = next();
    else if (a == "--no-weather") sky.weather = "none";
    else if (a == "--map-edge") sky.edge = next();
    else if (a == "--maps") moreMaps.push_back(next());
    else if (a == "--remasters") remastersFolders.push_back(next());
    else if (a == "--remasters-faf") fafRemastersFolders.push_back(next());
    else if (a == "--own") ownFolder = next();
    else if (a == "--seed") session_seed = static_cast<long long>(std::strtoull(next().c_str(), nullptr, 10));
    else if (a == "--scan-ticks") scanTicks = std::atoi(next().c_str());
    else if (a == "--fast-forward") fastForward = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--replay-out") replayOut = next();
    else if (a == "--size") {
      modeGiven = true;
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
  ui::AllowTestFileWrites(!o.luaFile.empty() || !gameLuaFile.empty());  // (test runs only: user_state.h)
  InstallCrashHandler();  // (a fatal fault writes its place and frames to the log: src/core/crash.h)
  if (o.quiet) LogSetEcho(false);
  if (o.init.empty() || width < 1 || height < 1 ||
      (o.stage != "userinit" && o.stage != "setup" && o.stage != "frontend")) {
    std::fprintf(stderr,
                 "usage: moho64ui --init <init.lua> [--mods uids.txt] [--prefs file] [--import-prefs file|none]\n"
                 "                [--stage userinit|setup|frontend]\n"
                 "                [--size 1920x1080] [--window] [--frames N] [--dump-ui] [--screenshot file.png]\n"
                 "                [--lua file] [--list-bindings] [--log out.log] [--quiet] [--no-sound] [--fullscreen] [-- game arguments]\n");
    return 2;
  }
  if (fullScreen && !ui::Window::ScreenSize(&width, &height)) fullScreen = false;  // the head is the screen
  Vfs vfs;
  ui::UserState::Config cfg;
  cfg.width = width;
  cfg.height = height;
  // The game's /preferences/Game.prefs (the init file mounts the original's folder, LOCAL_APPDATA/Gas Powered
  // Games/Supreme Commander Forged Alliance): one of this program's own, held in memory, is mounted there first,
  // so it is the one the game's scripts find (the first mount wins) and the original's file is never read. In
  // the original that file IS the program's preferences file; FAF's Blueprints.lua reads only its PreGameData
  // from it (CurrentMapDir: the map folder whose own blueprints load with the rules), which the host sets at
  // each launch (PreGameDataText). The engine's shared piece (sim/session.h kGamePrefsPath; ui REQUESTS 40).
  if (!vfs.MountMemoryFile(kGamePrefsPath, "PreGameData = {\n}\n"))
    Logf(LogLevel::Warning, "moho64ui: the game's own %s could not be mounted", kGamePrefsPath);
  if (!RunInit(o, vfs, cfg.hookDirs, cfg.protocols)) return 1;
  for (const std::string& folder : moreMaps) {
    if (!vfs.Mount(folder, "/maps")) Logf(LogLevel::Warning, "moho64ui: --maps %s: no such folder", folder.c_str());
  }
  // This program's own files: what it adds to the game's scripts (ui/data/hook, taken as one more
  // hook folder, so the game's files stay as they are). Beside the program, or in its source tree.
  if (ownFolder.empty()) {
    std::string dir = hostfs::ProgramFolder();  // (the engine's: every system)
    if (dir.empty()) dir = ".";
    for (const std::string& candidate : {dir + "/data", dir + "/../ui/data"}) {
      if (ownFolder.empty() && hostfs::IsDirectory(candidate + "/hook")) ownFolder = candidate;
    }
  }
  // (The simulation and the interface's rules get the hook folders without this program's own: the sim's
  // SetupSession imports /lua/ui/lobby/lobbyoptions.lua (simInit.lua 132), and our hook there made ~30 objects in
  // the sim's state that a game's playback without this program does not make, shifting its object serials and
  // with them its pairs() order (engine, 2026-10-21).)
  const std::vector<std::string> gameHookDirs = cfg.hookDirs;
  if (!ownFolder.empty() && vfs.Mount(ownFolder, "/moho64ui")) cfg.hookDirs.push_back("/moho64ui/hook");
  else Logf(LogLevel::Warning, "moho64ui: this program's own files were not found (--own <folder>): the lobby has no day and weather options");
#ifdef MOHO64UI_WORLD_PICTURE
  for (const std::string& folder : remastersFolders)
    Logf(LogLevel::Info, "moho64ui: remastered units: %d from %s", render::MountRemasters(&vfs, folder), hostfs::DisplayPath(folder).c_str());
  // (FAF's game files: they hold /lua/version.lua, FAF's own; the original's lua.scd has none)
  const bool fafData = vfs.Exists("/lua/version.lua");
  for (const std::string& folder : fafRemastersFolders) {
    if (fafData)
      Logf(LogLevel::Info, "moho64ui: remastered FAF units: %d from %s", render::MountRemasters(&vfs, folder), hostfs::DisplayPath(folder).c_str());
    else
      Logf(LogLevel::Info, "moho64ui: the game's files are not FAF's: FAF's units' remasters not mounted (%s)", hostfs::DisplayPath(folder).c_str());
  }
#endif
  Logf(LogLevel::Info, "moho64ui: %zu search paths, %zu files", vfs.MountCount(), vfs.FileCount());
  Logf(LogLevel::Info, "moho64ui: built from commit %s", ui::kBuildCommit);
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
  // Our preferences file: the one --prefs names; without it a live window has one of its own
  // (DefaultPrefsFile), and a run without a window (tests, probes) keeps them in memory only.
  // When ours does not exist yet, the original game's profiles are taken over once (after the
  // front end's state is made, below): from --import-prefs, or for our default file from the
  // original's own (OriginalPrefsFile). The original's file is only ever read.
  std::string importFrom = o.importPrefs == "none" ? std::string() : o.importPrefs;
  if (cfg.prefsFile.empty() && windowed) {
    cfg.prefsFile = DefaultPrefsFile();
    if (o.importPrefs.empty()) importFrom = OriginalPrefsFile();
    const std::string dir = cfg.prefsFile.substr(0, cfg.prefsFile.find_last_of('/'));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
  }
  if (!cfg.prefsFile.empty() && hostfs::DisplayPath(cfg.prefsFile) == hostfs::DisplayPath(OriginalPrefsFile())) {
    Logf(LogLevel::Warning, "moho64ui: --prefs names the original game's own file: not used (it is never written); preferences in memory only");
    cfg.prefsFile.clear();
  }
  const bool firstStart = cfg.prefsFile.empty() || !hostfs::IsFile(cfg.prefsFile);
  // A recorded game needs its launch table kept too: a game launched from the lobby writes it
  // beside the record unless a place of its own was named.
  if (sessionFile.empty() && !replayOut.empty()) sessionFile = replayOut + ".session.lua";
  cfg.sessionFile = sessionFile;

  // The game's sounds, for the front end's state and a game's alike. Only a live window plays
  // them aloud; a run without a window mixes them when a test asks.
  ui::sound::SoundSystem sounds(&vfs);
  cfg.sound = &sounds;
  if (windowed && !noSound && !sounds.OpenDevice()) Logf(LogLevel::Warning, "moho64ui: no sound device: the run is silent");
  // A live window never waits for the disk on its frame: a wave's bytes are brought in on the sound
  // system's own thread; a run without a window (the tests) brings them in there and then, so that a
  // cue sounds at once (MOHO64UI_SOUND_TOUCH=thread / here: either way for any run).
  {
    const char* touch = std::getenv("MOHO64UI_SOUND_TOUCH");
    sounds.SetTouchOnThread(touch ? std::strcmp(touch, "here") != 0 : windowed);
  }

  // A game launched from the lobby: the simulation starts from the game configuration and the host
  // runs its beats beside the interface's frames.
  ui::GameSession session(&vfs, gameHookDirs);
  session.sounds = &sounds;
  if (session_seed >= 0) session.seedGiven = true, session.seed = static_cast<uint32_t>(session_seed);
  session.fastForwardTo = fastForward;
  session.wantPicture = !noWorld;
  session.waitForWindow = windowed && directWindow;  // (a launched table's picture opens on the window)
  session.sky = sky;
  session.replayOut = replayOut;
  session.replayTable = !sessionIn.empty() ? sessionIn : sessionFile;  // (a lobby game's table is written there)
  session.replayMaps = moreMaps;
  session.scanInit = o.init;  // (the replay timeline's scan mounts the game as this program did)
  session.scanFolders = o.folders;
  session.scanTicks = scanTicks;
  cfg.launchSession = [&session](lua_State* L, int config, std::string* why) { return session.Launch(L, config, why); };
  cfg.launchSinglePlayer = [&session](lua_State* L, int info, std::string* why) { return session.LaunchSinglePlayer(L, info, why); };
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
  cfg.worldLoading = [&session]() { return session.Loading(); };
  cfg.liveClock = windowed;  // (a test run's clock goes in fixed frame steps)
  cfg.cardBackdrop = session.cardBackdrop();  // (the loading screen's film and the "IN TRANSIT" cover, where the window's picture shows)

  // The front end's background: the world's picture of a map behind the main menu, on a thread
  // of its own. In a live window unless told otherwise; not when a game starts at once.
  std::unique_ptr<ui::Backdrop> backdrop;
  if ((wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && sessionIn.empty() && !noWorld) {
    backdrop = std::make_unique<ui::Backdrop>(&vfs, "/maps/SCMP_009/SCMP_009.scmap");
    cfg.backdrop = backdrop.get();
    session.menuBackdrop = backdrop.get();
  }
  session.releaseWorlds = [&backdrop]() {
    if (backdrop) backdrop->Stop();
  };
  // The scene's samples (the player's Anti-Aliasing) are set before any world is made, and
  // afterwards only while none lives: the background begins once they are known.
  // (In a live window shown by the renderer the background is drawn by the window's own picture, the
  // game's way: GameSession::PresentToWindow.)
  auto beginBackdrop = [&backdrop, &session]() {
#ifdef MOHO64UI_WORLD_PICTURE
    // (Not while the window's picture lives: its worlds keep the samples they were made with; it is made anew for
    // other ones when no game's world shows in it, GameSession::EnsureWindowPicture.)
    if (!session.DrawsToWindow()) render::gSceneSamples = ui::Looks().SceneSamples();
#endif
    if (backdrop) backdrop->Begin(ui::Looks().vulkan, ui::Looks().textureDetail, windowed && directWindow);
  };
  // The front end's state. A new one is made when a game is left (the original makes a new state
  // for every change between front end and game, FA exe 0x885de0).
  auto front = std::make_unique<ui::UserState>(&vfs, cfg);
  // The original game's profiles taken over, once: ours did not exist yet. Written at once, so
  // ours exists from now on (and every later state reads it); a file that cannot be taken over
  // leaves a fresh start (the game's profile dialog), and the reason in the log.
  if (firstStart && !importFrom.empty()) {
    std::string why;
    const std::string source = hostfs::DisplayPath(importFrom);
    if (!hostfs::IsFile(importFrom)) {
      Logf(LogLevel::Info, "moho64ui: no preferences of the original game to take over (%s)", source.c_str());
    } else if (const int taken = ui::ImportPreferences(front->L(), importFrom, &why)) {
      Logf(LogLevel::Info, "moho64ui: preferences: %d profile(s) taken over from %s (names, options, key bindings; that file is left as it is)",
           taken, source.c_str());
      ui::SavePreferencesOf(front->L());
    } else {
      Logf(LogLevel::Warning, "moho64ui: the original game's preferences were not taken over (%s: %s): a fresh start", source.c_str(), why.c_str());
    }
  }
  bool ok = front->Boot();
  // The display mode (the Options menu's "Primary Adapter", DisplayMode). The original fills the
  // menu's list at the start (StartupHelpers::SetupPrimaryAdapterSettings, FA exe 0x8d21e0):
  // "Windowed" and the screen's modes, or only "Command Line Override" when its command line chose
  // the mode. Here --fullscreen, or --size with --window, chose it; otherwise the player's option
  // does, as the original's device takes it at the start (CScApp::CreateDevice 0x8d0370). A full
  // screen is the screen's own size, so the list has that one mode (the original's text,
  // "2560x1440(240)"). The second screen's list: "Disabled" only.
  ui::Display().overridden = windowed && modeGiven;
  if (ok) {
    int screenW = 1920, screenH = 1080;
    ui::Window::ScreenSize(&screenW, &screenH);
    const int rate = ui::Window::RefreshRate();
    const std::string native = std::to_string(screenW) + "," + std::to_string(screenH) + "," + std::to_string(rate);
    const std::string nativeText = std::to_string(screenW) + "x" + std::to_string(screenH) + "(" + std::to_string(rate) + ")";
    std::string lists = "local logic = import('/lua/options/optionslogic.lua') ";
    lists += ui::Display().overridden
                 ? "logic.SetCustomData('primary_adapter', {states = {{text = '<LOC _Command_Line_Override>', key = 'overridden'}}}, 'overridden') "
                 : "logic.SetCustomData('primary_adapter', {states = {{text = '<LOC OPTIONS_0070>Windowed', key = 'windowed'}, {text = '" +
                       nativeText + "', key = '" + native + "'}}}, '" + native + "') ";
    lists += "logic.SetCustomData('secondary_adapter', {states = {{text = '<LOC _Disabled>', key = 'disabled'}}}, 'disabled')";
    if (!front->RunString(lists, "=(display modes at start-up)")) Logf(LogLevel::Warning, "moho64ui: the display modes could not be listed");
    // A live window the command line left free: the player's choice (a profile that never chose
    // has the screen's own mode, the list's default: a full screen).
    if (windowed && !modeGiven) {
      front->RunString("local o = import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {} "
                       "ConExecute('SC_PrimaryAdapter ' .. tostring(o.primary_adapter or '" + native + "'))",
                       "=(display mode at start-up)");
      ui::DisplayMode& display = ui::Display();
      fullScreen = display.request != ui::DisplayMode::kWindowed;
      if (fullScreen) {
        width = screenW, height = screenH;
      } else {
        width = (std::min)(display.windowedWidth, screenW), height = (std::min)(display.windowedHeight, screenH);
      }
      display.request = ui::DisplayMode::kNone;
      front->SetHeadSize(width, height);
      cfg.width = width, cfg.height = height;
      Logf(LogLevel::Info, "moho64ui: display mode at the start (the player's option): %s %dx%d", fullScreen ? "full screen" : "a window of", width, height);
    }
  }
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
  // The game's own command line with /map <name> or /scenario <file> (after "--"): that map is
  // started at once, without the front end, as the original's start-up does (FA exe 0x8ce480:
  // /lua/SinglePlayerLaunch.lua's StartCommandLineSession(map, false), 0x8ce2a0). A campaign's
  // operation by name: `-- /map X1CA_001 /diff 2 /faction 1`. When it cannot start, the original
  // shows "Unable to launch %s:\n%s" and ends; here it is logged and the front end comes up.
  std::string commandLineMap;
  for (size_t i = 0; i + 1 < o.gameArgs.size(); ++i) {
    std::string a = o.gameArgs[i];
    for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a == "/map" || a == "/scenario") commandLineMap = o.gameArgs[i + 1];
  }
  if (ok && o.stage == "frontend" && !commandLineMap.empty()) {
    lua_State* L = front->L();
    const int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/SinglePlayerLaunch.lua");
    bool started = lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1);
    if (started) {
      lua_pushstring(L, "StartCommandLineSession");
      lua_gettable(L, -2);
      lua_pushstring(L, commandLineMap.c_str());
      lua_pushboolean(L, 0);
      started = lua_pcall(L, 2, 0, 0) == 0;
    }
    if (!started) {
      Logf(LogLevel::Error, "moho64ui: Unable to launch %s: %s", commandLineMap.c_str(), lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
      commandLineMap.clear();
    }
    lua_settop(L, top);
  }
  // (A game started at once, by --session as by /map, has no front end first: its loading screen
  // would destroy a main menu still opening, whose scripts then touch what is gone.)
  if (ok && o.stage == "frontend" && commandLineMap.empty() && sessionIn.empty()) ok = front->StartFrontEnd();
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
  // A new front end, its main menu up (after a game is left, or when a launch did not load).
  auto freshFrontEnd = [&]() {
    if ((wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && !noWorld) {
      session.menuBackdrop = nullptr;
      backdrop.reset();
      backdrop = std::make_unique<ui::Backdrop>(&vfs, "/maps/SCMP_009/SCMP_009.scmap");
      cfg.backdrop = backdrop.get();
      session.menuBackdrop = backdrop.get();
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
  int loadingDrawn = 0;  // frames the loading screen drew while the world loaded (the window's)
  auto loadingSince = std::chrono::steady_clock::now();
  auto startGameInterface = [&]() {
    if (game) return;
    // A launch loads the original's way (its session states, WLD_Frame 0x88cae0): the
    // frame after the launch call, the game's loading screen in the front end's own state
    // (DoPreload 0x88bee0); then the world loads on the worker thread while the front end goes
    // on drawing it (the faction's movie, the tips). Headless (tests): the load is waited for at
    // once, and the loading screen has exactly one frame (its scripts run once) before the game.
    if (session.LoadDue()) {
      if (backdrop) backdrop->Stop();  // (nothing else draws a world while one loads)
      cfg.width = shown->ui().width(), cfg.height = shown->ui().height();
      ui::SavePreferencesOf(front->L());  // (DoPreload writes the preferences first: the game's state reads them)
      vfs.SetMemoryFile(kGamePrefsPath, ui::PreGameDataText(front->L(), session.LoadingMapFolder()));  // (the rules read it)
      if (!front->StartLoadingScreen()) Logf(LogLevel::Warning, "moho64ui: the loading screen did not come up");
      session.BeginLoad();
      loadingDrawn = 0, loadingSince = std::chrono::steady_clock::now();
      if (!windowed) session.WaitForWorker();
      return;
    }
    session.ServeLoad();
    if (!session.Loading() || !session.LoadDone()) return;
    // (MOHO64UI_LONG_FRAMES: the start's parts, for the log of its long frame)
    auto lap = [t = std::chrono::steady_clock::now()](const char* what) mutable {
      const auto now = std::chrono::steady_clock::now();
      ui::LongFrames().Note(what, std::chrono::duration<double, std::milli>(now - t).count());
      t = now;
    };
    {
      std::string why;
      const bool finished = session.FinishLoad(&why);
      lap("(start: the session's load finished)");
      if (!finished) {  // (the original: "map %s failed. aborting session.", back to the front end)
        Logf(LogLevel::Error, "moho64ui: the game did not load: %s; back to the front end", why.c_str());
        if (!sessionIn.empty() || !commandLineMap.empty()) ok = false;  // (the run's own game: its exit code says so)
        session.End();
        freshFrontEnd();
        return;
      }
      Logf(LogLevel::Info, "moho64ui: the world loaded in %.2f s; the loading screen drew %d frames meanwhile",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - loadingSince).count(), loadingDrawn);
    }
    front->EndLoadingScreen();  // (the game's own state takes over: DoInitializing 0x88c4ad)
    lap("(start: the loading screen gone)");
    session.WaitForWorker();  // (no Lua state is made while a beat runs: the engine's rule)
    if (backdrop) backdrop->Stop();  // the game has its own picture of the world
    lap("(start: the beat waited for, the menu's background stopped)");
    // (A new state is made for the size the head has NOW: the window may have been resized.)
    cfg.width = shown->ui().width(), cfg.height = shown->ui().height();
    ui::UserState::Config gameCfg = cfg;
    gameCfg.bootScript = "/lua/SessionInit.lua";
    gameCfg.modUids = session.modUids();
    gameCfg.launchSession = nullptr;
    gameCfg.launchReplay = nullptr;  // (a replay starts from the front end)
    gameCfg.liveSim = session.sim();
    gameCfg.waitForBeat = [&session]() { session.WaitForWorker(); };
    gameCfg.groundHeight = [&session](float x, float z) { return session.TerrainHeight(x, z); };
    gameCfg.beatRunning = [&session]() { return session.BeatRunning(); };
    gameCfg.gameTime = [&session]() { return session.PictureTick() * 0.1; };
    gameCfg.commandSource = session.commandSource();
    gameCfg.ownArmy = session.OwnArmy();
    gameCfg.world = session.world();
    gameCfg.pace = &session.pace;
    gameCfg.timeline = session.timeline();  // (a replay's timeline; nullptr in a game played)
    game = std::make_unique<ui::UserState>(&vfs, gameCfg);
    bool up = session.LoadInterfaceRules(game->L());
    if (up) ui::InstallUnitCategoryFunctions(game->L());
    lap("(start: the game's state made, its rules)");
    up = up && game->Boot();
    lap("(start: SessionInit.lua)");
    up = up && game->StartMainScript();
    lap("(start: SetupUI)");
    up = up && game->StartGame();
    lap("(start: StartGameUI)");
    // The setup's own sync first (the original's beat 0: before the game's screen; the campaign's
    // Sync.CampaignMode is in it, which the screen's making reads), then beat 1.
    if (up) {
      const std::vector<std::string> setup = session.TakeSetupSync();
      Logf(LogLevel::Debug, "moho64ui: the setup's sync: %zu table(s), before the game's screen", setup.size());
      for (const std::string& sync : setup) game->DeliverSync(sync);
    }
    session.RunFirstBeat();
    lap("(start: the setup's sync, the first beat)");
    Logf(up ? LogLevel::Info : LogLevel::Error, "moho64ui: the in-game interface %s", up ? "is up" : "did not come up");
    shown = game.get();
    if (up) {
      // The simulation is already there when the interface's state comes up, so loading ends at
      // once: the game's screen is made (CreateGameInterface) BEFORE beat 1's sync reaches its
      // scripts, as in the original (DoInitializing 0x88c3f0 runs the setup's "beat 0" at
      // 0x88c507, then StopLoadingDialog and CreateGameInterface; beat 1 and on come after). An
      // operation's opening cinematic comes in beat 1's sync (Sync.NISMode 'on': gamemain.NISMode
      // asks the main world view, which CreateGameInterface makes). The setup's own sync went
      // to the scripts above, before the screen (TakeSetupSync).
      game->GameLoaded();
      lap("(start: CreateGameInterface)");
      for (const std::string& sync : session.TakePendingSync()) game->DeliverSync(sync);
      lap("(start: the first beat's sync)");
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
    if (session.Loading()) return;  // (asked while a launch loads: kept until its game runs)
    int request = ui::TakeSessionRequest();
    if (!request || !session.running()) return;
    session.WaitForWorker();  // (the game's state goes and a new one may come: not while a beat runs)
    cfg.width = shown->ui().width(), cfg.height = shown->ui().height();  // (the head's size now)
    if (game) ui::SavePreferencesOf(game->L());  // (what the game changed: the next front end reads the file)
    shown = front.get();
    game.reset();
    sounds.StopAll();
    if (request == 2) {
      // (The loading screen comes up in a front end's state made anew: the old one stood still
      // through the game, its first loading screen's script threads asleep in it, and at the
      // size the window had then.)
      auto fresh = std::make_unique<ui::UserState>(&vfs, cfg);
      if (fresh->Boot() && fresh->StartMainScript()) front = std::move(fresh), shown = front.get();
      std::string why;
      if (session.Relaunch(&why)) {
        Logf(LogLevel::Info, "moho64ui: the game starts over");
        return;
      }
      Logf(LogLevel::Error, "moho64ui: the game could not be started over: %s", why.c_str());
    }
    session.End();
    Logf(LogLevel::Info, "moho64ui: the game is left: back to the front end");
    freshFrontEnd();
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
        session.OpenWindowPicture();  // (the window's picture, the window still hidden: the menus' map begun behind it)
        session.OpenPictureIfDue();   // (a game loaded before the window: its map begun behind it too)
      };
      hooks.direct = [&]() { return session.DrawsToWindow(); };
      hooks.present = [&](const ui::SoftwareTarget& layer, const ui::IntRect& changed) { session.PresentToWindow(layer, changed); };
      hooks.waitForFrame = [&]() { session.WaitForNextFrame(); };
      // The game's beats on a thread of their own, the window showing the world meanwhile
      // (MOHO64UI_BEATS_HERE=1: on this thread, as before).
      session.beatsOnThread = std::getenv("MOHO64UI_BEATS_HERE") == nullptr;
      // The interface, input and camera run during a beat too (the original's way: its interface
      // reads copies, not the running simulation): what asks the simulation every frame answers
      // from copies taken between beats, the rest waits for the beat (UserState::LiveSim;
      // MOHO64UI_BEAT_LOG=1 names each wait). MOHO64UI_SHOW_ONLY_DURING_BEATS=1: frames during a
      // beat only show the picture, as before.
      if (std::getenv("MOHO64UI_SHOW_ONLY_DURING_BEATS") != nullptr) hooks.busy = [&]() { return session.running() && session.BeatRunning(); };
      hooks.clock = [&](double seconds) { session.AdvanceClock(seconds); };
    }
    hooks.catchingUp = [&]() { return session.CatchingUp(); };
    hooks.statsLines = [&](std::vector<std::string>* lines) { session.StatsLines(lines); };
    done = ui::RunWindow([&]() -> ui::UserState& { return *shown; }, width, height, o.frames,
                         [&](double seconds, ui::SoftwareTarget& target) {
                           // (MOHO64UI_LONG_FRAMES: the host step's parts, for the log of a long frame)
                           ui::LongFrameLog& longFrames = ui::LongFrames();
                           auto timed = [&](const char* what, auto&& f) {
                             if (longFrames.thresholdMs <= 0) return f();
                             const auto t0 = std::chrono::steady_clock::now();
                             f();
                             longFrames.Note(what, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                           };
                           session.InterfaceRendered();  // (what the interface's drawing handed to the card is this frame's)
                           timed("(host: the picture opened)", [&] { session.OpenPictureIfDue(); });
                           if (session.Loading()) ++loadingDrawn;
                           timed("(host: the sounds)", [&] {
                             listen();
                             sounds.Update(static_cast<float>(seconds));
                           });
                           if (!game && !session.Loading()) session.WaitForWorker(), session.DrawStatus(target);  // the host's notice, until the game's interface is there
                           timed("(host: leave or restart)", [&] { leaveOrRestart(); });
                           timed("(host: the loading screen or the game's interface started)", [&] { startGameInterface(); });
                           if (!windowCapture.empty() && session.DrawsToWindow()) {
                             // (MOHO64UI_CAPTURE_FRAME: the game's frame it is taken at, 300 unless given; the
                             // first front end's too, before any game)
                             static const int captureAt = std::getenv("MOHO64UI_CAPTURE_FRAME") ? std::atoi(std::getenv("MOHO64UI_CAPTURE_FRAME")) : 300;
                             if (!game && !gameSeen && !session.Loading() && ++frontFrames == captureAt) session.CaptureWindow(windowCapture + "_front.png");
                             if (game && ++capturedFrames == captureAt) session.CaptureWindow(windowCapture + "_game.png"), gameSeen = true;
                             if (!game && gameSeen && ++menuFrames == 300) session.CaptureWindow(windowCapture + "_menu.png");
                             if (!game && session.Loading() && ++loadingFrames == 90) session.CaptureWindow(windowCapture + "_loading.png");
                           }
                           // LAST: a beat it starts on its own thread runs beside nothing more of
                           // this frame that touches the simulation (only the picture is shown).
                           timed("(host: the game's beats)", [&] { session.Advance(seconds); });
                         },
                         fullScreen, hooks);
    session.WaitForBeat();  // (a beat still on its thread at the end: done before anything looks at the game again)
    if (done < 0) ok = false;
  } else {
    for (; done < o.frames && !ui::ExitRequested(); ++done) {
      shown->Frame(1.0 / 60.0);
      session.Advance(1.0 / 60.0);
      listen();
      sounds.Update(1.0f / 60.0f);
      leaveOrRestart();
      startGameInterface();
    }
  }
  if (o.frames) Logf(LogLevel::Info, "moho64ui: ran %d frames", done);
  ui::SavePreferencesOf(shown->L());  // (the program closes: CScApp::Destroy writes the preferences)
  for (const auto& line : session.Status()) Logf(LogLevel::Info, "moho64ui: session: %s", line.c_str());
  if (dumpUi) shown->ui().Dump();
  if (!screenshot.empty()) {  // draw the head once and save the picture
    ui::SoftwareTarget target(width, height);
    if (backdrop && !game && !backdrop->Direct()) {  // the background's first picture takes a moment: ask for it, then wait
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
