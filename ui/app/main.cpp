// moho64ui -- the interface track's host: mount the game data the way the original does, boot the
// user Lua state and enter the game's interface scripts. No window yet: it runs frames headless
// and the log says how far the scripts came and which engine functions they missed.
//
//   moho64ui --init <init.lua> [--mods uids.txt] [--prefs <file>] [--stage userinit|setup|frontend]
//            [--size 1920x1080] [--frames N] [--dump-ui] [--lua <file>] [--list-bindings]
//            [--log out.log] [--quiet] [-- <the game's own arguments>]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "script/script_state.h"
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
  if (o.quiet) LogSetEcho(false);
  if (o.init.empty() || width < 1 || height < 1 ||
      (o.stage != "userinit" && o.stage != "setup" && o.stage != "frontend")) {
    std::fprintf(stderr,
                 "usage: moho64ui --init <init.lua> [--mods uids.txt] [--prefs file] [--stage userinit|setup|frontend]\n"
                 "                [--size 1920x1080] [--frames N] [--dump-ui] [--lua file] [--list-bindings]\n"
                 "                [--log out.log] [--quiet] [-- game arguments]\n");
    return 2;
  }
  Vfs vfs;
  ui::UserState::Config cfg;
  cfg.width = width;
  cfg.height = height;
  if (!RunInit(o, vfs, cfg.hookDirs)) return 1;
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

  ui::UserState user(&vfs, cfg);
  bool ok = user.Boot();
  if (ok && o.stage != "userinit") ok = user.StartMainScript();
  if (ok && o.stage == "frontend") ok = user.StartFrontEnd();
  if (!o.luaFile.empty()) {  // debugging aid: a Lua file of your own, run before the frames (it may fork threads)
    if (auto code = hostfs::ReadFile(o.luaFile)) user.RunString(*code, "@" + o.luaFile);
    else Logf(LogLevel::Error, "cannot read %s", o.luaFile.c_str());
  }
  int done = 0;
  for (; done < o.frames; ++done) user.Frame(1.0 / 60.0);
  if (o.frames) Logf(LogLevel::Info, "moho64ui: ran %d frames", done);
  if (dumpUi) user.ui().Dump();
  if (!screenshot.empty()) {  // draw the head once and save the picture
    ui::SoftwareTarget target(width, height);
    user.ui().Render(user.L(), target, 0);
    if (target.SavePng(screenshot)) Logf(LogLevel::Info, "moho64ui: screenshot %s", screenshot.c_str());
    else Logf(LogLevel::Error, "moho64ui: cannot write %s", screenshot.c_str());
  }
  ui::ReportStubCalls();
  if (o.listBindings) ui::ListBindings();
  return ok ? 0 : 1;
}
