// moho64 -- milestone 1 host: mount the game data the way the original does, then run the
// rules state (blueprint loading) or compile-check every script.
//
//   moho64 --init <init.lua> [--drive c=/host/dir] [--folder NAME=path] [--mods uids.txt]
//          [--log out.log] [--check-lua] [--rules] [--quiet]
#include <cstdio>
#include <cstring>
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
#include "script/script_state.h"

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
  std::vector<std::string> exec;  // --exec <lua>: run in a fresh rules state after mounting
};

Options* g_opts = nullptr;

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
      std::string clean = mdir;
      size_t sl = clean.find_last_of("/\\");
      std::string base = clean.substr(0, sl);
      for (const auto& n : hostfs::Glob(clean)) {
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

// Build __active_mods the way the game's mod manager does: each selected mod's mod_info.lua
// run in an environment of defaults, `location` = its folder.
void SetActiveMods(ScriptState& st, const std::vector<std::string>& uids) {
  lua_State* L = st.L();
  Vfs* vfs = st.vfs();
  std::map<std::string, int> refByUid;  // uid -> stack index
  lua_newtable(L);                      // __active_mods
  int modsIdx = lua_gettop(L);
  std::map<std::string, std::string> fileByUid;
  for (const auto& file : vfs->FindFiles("/mods", "*mod_info.lua")) {
    std::string location = file.substr(0, file.rfind('/'));
    std::string code =
        "return function(file, location)\n"
        "local env = { location = location, name = file, description = '', author = '', copyright = '',\n"
        "  exclusive = false, selectable = true, hookdir = '/hook', shadowdir = '/shadow', uid = file }\n"
        "doscript(file, env)\n"
        "env.location = location\n"
        "return env\n"
        "end\n";
    lua_pushcfunction(L, ScriptTraceback);
    int eh = lua_gettop(L);
    if (luaL_loadbuffer(L, code.data(), code.size(), "=modinfo") != 0) {
      LogScriptError(lua_tostring(L, -1));
      lua_settop(L, modsIdx);
      continue;
    }
    lua_call(L, 0, 1);  // -> the loader function
    lua_pushstring(L, file.c_str());
    lua_pushstring(L, location.c_str());
    if (lua_pcall(L, 2, 1, eh) != 0) {
      LogScriptError(std::string("Problem loading ") + file + ":\n" + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
      lua_settop(L, modsIdx);
      continue;
    }
    lua_getfield(L, -1, "uid");
    std::string uid = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    for (size_t i = 0; i < uids.size(); ++i) {
      if (uids[i] == uid) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, modsIdx, static_cast<int>(i) + 1);
      }
    }
    lua_settop(L, modsIdx);
  }
  lua_setglobal(L, "__active_mods");
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
    else if (a == "--check-lua") o.checkLua = true;
    else if (a == "--rules") o.rules = true;
    else if (a == "--quiet") o.quiet = true;
    else if (a == "--exec") o.exec.push_back(next());
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
  if (!RunInit(o, vfs, hookDirs)) return 1;
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
  return rc;
}
