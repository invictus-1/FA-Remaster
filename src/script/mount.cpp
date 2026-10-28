#include "script/mount.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

#include <cctype>
#include <filesystem>
#include <system_error>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace moho {
namespace {

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

// SHGetFolderPath(name); upvalue 1: the caller's answers (a std::map<std::string, std::string>*)
int l_SHGetFolderPath(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  const auto* folders = static_cast<const std::map<std::string, std::string>*>(lua_touserdata(L, lua_upvalueindex(1)));
  auto it = folders->find(name);
  std::string v = it != folders->end() ? it->second : SystemFolder(name);
  if (!v.empty() && v.back() != '/' && v.back() != '\\') v += '\\';
  lua_pushstring(L, v.c_str());
  return 1;
}

// A host folder for comparing: made absolute (an init file may list it relative to the working folder), lower
// case, \ as /, no trailing /
std::string FolderKey(const std::string& folder) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(folder), ec);
  std::string d = ec ? folder : abs.lexically_normal().string();
  for (char& c : d) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  return d;
}

}  // namespace

bool MountFromInitFile(const std::string& initFile, const std::map<std::string, std::string>& folders, Vfs& vfs,
                       std::vector<std::string>& hookDirs, const std::vector<std::string>* skip) {
  auto skipped = [&](const std::string& d) {
    if (!skip) return false;
    const std::string k = FolderKey(d);
    for (const std::string& s : *skip)
      if (FolderKey(s) == k) return true;
    return false;
  };
  ScriptState init(ScriptState::Kind::Init, &vfs);
  lua_State* L = init.L();
  std::string dir = initFile;
  size_t s = dir.find_last_of("/\\");
  dir = s == std::string::npos ? "." : dir.substr(0, s);
  lua_pushstring(L, dir.c_str());
  lua_setglobal(L, "InitFileDir");
  lua_pushlightuserdata(L, const_cast<std::map<std::string, std::string>*>(&folders));
  lua_pushcclosure(L, l_SHGetFolderPath, 1);
  lua_setglobal(L, "SHGetFolderPath");
  auto src = hostfs::ReadFile(initFile);
  if (!src) {
    Logf(LogLevel::Error, "cannot read init file %s", initFile.c_str());
    return false;
  }
  if (!init.DoString(*src, "@" + hostfs::DisplayPath(initFile))) return false;

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
        if (!skipped(base + "/" + n)) vfs.Mount(base + "/" + n, mp);
      }
    } else if (skipped(mdir)) {
      Logf(LogLevel::Info, "init file: %s left out", hostfs::DisplayPath(mdir).c_str());
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

}  // namespace moho
