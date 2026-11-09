// moho64ui -- the interface track's host: mount the game data the way the original does, boot the
// user Lua state and enter the game's interface scripts. Without --window it runs frames headless
// on its own clock: the log says how far the scripts came and which engine functions they missed,
// and the screen can be logged (--dump-ui) or saved as a picture (--screenshot). With --window the
// interface runs live in a window, with the mouse.
//
//   moho64ui --init <init.lua> [--mods uids.txt] [--prefs <file>] [--import-prefs <file>|none]
//            [--stage userinit|setup|frontend]
//            [--size 1920x1080] [--window] [--frames N] [--dump-ui] [--screenshot file.png]
//            [--lua <file> [--lua-again]] [--list-bindings] [--log out.log] [--quiet] [-- <the game's own arguments>]
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#endif

#include "app/backdrop.h"
#include "app/game_session.h"
#include "app/menu_flight.h"
#include "app/interface_scripts.h"
#include "app/window.h"
#include "core/crash.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/placement.h"
#include "core/trace.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "maui/texture_info.h"
#ifdef MOHO64UI_WORLD_PICTURE
#include "render/remasters.h"
#include "render/view.h"
#endif
#include "script/mount.h"
#include "script/script_state.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sound/sound_system.h"
#include "user/bindings.h"
#include "user/keymap.h"
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
  // --lua-again: the --lua file runs again in every front end made anew (after a game is left, or a launch that did not
  // load), its main menu up: a test of a second visit to the lobby (ui/tests/lobby_secondvisit.lua). Off by default:
  // the tests that leave a game for the menu (menu_backdrop.lua) would launch again.
  bool luaAgain = false;
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
// into the first front end (MOHO64UI_CAPTURE_FRAME: the game's and the first front end's frame); and the replay
// timeline's first jump back: <prefix>_jump_start.png 30 frames into it, _jump_half.png once half of its run ahead is
// through, _jump_there.png 30 frames after its beat.
std::string windowCapture;
int capturedFrames = 0, menuFrames = 0, loadingFrames = 0, frontFrames = 0, jumpFrames = 0, jumpedFrames = 0;
bool jumpHalf = false;
bool gameSeen = false;
bool fullScreen = false;          // --fullscreen: the live window fills the main screen, without a frame
// --second-window WxH (or MOHO64UI_SECOND_WINDOW=WxH): the second head as a framed window of that size on the primary's
// screen, also beside a windowed primary (tests, a PC with one screen: DisplayMode::secondWindowTest). Ours.
std::string secondWindowArg;
bool modeGiven = false;           // --fullscreen or --size: the command line chose the display mode (DisplayMode)
int wantBackdrop = -1;            // --backdrop / --no-backdrop: the menu's moving background (by itself: in a live window)
// --menu-flight: the main menu's flight's battle (app/menu_flight.h) without a window, no picture (tests); in a live
// window it comes with the background (the option Menu Background).
bool menuFlightHeadless = false;
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
// --remasters-mods <folder> (visuals/work_mods): the mods' remastered units, each mod's folder mounted only for a game
// with that mod on (its MODS.json: GameSession::MountModRemasters).
std::string remastersModsRoot;
std::string ownFolder;
// M28AI in the lobby: our variant (the repository's mods/m28-variant, maudlin27's AI made faster, the same decisions:
// its README) in place of the original v326 (the engine, Chris's item 20: his lobby ran the original, so the AI's
// speed-ups did nothing for him). The original stays mounted, not selectable, so that a replay of a game played with
// it plays with it (a replay names its mods by uid; without the original's files it ran with no M28 at all and left
// the game at beat 10, 2026-10-28); the player's M28 in his active mods becomes the variant (both in
// ui/data/hook/lua/mods.lua). --m28 original: the original, without the variant (for checks). --our-mods <folder>:
// the folder holding m28-variant; without it, beside the program or in its source tree.
bool originalM28 = false;
std::string ourMods;
std::string m28Variant;  // the variant's folder when it is used (else empty)
bool noWorld = false;             // --no-world: a launched game gets no picture of the world (no graphics device is opened)
std::string sessionIn;            // --session file: a recorded game configuration to start a game from at once
std::string gameLuaFile;          // --game-lua file: a host Lua file run in a launched game's state once its interface is up
long long session_seed = -1;      // --seed N: the random seed of a launched game (default: from the clock)
int scanTicks = 0;              // --scan-ticks N: the replay timeline's scan stops at game tick N (tests; 0: the whole replay)
uint32_t fastForward = 0;         // --fast-forward N: a game runs ahead to beat N as fast as it goes (for measurements)
std::string replayOut;            // --replay-out <prefix>: each game's orders, beat by beat (GameSession::replayOut)
std::string sessionFile;          // --session-out file: where a launched game's configuration is written

// The system's folders as this program answers SHGetFolderPath(name): the init file's (through the engine's
// MountFromInitFile, given them as its answers: AnswerSystemFolders) and the preferences' places (BelowFolder).
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

// The folders the init file may ask SHGetFolderPath for (the five names script/mount.cpp knows), each answered
// once here: the --folder answer, else SystemFolder's. They go to the engine's MountFromInitFile as its answers: on
// Windows its own are the same (SHGetFolderPathW), and on Linux and Mac it has none of its own ("C:/Users/Player/
// <name>/", script/mount.cpp), so the init file finds there the folders BelowFolder names.
void AnswerSystemFolders(Options& o) {
  for (const char* name : {"LOCAL_APPDATA", "APPDATA", "PERSONAL", "COMMON_APPDATA", "COMMON_DOCUMENTS"})
    o.folders.emplace(name, SystemFolder(name));  // (a --folder answer stays)
}

// The init file's `protocols` table: the schemes OpenURL may open (http, https, mailto, ... in FAF's). READ (FA exe):
// the original takes it from the same run of the init file as `path` and `hook`
// (StartupHelpers::DISK_SetupDataAndSearchPaths 0x459de0: the file run at 0x45a05e, "path" read at 0x45a0e7, "hook"
// at 0x45a3a8, "protocols" at 0x45a45f, each of its values added to a set at 0x10a7bf0 by 0x45b670). The engine's
// MountFromInitFile (script/mount.h), which mounts for us, does not hand that table out (ui/REQUESTS.md, engine 52):
// until it does, the file runs once more here, in a state of its own over an empty file system, so nothing is
// mounted, with its LOG / WARN / SPEW / print lines kept out of the log (the first run wrote them; the engine's own
// debug notes on the file writes it blocks, FAF's shader cache clean-up, come twice). About 60-90 ms (2026-11-01,
// the lab's init file; the mounting itself 5.5-7 s).
std::vector<std::string> InitProtocols(const std::string& initFile) {
  std::vector<std::string> protocols;
  Vfs nothing;
  ScriptState init(ScriptState::Kind::Init, &nothing);
  init.onPrint = [](const std::vector<std::string>&) { return true; };
  lua_State* L = init.L();
  for (const char* quiet : {"LOG", "WARN", "SPEW", "_ALERT"}) {
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
    lua_setglobal(L, quiet);
  }
  std::string dir = initFile;
  size_t s = dir.find_last_of("/\\");
  dir = s == std::string::npos ? "." : dir.substr(0, s);
  lua_pushstring(L, dir.c_str());
  lua_setglobal(L, "InitFileDir");
  lua_pushcfunction(L, l_SHGetFolderPath);
  lua_setglobal(L, "SHGetFolderPath");
  auto src = hostfs::ReadFile(initFile);
  if (!src || !init.DoString(*src, "@" + hostfs::DisplayPath(initFile))) return protocols;
  lua_getglobal(L, "protocols");
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
  return protocols;
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
    else if (a == "--lua-again") o.luaAgain = true;
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
    else if (a == "--second-window") secondWindowArg = next();
    else if (a == "--backdrop") wantBackdrop = 1;
    else if (a == "--no-backdrop") wantBackdrop = 0;
    else if (a == "--menu-flight") menuFlightHeadless = true;
    else if (a == "--day-weather") sky.startHour = 9;
    else if (a == "--day") sky.startHour = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--day-length") sky.dayLengthSeconds = static_cast<float>(std::atof(next().c_str()));
    else if (a == "--weather") sky.weather = next();
    else if (a == "--no-weather") sky.weather = "none";
    else if (a == "--map-edge") sky.edge = next();
    else if (a == "--maps") moreMaps.push_back(next());
    else if (a == "--remasters") remastersFolders.push_back(next());
    else if (a == "--remasters-faf") fafRemastersFolders.push_back(next());
    else if (a == "--remasters-mods") remastersModsRoot = next();
    else if (a == "--own") ownFolder = next();
    else if (a == "--our-mods") ourMods = next();
    else if (a == "--m28") originalM28 = next() == "original";
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
                 "                [--lua file [--lua-again]] [--list-bindings] [--log out.log] [--quiet] [--no-sound] [--fullscreen] [-- game arguments]\n");
    return 2;
  }
  if (fullScreen && !ui::Window::ScreenSize(&width, &height)) fullScreen = false;  // the head is the screen
  AnswerSystemFolders(o);
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
  // Our mods' folders: --our-mods, else beside the program or in its source tree.
  std::vector<std::string> modCandidates;
  if (!ourMods.empty()) modCandidates.push_back(ourMods);
  {
    std::string dir = hostfs::ProgramFolder();
    if (dir.empty()) dir = ".";
    modCandidates.push_back(dir + "/mods");
    modCandidates.push_back(dir + "/../mods");
  }
  if (!originalM28) {  // (see m28Variant)
    for (const std::string& c : modCandidates)
      if (m28Variant.empty() && hostfs::IsFile(c + "/m28-variant/mod_info.lua")) m28Variant = c + "/m28-variant";
  }
  // Our fixes of other mods (fa-remaster/mods/<name>, each holding none of the mod's files: Chris's, e.g. Ultra
  // Shields' ESD Pulse): always mounted at /mods/<name>, so a game played with one and its replay find it; the
  // lobby's mods list adds a fix when its mod is on (ui/data/hook/lua/mods.lua, the same list by uid).
  // Chris's own reworks of mod units hold files made from the mods, so they never go in the repository: they are looked
  // for in the lab's local_mod_fixes too (MOHO64UI_LOCAL_MOD_FIXES names another place); where that folder or a fix is
  // missing (another PC, a public build) the fix is simply not there. A name in the list below is mounted once the
  // engine says that fix is final (its row in mods.lua's table goes in with it).
  std::vector<std::string> fixCandidates = modCandidates;
  {
    if (const char* local = std::getenv("MOHO64UI_LOCAL_MOD_FIXES")) fixCandidates.push_back(local);
    std::string dir = hostfs::ProgramFolder();
    if (dir.empty()) dir = ".";
    fixCandidates.push_back(dir + "/../../local_mod_fixes");  // (fa-remaster-ui/build-ui -> the lab)
    fixCandidates.push_back(dir + "/../local_mod_fixes");
  }
  std::vector<std::pair<std::string, std::string>> modFixes;  // (folder, mount point)
  for (const char* name : {"ultra-shields-fix", "antares-rework", "wyvern-rework", "totalmayhem-rework", "experimental-sacus-fix",
                           "schlachtfeld-fix"}) {
    for (const std::string& c : fixCandidates) {
      if (!hostfs::IsFile(c + "/" + name + "/mod_info.lua")) continue;
      modFixes.push_back({c + "/" + name, std::string("/mods/") + name});
      break;
    }
  }
  // The init file's `path` mounted and its `hook` taken by the engine's function, as its host does (script/mount.h;
  // ui/REQUESTS.md, engine 1); its `protocols` read by ours (InitProtocols).
  if (!MountFromInitFile(o.init, o.folders, vfs, cfg.hookDirs)) return 1;
  cfg.protocols = InitProtocols(o.init);
  for (const auto& [folder, at] : modFixes) {
    if (vfs.Mount(folder, at)) Logf(LogLevel::Info, "moho64ui: our mod fix %s from %s", at.c_str(), hostfs::DisplayPath(folder).c_str());
    else Logf(LogLevel::Warning, "moho64ui: %s could not be mounted", folder.c_str());
  }
  if (!m28Variant.empty()) {
    if (vfs.Mount(m28Variant, "/mods/m28-variant"))
      Logf(LogLevel::Info, "moho64ui: M28AI is our variant: %s", hostfs::DisplayPath(m28Variant).c_str());
    else
      Logf(LogLevel::Warning, "moho64ui: %s could not be mounted", m28Variant.c_str());
  }
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
  // The interface's texture sizes taken ahead, on a thread of their own (maui/texture_info.h; ours, only about time):
  // waited for before a launch changes the game's preferences file in the file system (startGameInterface), and at
  // the end.
  struct JoinedThread {
    std::thread thread;
    void Join() {
      if (thread.joinable()) thread.join();
    }
    ~JoinedThread() { Join(); }
  } texturePrefetch;
  texturePrefetch.thread = std::thread([&vfs] { ui::PrefetchTextureSizes(&vfs); });
  // The skirmish lobby's faction paintings decoded ahead on another (FAF's lobby.lua RefreshLobbyBackground: one of
  // them over the whole lobby with the profile's LobbyBackground at 1, its default; the player's faction's first, the
  // others when he changes it), 8 MB each (draw.h DecodeTexturesAhead; ours, only about time). Joined where the sizes'
  // is.
  JoinedThread paintingsAhead;
  paintingsAhead.thread = std::thread([&vfs] {
    ui::DecodeTexturesAhead(&vfs, vfs.FindFiles("/textures/ui/common/BACKGROUND/faction", "faction-background-paint_*_bmp.dds"));
  });
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
  // (the original's key bindings on the player's ask, moho64ui_OriginalKeyBindings: from --import-prefs when it names
  // a file, the tests' made-up one, else the original's own)
  cfg.originalPrefsFile = !o.importPrefs.empty() && o.importPrefs != "none" ? o.importPrefs : OriginalPrefsFile();
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
  const bool tableBesideRecord = sessionFile.empty() && !replayOut.empty();
  if (tableBesideRecord) sessionFile = replayOut + ".session.lua";
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
  // (the game's time at the PC's clock: a window, or MOHO64UI_REAL_TIME below; then a local game measures its maxSP)
  session.paceByClock = windowed || std::getenv("MOHO64UI_REAL_TIME") != nullptr;
  // A game in a window: its simulation on all the machine's cores (the engine, Chris's item 20: his snapshot games ran
  // one thread, as MOHO64_THREADS was unset; the result is the same beat for beat). MOHO64_THREADS, when set, decides.
  if (windowed && !std::getenv("MOHO64_THREADS")) session.simThreads = static_cast<int>(std::thread::hardware_concurrency());  // (0 if unknown: as unset)
  session.sky = sky;
  session.replayOut = replayOut;
  session.recordFingerprint = !windowed || std::getenv("MOHO64UI_RECORD_FINGERPRINT") != nullptr;
  session.replayTable = !sessionIn.empty() ? sessionIn : sessionFile;  // (a lobby game's table is written there)
  session.numberTables = tableBesideRecord && sessionIn.empty();  // (the second game's beside its own record)
  if (session.numberTables) cfg.sessionFileNow = [&session] { return session.TableFile(); };
  session.replayMaps = moreMaps;
  session.scanInit = o.init;  // (the replay timeline's scan mounts the game as this program did)
  session.scanFolders = o.folders;
  if (!m28Variant.empty()) session.scanMounts.push_back({m28Variant, "/mods/m28-variant"});
  session.remastersModsRoot = remastersModsRoot;
  for (const auto& [folder, at] : modFixes) session.scanMounts.push_back({folder, at});
  session.scanTicks = scanTicks;
  cfg.launchSession = [&session](lua_State* L, int config, std::string* why) { return session.Launch(L, config, why); };
  cfg.launchNetworkSession = [&session](lua_State* L, int config, net::LobbyCore* lobby, std::string* why) {
    return session.LaunchNetwork(L, config, lobby, why);
  };
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
  // The main menu's flight (app/menu_flight.h): with the background in a live window shown by the renderer (its Flight
  // mode, the option Menu Background), or without a window for tests (--menu-flight: its battle alone, no picture).
  std::unique_ptr<ui::MenuFlight> menuFlight;
  const bool backdropWanted = (wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && sessionIn.empty() && !noWorld;
  if ((backdropWanted && windowed && directWindow) || (menuFlightHeadless && !windowed)) {
    menuFlight = std::make_unique<ui::MenuFlight>(&vfs, gameHookDirs);
    session.menuFlight = menuFlight.get();
    cfg.menuFlightInfo = [&menuFlight, &backdrop, &session](lua_State* L) {
      menuFlight->PushInfo(L);
      session.PushWindowInfo(L);  // (the window's picture: the hand-over to a game, the kept first frame, the world's switches)
      lua_pushstring(L, "mode");
      lua_pushstring(L, backdrop ? ui::Backdrop::ModeName(backdrop->mode()) : "none");
      lua_rawset(L, -3);
    };
  }
  // (its mode as the player's option has it, once known: GameSession::FollowMenuMode; the flight's until then)
  auto backdropMode = [&menuFlight]() {
    const int option = ui::Looks().menuBackground;
    return !menuFlight || option == 1 ? ui::Backdrop::Mode::Dusk : option == 2 ? ui::Backdrop::Mode::Still : ui::Backdrop::Mode::Flight;
  };
  if (backdropWanted) {
    backdrop = std::make_unique<ui::Backdrop>(&vfs, backdropMode());
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
  // menu's lists at the start (StartupHelpers::SetupPrimaryAdapterSettings, FA exe 0x8d21e0, and
  // SetupSecondaryAdapterSettings 0x8d26d0, after the device is made, 0x8d0e81): "Windowed" and the
  // screens' modes, or only "Command Line Override" when its command line chose the mode. Here
  // --fullscreen, or --size with --window, chose it; otherwise the player's option does, as the
  // original's device takes it at the start (CScApp::CreateDevice 0x8d0370). A full screen is a
  // screen's own size, so each screen has that one mode (the original's text, "2560x1440(240)"),
  // and the window opens on the screen chosen (the original's device took adapter 0 only).
  // MOHO64UI_SCREENS=2560x1440@240,1920x1080@60 (tests without a window): those screens, side by
  // side, in place of the PC's; MOHO64UI_SECOND_HEAD=1: the Secondary Adapter lists the screens.
  // [head2] The Secondary Adapter lists the other screens (on a PC with two or more: keymap.cpp
  // SetupSecondaryAdapterSettings) where the second head's world can be shown: the renderer's picture in the window
  // (built with it, a window, not --gdi), whose second window draws it (the renderer's answer to ui request 48: Direct3D
  // 12 and Vulkan both). Not probed here: the second window is made only once a second head is chosen; should it not
  // open, the log says why and head 1 shows its interface without the world (GameSession::UseSecondWindow). INFERRED rule
  // (ours): the original lists adapter 1's modes whenever the PC has one.
  ui::DisplayMode& display = ui::Display();
  display.overridden = windowed && modeGiven;
  display.secondHeadShown = std::getenv("MOHO64UI_SECOND_HEAD") != nullptr && std::string(std::getenv("MOHO64UI_SECOND_HEAD")) == "1";
#ifdef MOHO64UI_WORLD_PICTURE
  if (windowed && directWindow) display.secondHeadShown = true;
#endif
  display.screens.clear();
  if (const char* fake = std::getenv("MOHO64UI_SCREENS")) {
    int x = 0;
    for (std::string rest = fake; !rest.empty();) {
      const size_t comma = rest.find(',');
      const std::string one = rest.substr(0, comma);
      rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
      ui::DisplayScreen s;
      if (std::sscanf(one.c_str(), "%dx%d@%d", &s.width, &s.height, &s.rate) < 2 || s.width < 1 || s.height < 1) continue;
      s.x = x, x += s.width;
      display.screens.push_back(s);
    }
  } else {
    for (const ui::Window::Screen& w : ui::Window::Screens()) {
      ui::DisplayScreen s;
      s.x = w.x, s.y = w.y, s.width = w.width, s.height = w.height, s.rate = w.rate;
      display.screens.push_back(s);
    }
  }
  if (display.screens.empty()) {  // (no window system to ask: the main screen as ScreenSize has it, else 1920x1080)
    ui::DisplayScreen s;
    s.width = 1920, s.height = 1080;
    ui::Window::ScreenSize(&s.width, &s.height);
    s.rate = ui::Window::RefreshRate();
    display.screens.push_back(s);
  }
  for (size_t i = 0; i < display.screens.size(); ++i) {
    const ui::DisplayScreen& s = display.screens[i];
    Logf(LogLevel::Info, "moho64ui: screen %d: %dx%d at %d,%d, %d pictures a second%s", static_cast<int>(i + 1), s.width, s.height, s.x,
         s.y, s.rate, i == 0 ? " (the main one)" : "");
  }
  if (ok) {
    const ui::DisplayScreen& mainScreen = display.screens[0];
    const std::string native = std::to_string(mainScreen.width) + "," + std::to_string(mainScreen.height) + "," + std::to_string(mainScreen.rate);
    // The player's choices as kept (none: the main screen's mode, the list's default; and "disabled")
    std::string primary = native, secondary = "disabled";
    if (front->RunString("local o = import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {} "
                         "moho64ui_start_adapters = {tostring(o.primary_adapter or ''), tostring(o.secondary_adapter or '')}",
                         "=(display mode at start-up)")) {
      lua_State* L = front->L();
      lua_getglobal(L, "moho64ui_start_adapters");
      if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        if (lua_isstring(L, -1) && *lua_tostring(L, -1)) primary = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        if (lua_isstring(L, -1) && *lua_tostring(L, -1)) secondary = lua_tostring(L, -1);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
      lua_pushnil(L);
      lua_setglobal(L, "moho64ui_start_adapters");
    }
    const bool primaryWindowed = display.overridden ? !fullScreen : primary == "windowed";
    ui::SetupPrimaryAdapterSettings(front->L());
    ui::SetupSecondaryAdapterSettings(front->L(), primaryWindowed);
    // A live window the command line left free: the player's choice (a profile that never chose
    // has the main screen's own mode, the list's default: a full screen there).
    if (windowed && !modeGiven) {
      ui::ExecuteConsoleCommand(front->L(), "SC_PrimaryAdapter " + primary);
      fullScreen = display.request != ui::DisplayMode::kWindowed;
      const ui::DisplayScreen& on = display.screens[static_cast<size_t>(fullScreen ? display.screen : 0)];
      if (fullScreen) {
        width = on.width, height = on.height;
      } else {
        width = (std::min)(display.windowedWidth, on.width), height = (std::min)(display.windowedHeight, on.height);
      }
      display.request = ui::DisplayMode::kNone;
      front->SetHeadSize(width, height);
      cfg.width = width, cfg.height = height;
      Logf(LogLevel::Info, "moho64ui: display mode at the start (the player's option): %s %dx%d%s", fullScreen ? "full screen" : "a window of",
           width, height, fullScreen ? (" on screen " + std::to_string(display.screen + 1)).c_str() : "");
    }
    // The Secondary Adapter: the original opens a second head on that screen, both heads in full screen
    // (CScApp::CreateDevice 0x8d0712: secondary_adapter not "disabled" (0x8d0721..0x8d0755) and primary_adapter not
    // "windowed" (0x8d075a..0x8d0793); the second head added at 0x8d07e8, its mode from secondary_adapter, 0x8d08b3;
    // both full screen, 0x8d082e / 0x8d0838). Decided once here, as the original makes its device once (the option is
    // restart = true): a second head when the PC has 2 screens or more, the Secondary Adapter names one of them other
    // than the primary's, and the primary is full screen; it is that screen's own size (a full screen here never
    // switches the screen's mode). Else one head, and the log says which condition failed (docs/ui-dualscreen.md 3.2).
    // Without a window no second window opens: one head unless MOHO64UI_HEADS=2 (tests: MOHO64UI_HEAD1_SIZE=WxH, else
    // head 0's size). The test window (--second-window / MOHO64UI_SECOND_WINDOW=WxH): head 1 a framed window of that
    // size on the primary's screen, whatever the option and the primary.
    const int second = ui::ScreenOfModeKey(secondary, 1);
    display.secondScreen = -1;
    display.heads = 1;
    display.secondWindowTest = false;
    std::string noSecond;  // (why there is none, when the option asks for one)
    std::string word = secondary;
    for (char& c : word) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (word == "DISABLED" || word == "OVERRIDDEN") {
    } else if (second < 0) {
      noSecond = "the Secondary Adapter names no screen (" + secondary + ")";
    } else if (primaryWindowed) {
      noSecond = "the primary is windowed (the original's rule: a second head only beside a full screen)";
    } else if (display.screens.size() < 2) {
      noSecond = "the PC has one screen";
    } else if (second >= static_cast<int>(display.screens.size())) {
      noSecond = "the Secondary Adapter's screen " + std::to_string(second + 1) + " is not there";
    } else if (second == display.screen) {
      noSecond = "the Secondary Adapter names the primary's own screen";
    } else {
      display.secondScreen = second;
      if (!windowed) noSecond = "the Secondary Adapter names screen " + std::to_string(second + 1) + ", but there is no window";
    }
    auto sizeOf = [](const char* text, int* w, int* h) {
      return text && std::sscanf(text, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
    };
    const char* testWindow = !secondWindowArg.empty() ? secondWindowArg.c_str() : std::getenv("MOHO64UI_SECOND_WINDOW");
    int w1 = 0, h1 = 0;
    if (windowed && testWindow && sizeOf(testWindow, &w1, &h1)) {
      display.heads = 2, display.secondWindowTest = true, display.secondWidth = w1, display.secondHeight = h1;
      Logf(LogLevel::Info, "moho64ui: a second head: a framed window of %dx%d on the primary's screen (the test window)", w1, h1);
    } else if (!windowed && std::getenv("MOHO64UI_HEADS") && std::string(std::getenv("MOHO64UI_HEADS")) == "2") {
      if (!sizeOf(std::getenv("MOHO64UI_HEAD1_SIZE"), &w1, &h1)) w1 = width, h1 = height;
      display.heads = 2, display.secondWidth = w1, display.secondHeight = h1;
      Logf(LogLevel::Info, "moho64ui: a second head of %dx%d without a window (MOHO64UI_HEADS=2)", w1, h1);
    } else if (display.secondScreen >= 0 && windowed) {
      const ui::DisplayScreen& s = display.screens[static_cast<size_t>(display.secondScreen)];
      display.heads = 2, display.secondWidth = s.width, display.secondHeight = s.height;
      Logf(LogLevel::Info,
           "moho64ui: a second head: full screen on screen %d, %dx%d at %d pictures a second (its world view's picture waits "
           "for the renderer's second window: its interface only)",
           display.secondScreen + 1, s.width, s.height, s.rate);
    }
    if (display.heads < 2 && !noSecond.empty()) Logf(LogLevel::Info, "moho64ui: no second head: %s", noSecond.c_str());
    cfg.heads = display.heads, cfg.head1Width = display.secondWidth, cfg.head1Height = display.secondHeight;
    front->SetSecondHead(cfg.heads, cfg.head1Width, cfg.head1Height);
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
  // (the flight's session described in the front end's state, its scenario run there as a launch's is: once; a PC
  // without SETONS_R falls back to Dusk, GameSession::FollowMenuMode)
  if (ok && menuFlight) menuFlight->Describe(front->L());
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
  // The heads' sizes the state shown has now, for the states made next (a window may have been resized).
  auto takeHeadSizes = [&]() {
    cfg.width = shown->ui().width(), cfg.height = shown->ui().height();
    if (cfg.heads >= 2 && shown->ui().width(1) > 0) cfg.head1Width = shown->ui().width(1), cfg.head1Height = shown->ui().height(1);
  };
  // A new front end, its main menu up (after a game is left, or when a launch did not load).
  auto freshFrontEnd = [&]() {
    if ((wantBackdrop == 1 || (wantBackdrop < 0 && windowed)) && !noWorld) {
      session.menuBackdrop = nullptr;
      backdrop.reset();
      backdrop = std::make_unique<ui::Backdrop>(&vfs, backdropMode());
      cfg.backdrop = backdrop.get();
      session.menuBackdrop = backdrop.get();
      beginBackdrop();  // (the game's picture is gone: the samples may follow the option again)
    }
    auto fresh = std::make_unique<ui::UserState>(&vfs, cfg);
    if (fresh->Boot() && fresh->StartMainScript() && fresh->StartFrontEnd()) {
      front = std::move(fresh);
      shown = front.get();
      if (o.luaAgain && !o.luaFile.empty()) {  // (--lua-again: the test's file in this front end too)
        Logf(LogLevel::Info, "moho64ui: --lua-again: %s runs in the new front end", o.luaFile.c_str());
        if (auto code = hostfs::ReadFile(o.luaFile)) front->RunString(*code, "@" + o.luaFile);
        else Logf(LogLevel::Error, "cannot read %s", o.luaFile.c_str());
      }
    } else {
      Logf(LogLevel::Error, "moho64ui: the front end did not come up again");
    }
  };
  int loadingDrawn = 0;  // frames the loading screen drew while the world loaded (the window's)
  auto loadingSince = std::chrono::steady_clock::now();
  // The game's state as the loading thread made it (GameSession::prepareInterface): made, its rules in it and
  // /lua/SessionInit.lua run, as the original's "Map loader" task does them (func_WorldSessionUserLoad 0x885de0: the
  // session's user Lua state 0x885ec9, its functions registered 0x885f37..0x885f8a, the rules exported into it
  // 0x885ff2, SessionInit.lua 0x88603e), so that the frame the load ends in runs only the scripts the original runs on
  // its main thread (DoInitializing 0x88c3f0: StartGameUI 0x88c4ad, the setup's beat 0x88c507, StopLoadingDialog,
  // CreateGameInterface). Written there, read here after the worker's wait (FinishLoad).
  struct Prepared {
    std::unique_ptr<ui::UserState> state;
    bool up = false;
    double stateMs = 0, rulesMs = 0, bootMs = 0, aheadMs = 0, compiledMs = 0, coversMs = 0;  // (its parts' times, for the log)
    int compiled = 0, covers = 0;  // (the game screen's scripts compiled ahead, the covers decoded)
  } prepared;
  // The game's interface is started over two frames or more in the window (ours, only about time;
  // MOHO64UI_START_ONE_FRAME=1: in one, as before): in the first its state is taken, SetupUI, StartGameUI, the setup's sync and the first beat
  // begun (on the beats' thread), the loading screen still shown; in the next the loading screen goes and the game's
  // screen is made (StopLoadingDialog, CreateGameInterface) and beat 1's sync given. The original's order is kept
  // (DoInitializing 0x88c3f0: StartGameUI 0x88c4ad, the setup's beat 0x88c507, StopLoadingDialog 0x88c577,
  // CreateGameInterface 0x88c5cd), with a frame between its beat and StopLoadingDialog: the game's state runs no frame
  // in between (it is not shown: none of its script threads, input or syncs), the beats are held (HoldBeats), and the
  // front end's loading screen draws on, as it drew while the world loaded. The second half comes once the first beat
  // is through (1-2 frames at 4K: its tick and the simulation's collection, 15-25 ms, no longer in a frame). (The
  // loading screen's tips' thread asks WorldIsLoading, false from the load's end: it may end a frame sooner. INFERRED:
  // nothing else of FAF's loading screen or gamemain.lua waits on those frames.) Without a window both halves run in
  // one frame.
  std::unique_ptr<ui::UserState> starting;  // between the two: the game's state, its screen not made yet
  bool startingUp = false;
  bool aheadDue = false;  // (the beats behind the loading screen still to be begun once the first is through)
  bool startUp = false;  // (the game's state came up so far: its boot, SetupUI, StartGameUI)
  std::function<void()> finishGameInterface;
  // A session's state made and booted (the loading thread's work, or this thread's when it did not do it): its rules,
  // the category functions in front of the engine's, then /lua/SessionInit.lua.
  auto makeGameState = [&vfs, &session](ui::UserState::Config gameCfg, Sim* sim, const std::vector<std::string>& uids,
                                        const std::string& sessionMods, Prepared* out) {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = Clock::now();
    gameCfg.modUids = uids;
    gameCfg.liveSim = sim;  // (its bindings are those of a session's state; the rest of the session's links come at the hand-over)
    gameCfg.gameVersion = sim ? sim->gameVersion : 0;  // (GetVersion: the session's own, a replay's as recorded)
    out->state = std::make_unique<ui::UserState>(&vfs, gameCfg);
    const auto t1 = Clock::now();
    bool up = session.LoadInterfaceRules(out->state->L(), sim, uids, sessionMods);
    if (up) ui::InstallUnitCategoryFunctions(out->state->L());
    const auto t2 = Clock::now();
    out->up = up && out->state->Boot();
    out->stateMs = ms(t0, t1), out->rulesMs = ms(t1, t2), out->bootMs = ms(t2, Clock::now());
  };
  // A network game's links for the game's state: SessionIsMultiplayer, and its side of the game (GetSessionClients and
  // so the disconnect dialog, EjectSessionClient, chat, game speed, the waiting and desync dialogs).
  auto linkNetwork = [&session](ui::UserState::Config& gameCfg) {
    gameCfg.multiplayer = session.networkGame();
    gameCfg.net = session.netLink();
  };
  // An Aheadable game's first beats before its screen (GameSession::RunAhead, aheadInLoad): in a window only.
  // (MOHO64UI_AHEAD=0: no beats behind the loading screen; =1: those beats also without a window, for tests)
  auto aheadWanted = [&]() {
    static const char* aheadMode = std::getenv("MOHO64UI_AHEAD");
    return session.Aheadable() && (aheadMode ? std::string(aheadMode) != "0" && (windowed || std::string(aheadMode) == "1") : windowed);
  };
  auto startGameInterface = [&]() {
    if (game) return;
    // A launch loads the original's way (its session states, WLD_Frame 0x88cae0): the
    // frame after the launch call, the game's loading screen in the front end's own state
    // (DoPreload 0x88bee0); then the world loads on the worker thread while the front end goes
    // on drawing it (the faction's movie, the tips). Headless (tests): the load is waited for at
    // once, and the loading screen has exactly one frame (its scripts run once) before the game.
    if (session.LoadDue()) {
      if (backdrop) backdrop->Stop();  // (nothing else draws a world while one loads)
      // (A jump back held, GameSession::Rewind: the same replay again behind the picture held. The front end is the
      // one from before the game, neither stepped nor drawn: no loading screen in it, and its preferences, older than
      // the game's, are not written (the game's were, by rewind); the size is the game's (rewind's cfg); the
      // PreGameData file stays the replay's own.)
      const bool held = session.Rewinding();
      if (!held) {
        takeHeadSizes();
        ui::SavePreferencesOf(front->L());  // (DoPreload writes the preferences first: the game's state reads them)
      }
      texturePrefetch.Join();  // (long done: nothing else reads the file system's list while it changes)
      paintingsAhead.Join();
      if (!held) {
        vfs.SetMemoryFile(kGamePrefsPath, ui::PreGameDataText(front->L(), session.LoadingMapFolder()));  // (the rules read it)
        if (!front->StartLoadingScreen()) Logf(LogLevel::Warning, "moho64ui: the loading screen did not come up");
        session.NoteLoadingScreen();
      }
      // The game's state is made on the loading thread once the simulation has started (its configuration as this
      // thread's now: a copy, nothing of this thread's is touched there). MOHO64UI_STATE_HERE=1: on this thread at the
      // end of the load, as before.
      prepared = Prepared{};
      if (std::getenv("MOHO64UI_STATE_HERE") == nullptr) {
        ui::UserState::Config gameCfg = cfg;
        gameCfg.bootScript = "/lua/SessionInit.lua";
        gameCfg.launchSession = nullptr;
        gameCfg.launchNetworkSession = nullptr;
        gameCfg.launchReplay = nullptr;  // (a replay starts from the front end)
        linkNetwork(gameCfg);
        session.prepareInterface = [&prepared, &vfs, makeGameState, gameCfg](Sim* sim, const std::vector<std::string>& uids,
                                                                             const std::string& sessionMods) {
          makeGameState(gameCfg, sim, uids, sessionMods, &prepared);
          // Also here, ahead of the frame the load ends in (ours, only about time): the game screen's scripts compiled
          // (interface_scripts.h), then the state's collection of what its making left (the rules' copy: a full
          // collection of some 35 MB fell inside that frame, 8-17 ms).
          auto t0 = std::chrono::steady_clock::now();
          if (prepared.state && prepared.up)
            prepared.compiled = prepared.state->PrecompileScripts(ui::kGameInterfaceScripts, std::size(ui::kGameInterfaceScripts));
          prepared.compiledMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          t0 = std::chrono::steady_clock::now();
          if (prepared.state) lua_setgcthreshold(prepared.state->L(), 0);
          prepared.aheadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          // And the factions' "IN TRANSIT" covers decoded (FAF's StopLoadingDialog lays the profile's LoadingFaction's
          // over the screen, gamemain.lua: 4-6 ms in the game's first frame at 4K; draw.h DecodeTexturesAhead).
          t0 = std::chrono::steady_clock::now();
          const std::vector<std::string> covers = vfs.FindFiles("/textures/ui/common", "*_load.dds");
          ui::DecodeTexturesAhead(&vfs, covers);
          prepared.covers = static_cast<int>(covers.size());
          prepared.coversMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
      } else {
        session.prepareInterface = nullptr;
      }
      // Those beats on the loading thread, while the map still opens (GameSession::aheadInLoad; Chris, 2026-11-07: the
      // start held past the load no more). MOHO64UI_AHEAD_IN_LOAD=0: after the load, as before (RunAhead below).
      static const bool aheadAfterLoad = [] {
        const char* v = std::getenv("MOHO64UI_AHEAD_IN_LOAD");
        return v && std::string(v) == "0";
      }();
      session.aheadInLoad = !held && !aheadAfterLoad && aheadWanted();
      session.BeginLoad();
      loadingDrawn = 0, loadingSince = std::chrono::steady_clock::now();
      if (!windowed) session.WaitForWorker();
      return;
    }
    if (startingUp) {  // (the second half once the first beat is through: a frame or two, the loading screen shown)
      if (session.BeatRunning()) return;
      if (aheadDue) {  // (then the beats up to the commander's arrival, still behind the loading screen: GameSession::Aheadable)
        aheadDue = false;
        session.FinishFirstBeat();
        session.RunAhead(ui::GameSession::kAheadTicks);
        return;
      }
      if (session.RunningAhead()) return;
      finishGameInterface();
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
        prepared = Prepared{};  // (made before the session's end: its categories are the session's)
        if (!sessionIn.empty() || !commandLineMap.empty()) ok = false;  // (the run's own game: its exit code says so)
        session.End();
        freshFrontEnd();
        return;
      }
      Logf(LogLevel::Info, "moho64ui: the world loaded in %.2f s; the loading screen drew %d frames meanwhile",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - loadingSince).count(), loadingDrawn);
    }
    session.WaitForWorker();  // (no Lua state is made while a beat runs: the engine's rule)
    if (backdrop) backdrop->Stop();  // the game has its own picture of the world
    lap("(start: the beat waited for, the menu's background stopped)");
    // (A new state is made for the size the head has NOW: the window may have been resized. Held, a jump back: the
    // game's size as it was left; a resize meanwhile reaches the new state when it is first shown, run_window.cpp.)
    if (!session.Rewinding()) takeHeadSizes();
    ui::UserState::Config gameCfg = cfg;
    gameCfg.bootScript = "/lua/SessionInit.lua";
    gameCfg.modUids = session.modUids();
    gameCfg.launchSession = nullptr;
    gameCfg.launchNetworkSession = nullptr;
    gameCfg.launchReplay = nullptr;  // (a replay starts from the front end)
    gameCfg.liveSim = session.sim();
    linkNetwork(gameCfg);
    gameCfg.waitForBeat = [&session]() { session.WaitForWorker(); };
    gameCfg.groundHeight = [&session](float x, float z) { return session.TerrainHeight(x, z); };
    gameCfg.beatRunning = [&session]() { return session.BeatRunning(); };
    gameCfg.syncsOwed = [&session]() { return session.SyncsOwed(); };
    gameCfg.gameTime = [&session]() { return session.PictureTick() * 0.1; };
    gameCfg.commandSource = session.commandSource();
    gameCfg.ownArmy = session.OwnArmy();
    gameCfg.world = session.world();
    gameCfg.pace = &session.pace;
    gameCfg.timeline = session.timeline();  // (a replay's timeline; nullptr in a game played)
    gameCfg.gameMoments = [&session](bool start) { return session.GameMoments(start); };  // (the score screen's)
    if (prepared.state) {  // (made on the loading thread: given the session's links)
      Logf(LogLevel::Info,
           "moho64ui: the game's state was made on the loading thread: the state %.0f ms, its rules %.0f ms, %s %.0f ms (then %d "
           "of the game screen's scripts compiled, %.0f ms; a collection, %.0f ms; %d covers decoded, %.0f ms)",
           prepared.stateMs, prepared.rulesMs, gameCfg.bootScript.c_str(), prepared.bootMs, prepared.compiled, prepared.compiledMs,
           prepared.aheadMs, prepared.covers, prepared.coversMs);
      prepared.state->LinkSession(gameCfg);
      lap("(start: the game's state taken)");
    } else {
      makeGameState(gameCfg, session.sim(), session.modUids(), session.sessionMods(), &prepared);
      prepared.state->LinkSession(gameCfg);
      lap("(start: the game's state made here, its rules, SessionInit.lua)");
    }
    starting = std::move(prepared.state);
    startUp = prepared.up;
    prepared = Prepared{};
    startUp = startUp && starting->StartMainScript();
    lap("(start: SetupUI)");
    startUp = startUp && starting->StartGame();
    lap("(start: StartGameUI)");
    // The setup's own sync first (the original's beat 0: before the game's screen; the campaign's
    // Sync.CampaignMode is in it, which the screen's making reads), then beat 1.
    if (startUp) {
      const std::vector<std::string> setup = session.TakeSetupSync();
      Logf(LogLevel::Debug, "moho64ui: the setup's sync: %zu table(s), before the game's screen", setup.size());
      for (const std::string& sync : setup) starting->DeliverSync(sync);
    }
    static const bool oneFrame = std::getenv("MOHO64UI_START_ONE_FRAME") != nullptr;
    // (not when they ran in the load: the session's tick is theirs already, AheadRanTo)
    const bool ahead = aheadWanted() && !session.AheadRanTo();
    if ((windowed && !oneFrame) || ahead) {  // (the game's screen in the next frame: see `starting')
      session.StartFirstBeat();
      lap("(start: the setup's sync, the first beat begun)");
      session.HoldBeats(true);
      startingUp = true;
      aheadDue = ahead;
      return;
    }
    session.RunFirstBeat();
    lap("(start: the setup's sync, the first beat)");
    finishGameInterface();
  };
  // (the second half of startGameInterface)
  finishGameInterface = [&]() {
    auto lap = [t = std::chrono::steady_clock::now()](const char* what) mutable {
      const auto now = std::chrono::steady_clock::now();
      ui::LongFrames().Note(what, std::chrono::duration<double, std::milli>(now - t).count());
      t = now;
    };
    if (startingUp) {
      session.FinishFirstBeat();
      session.HoldBeats(false);
      startingUp = false;
      lap("(start: the first beat waited for)");
    }
    // The front end's loading screen goes here, just before the game's StopLoadingDialog (0x88c577). Ours, INFERRED:
    // that it stays up until then (through SetupUI, StartGameUI and the setup's beat of the game's own state, and the
    // frames between the two halves); the original runs those in one DoInitializing step, so its one frame shows no
    // difference.
    if (!session.Rewinding()) front->EndLoadingScreen();  // (a jump back held put up none)
    lap("(start: the loading screen gone)");
    game = std::move(starting);
    const bool up = startUp;
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
      // (the first beats ran behind the loading screen: the tick the screen comes at, for gamemain's StartupSequence,
      // ui/data/hook/lua/ui/game/gamemain.lua)
      if (session.AheadRanTo()) game->RunString("moho64ui_aheadTick = " + std::to_string(session.AheadRanTo()), "=host");
      game->GameLoaded();
      lap("(start: CreateGameInterface)");
      game->DropPrecompiledScripts();
      for (const std::string& sync : session.TakePendingSync()) game->DeliverSync(sync);
      lap("(start: the first beat's sync)");
      // (the start's times: how long from the launch call to the game's screen, and how much of it came after the load)
      if (!session.Rewinding())
        Logf(LogLevel::Info, "moho64ui: the game's screen comes %.2f s after the launch call: the load %.2f s, then %.2f s more (at tick %u)",
             session.LaunchToLoadEnd() + session.SinceLoadEnd(), session.LaunchToLoadEnd(), session.SinceLoadEnd(), session.tick());
      if (!gameLuaFile.empty()) {  // a Lua file of your own, run in the game's state (tests, looking around)
        if (auto code = hostfs::ReadFile(gameLuaFile)) game->RunString(*code, "@" + gameLuaFile);
        else Logf(LogLevel::Error, "cannot read %s", gameLuaFile.c_str());
      }
      // A network game: READY to the others now that the screen is up (the original's DoInitializing ends so,
      // 0x88c664); its beats wait for theirs. (After the test's Lua file, so that a test may stop it first.)
      session.NetReady();
      session.onSync = [&game](const std::string& sync) { game->DeliverSync(sync); };
      session.takeOrders = [&game]() { return ui::TakeOrders(game->L()); };
    }
  };
  // A script has asked to leave the game (ExitGame) or to start it over (RestartSession): done
  // here, between two frames. Leaving: the game's interface and the session go, and a new front
  // end comes up with its main menu, as after the program's start. Starting over: the same
  // launch again; its interface comes up as it did the first time.
  //
  // The replay timeline's jump back (GameSession::Rewind; MOHO64UI_REWIND_RELOAD=1: as a restart, below): the frame it
  // is asked in is read back as the picture the window holds; in the next the game's interface goes (its preferences
  // written) and the replay starts over behind that picture, no front end made anew (the one from before the game is
  // only the state shown until the new interface is there, never stepped or drawn: RunWindow's `held`).
  bool rewindDue = false;  // (asked; waits for the picture to be read back)
  // The new interface's time behind the picture held (its frames' seconds). Its opening is FAF's: StopLoadingDialog
  // lays the "IN TRANSIT" cover over the screen, fading it from 1.5 s to 3.5 s of its frames' time, then slides the
  // panels in (InitialAnimations: 3 x 0.15 s; gamemain.lua). At the beat, what is left of those 4 s is run at once
  // behind the picture, in frames of 0.25 s with no beat between them, so that the player comes back to the game as
  // it was, not to a cover fading out (a jump whose run ahead is shorter than that). Ours; INFERRED from those scripts.
  double heldUiSeconds = 0;
  constexpr double kHeldOpeningSeconds = 5.0;
  auto finishRewind = [&]() {
    const double had = heldUiSeconds;
    int frames = 0;
    if (game)
      while (heldUiSeconds < kHeldOpeningSeconds) game->Frame(0.25), heldUiSeconds += 0.25, ++frames;
    Logf(LogLevel::Info, "moho64ui: the jump back: the picture held goes (the new interface had %.2f s of frames behind it, %d more of 0.25 s now)",
         had, frames);
    heldUiSeconds = 0;
    session.EndRewind();
  };
  auto rewind = [&]() {
    session.WaitForWorker();
    takeHeadSizes();  // (the heads' sizes now: the new state's)
    if (game) ui::SavePreferencesOf(game->L());
    shown = front.get();
    game.reset();
    sounds.StopAll();
    std::string why;
    if (session.Rewind(&why)) return;
    Logf(LogLevel::Error, "moho64ui: the replay could not be started over: %s", why.c_str());
    session.End();
    Logf(LogLevel::Info, "moho64ui: the game is left: back to the front end");
    freshFrontEnd();
  };
  auto leaveOrRestart = [&]() {
    if (session.Loading() || startingUp) return;  // (asked while a launch loads: kept until its game runs)
    if (rewindDue) {
      if (!session.running()) {
        rewindDue = false;
      } else if (session.PictureFrozen()) {
        rewindDue = false;
        rewind();
      }
      return;
    }
    int request = ui::TakeSessionRequest();
    if (!request || !session.running()) return;
    if (request == 2 && session.JumpBackDue()) {
      session.FreezePicture();
      if (session.PictureFrozen()) rewind();
      else rewindDue = true;
      return;
    }
    session.WaitForWorker();  // (the game's state goes and a new one may come: not while a beat runs)
    takeHeadSizes();  // (the heads' sizes now)
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
      // [head2] The second head's window: the renderer's second window on the window's picture (GameSession)
      hooks.secondOpened = [&](void* handle, int w, int h) { return session.UseSecondWindow(handle, w, h); };
      hooks.secondOn = [&]() { return session.SecondWindowOn(); };
      hooks.secondLayer = [&](const ui::SoftwareTarget& layer, const ui::IntRect& changed) { session.GiveSecondLayer(layer, changed); };
      hooks.secondDrawn = [&]() { return session.SecondWindowDrawnLastFrame(); };
      hooks.secondCapture = [&](const std::string& path) { return session.CaptureSecondWindow(path); };
      hooks.secondClosing = [&]() { session.CloseSecondWindow(); };
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
    hooks.framePeriod = [&]() { return session.MenuFramePeriod(); };  // (the menu's flight: at most 60 frames a second)
    // (the hold ends at the top of the frame after the one its beat was reached in: that frame still showed the picture)
    hooks.held = [&]() {
      if (session.RewindArrived()) finishRewind();
      return session.Rewinding();
    };
    hooks.statsLines = [&](std::vector<std::string>* lines) { session.StatsLines(lines); };
    hooks.frameGenerationOn = [&]() { return session.FrameGenerationOn(); };
#ifdef _WIN32
    // On trial (the engine, 2026-11-04): the window's frame thread on the big-cache processors, as the sim's beat
    // thread is (core/placement.h). Off unless MOHO64UI_PIN_FRAME=1, until the frame times on Chris's game decide.
    if (const char* pin = std::getenv("MOHO64UI_PIN_FRAME"); pin && std::strcmp(pin, "1") == 0) {
      const uint64_t mask = place::BigCacheMask();
      if (mask && SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask)))
        Logf(LogLevel::Info, "moho64ui: the frame thread on the big-cache processors (%d of them)", place::BigCacheCount());
      else
        Logf(LogLevel::Info, "moho64ui: MOHO64UI_PIN_FRAME: no big-cache processors here; the frame thread stays where it is");
    }
#endif
    done = ui::RunWindow([&]() -> ui::UserState& { return *shown; }, width, height, o.frames,
                         [&](double seconds, ui::SoftwareTarget& target) {
                           // (MOHO64UI_LONG_FRAMES: the host step's parts, for the log of a long frame)
                           ui::LongFrameLog& longFrames = ui::LongFrames();
                           auto timed = [&](const char* what, auto&& f) {
                             MOHO_ZONE("ui: host step");  // (its name as the zone's text)
                             MOHO_ZONE_TEXT(what, std::strlen(what));
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
                           if (session.Rewinding()) {
                             // A jump back held: the new interface's scripts get their frames as before (not drawn, no
                             // input) once it is there, and the host's note goes over the picture held.
                             if (game) timed("(host: the game's interface stepped, held)", [&] { game->Frame(seconds), heldUiSeconds += seconds; });
                             session.DrawHeld(target);
                           } else if (!game && !session.Loading() && !startingUp) {
                             session.WaitForWorker(), session.DrawStatus(target);  // the host's notice, until the game's interface is there
                           }
                           timed("(host: leave or restart)", [&] { leaveOrRestart(); });
                           timed("(host: the loading screen or the game's interface started)", [&] { startGameInterface(); });
                           if (!windowCapture.empty() && session.DrawsToWindow()) {
                             // (MOHO64UI_CAPTURE_FRAME: the game's frame it is taken at, 300 unless given; the
                             // first front end's too, before any game)
                             static const int captureAt = std::getenv("MOHO64UI_CAPTURE_FRAME") ? std::atoi(std::getenv("MOHO64UI_CAPTURE_FRAME")) : 300;
                             if (!game && !gameSeen && !session.Loading() && ++frontFrames == captureAt) session.CaptureWindow(windowCapture + "_front.png");
                             // (the main menu's flight at battle ticks: MOHO64UI_MENUFLIGHT_CAPTURES=600,1500,2050 gives
                             // <prefix>_flight_<tick>.png for each, its first loop)
                             if (menuFlight && !game && !session.Loading()) {
                               static std::vector<int> flightAt = [] {
                                 std::vector<int> at;
                                 const char* list = std::getenv("MOHO64UI_MENUFLIGHT_CAPTURES");
                                 for (const char* c = list; c && *c;) {
                                   at.push_back(std::atoi(c));
                                   c = std::strchr(c, ',');
                                   if (c) ++c;
                                 }
                                 return at;
                               }();
                               for (int& t : flightAt)
                                 if (t > 0 && menuFlight->loops() == 0 && menuFlight->tick() >= static_cast<uint32_t>(t) &&
                                     menuFlight->state() == ui::MenuFlight::State::Running) {
                                   session.CaptureWindow(windowCapture + "_flight_" + std::to_string(t) + ".png");
                                   Logf(LogLevel::Info, "moho64ui: the menu's flight captured at tick %u", menuFlight->tick());
                                   t = 0;
                                   break;
                                 }
                             }
                             if (game && ++capturedFrames == captureAt) session.CaptureWindow(windowCapture + "_game.png"), gameSeen = true;
                             if (!game && gameSeen && !session.Rewinding() && ++menuFrames == 300) session.CaptureWindow(windowCapture + "_menu.png");
                             if (!game && session.Loading() && ++loadingFrames == 90) session.CaptureWindow(windowCapture + "_loading.png");
                             // (a jump back of the replay timeline, either way: its 30th frame, its first past half of
                             // the run ahead, and the 30th after its beat)
                             if (session.JumpingBack()) {
                               if (++jumpFrames == 30) session.CaptureWindow(windowCapture + "_jump_start.png");
                               if (!jumpHalf && session.JumpBackPercent() >= 50) session.CaptureWindow(windowCapture + "_jump_half.png"), jumpHalf = true;
                               jumpedFrames = 1;
                             } else if (jumpedFrames > 0 && ++jumpedFrames == 31) {
                               session.CaptureWindow(windowCapture + "_jump_there.png");
                             }
                           }
                           // LAST: a beat it starts on its own thread runs beside nothing more of
                           // this frame that touches the simulation (only the picture is shown).
                           timed("(host: the game's beats)", [&] { session.Advance(seconds); });
                         },
                         fullScreen, hooks);
    session.WaitForBeat();  // (a beat still on its thread at the end: done before anything looks at the game again)
    if (done < 0) ok = false;
  } else {
    // Without a window each frame is 1/60 s of the game, as fast as the frames go. MOHO64UI_REAL_TIME=1 (tests): at
    // most 60 frames a second of the PC's clock, each the time since the last (at most 0.25 s, as a window's), so the
    // beats come as in a live window (a network game over a link with latency: TASK_netlat).
    static const bool realTime = std::getenv("MOHO64UI_REAL_TIME") != nullptr;
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    if (realTime) {
#ifdef _WIN32
      timeBeginPeriod(1);  // (sleeps of a millisecond, as the window's)
#endif
      Logf(LogLevel::Info, "moho64ui: without a window, at the PC's clock (MOHO64UI_REAL_TIME)");
    }
    for (; done < o.frames && !ui::ExitRequested(); ++done) {
      double seconds = 1.0 / 60.0;
      if (realTime) {
        const auto now = Clock::now();
        seconds = (std::min)(0.25, std::chrono::duration<double>(now - last).count());
        last = now;
      }
      if (session.RewindArrived()) finishRewind();
      if (session.Rewinding() && game) heldUiSeconds += seconds;
      if (!(session.Rewinding() && !game)) shown->Frame(seconds);  // (a jump back held: not the front end from before)
      // (--menu-flight: the main menu's battle while no game is there, as the window's menu would show it; its end
      // starts it over at once, nothing to cover)
      if (menuFlight && !game && !session.Loading() && !session.running() && !startingUp) {
        using FS = ui::MenuFlight::State;
        if (menuFlight->state() == FS::Idle) menuFlight->Begin();
        menuFlight->WaitLoaded();  // (its load waited for: these frames go faster than the PC's clock, a test's waits count them)
        menuFlight->Advance(seconds, true);
        if (menuFlight->state() == FS::Ended) menuFlight->Restart();
      }
      session.Advance(seconds);
      listen();
      sounds.Update(static_cast<float>(seconds));
      leaveOrRestart();
      startGameInterface();
      if (realTime) std::this_thread::sleep_until(last + std::chrono::microseconds(16667));
    }
  }
  if (o.frames) Logf(LogLevel::Info, "moho64ui: ran %d frames", done);
  // (the program closes: CScApp::Destroy writes the preferences; in a jump back held before the new interface is there,
  // the game's were written when it went, and the front end shown holds older ones)
  if (!(session.Rewinding() && !game)) ui::SavePreferencesOf(shown->L());
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
