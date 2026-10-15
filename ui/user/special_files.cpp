// The player's own files, as the game's scripts see them: replays and saved games ("special
// files" in the original, FA exe USER_GetSpecialFiles 0x8ca650 and the bindings at 0x844120..
// 0x846280), and starting a replay (LaunchReplaySession 0x876e50).
//
// Where they are: in the original, <documents>/My Games/Gas Powered Games/Supreme Commander Forged
// Alliance/replays/<profile>/<name>.SCFAReplay (savegames/ and .SCFASave for saved games). Here the
// same layout under the player's folder the host names (UserState::Config::userDir: the folder of
// the preferences file), so a lab's files stay in the lab.
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "core/hostfs.h"
#include "core/log.h"
#include "script/lua.hpp"
#include "user/bindings.h"
#include "user/user_state.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace moho::ui {
namespace {

namespace fs = std::filesystem;

// (UTF-8 text and paths both ways; std::filesystem's own u8 forms are char8_t under C++20)
fs::path PathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string TextOf(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// The folder and the extension of a kind of special file (the original's three: Replay,
// SaveGame, CampaignSave). The campaign's own extension is not read (its string is made at
// start-up): a campaign save is refused until the campaign is built.
bool KindOf(const std::string& type, const char** folder, const char** extension) {
  if (type == "Replay") return *folder = "replays", *extension = "SCFAReplay", true;
  if (type == "SaveGame") return *folder = "savegames", *extension = "SCFASave", true;
  return false;
}
// The kind's folder, with a slash at its end (the scripts add "<profile>/<name>.<ext>" to it);
// raises the original's error for an unknown kind.
std::string FolderOf(lua_State* L, const std::string& type, const char** extension) {
  const char* folder = nullptr;
  if (!KindOf(type, &folder, extension)) luaL_error(L, "Invalid special file type %s", type.c_str());
  std::string base = UserState::From(L)->config().userDir;
  if (base.empty()) base = ".";
  std::string dir = hostfs::CleanPath(base + "/" + folder);
  std::error_code ec;
  fs::create_directories(PathOf(dir), ec);
  return dir + "/";
}
std::string FileOf(lua_State* L, int first) {
  const std::string profile = luaL_checkstring(L, first), name = luaL_checkstring(L, first + 1), type = luaL_checkstring(L, first + 2);
  const char* extension = nullptr;
  const std::string dir = FolderOf(L, type, &extension);
  return dir + profile + "/" + name + "." + extension;
}
void SetField(lua_State* L, const char* key, lua_Number v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}

// GetSpecialFiles(type) -> {directory = "<folder>/", extension = "<ext>", files = {[profile] =
// {name, ...}}}: every profile's folder and the files of the kind in it, names without the
// extension.
int l_GetSpecialFiles(lua_State* L) {
  const char* extension = nullptr;
  const std::string dir = FolderOf(L, luaL_checkstring(L, 1), &extension);
  const std::string suffix = hostfs::ToLower(std::string(".") + extension);
  lua_newtable(L);
  lua_pushstring(L, "files");
  lua_newtable(L);
  for (const std::string& profile : hostfs::ListDirectory(dir)) {
    if (!hostfs::IsDirectory(dir + profile)) continue;
    lua_pushstring(L, profile.c_str());
    lua_newtable(L);
    int n = 0;
    for (const std::string& name : hostfs::ListDirectory(dir + profile)) {
      if (name.size() <= suffix.size() || hostfs::ToLower(name.substr(name.size() - suffix.size())) != suffix) continue;
      lua_pushstring(L, name.substr(0, name.size() - suffix.size()).c_str());
      lua_rawseti(L, -2, ++n);
    }
    lua_rawset(L, -3);
  }
  lua_rawset(L, -3);
  lua_pushstring(L, "directory");
  lua_pushstring(L, dir.c_str());
  lua_rawset(L, -3);
  lua_pushstring(L, "extension");
  lua_pushstring(L, extension);
  lua_rawset(L, -3);
  return 1;
}

// GetSpecialFilePath(profile, name, type) -> the file's path.
int l_GetSpecialFilePath(lua_State* L) {
  lua_pushstring(L, FileOf(L, 1).c_str());
  return 1;
}

// GetSpecialFileInfo(profile, name, type) -> {IsFolder, ReadOnly, SizeBytes, TimeStamp (the
// file's write time as 16 hex digits of Windows' 100 ns count, so that it sorts as text),
// WriteTime = {year, month, mday, wday, hour, minute, second}} (FA exe 0x8455c0), or false when
// there is no such file. (The time's fields are local time: INFERRED.)
int l_GetSpecialFileInfo(lua_State* L) {
  const auto stat = hostfs::Stat(FileOf(L, 1));
  if (!stat) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, "IsFolder");
  lua_pushboolean(L, stat->isFolder);
  lua_rawset(L, -3);
  lua_pushstring(L, "ReadOnly");
  lua_pushboolean(L, stat->isReadOnly);
  lua_rawset(L, -3);
  SetField(L, "SizeBytes", static_cast<lua_Number>(stat->size));
  char stamp[32];
  const unsigned long long ticks = (static_cast<unsigned long long>(stat->writeTime) + 11644473600ULL) * 10000000ULL;
  std::snprintf(stamp, sizeof stamp, "%016llx", ticks);
  lua_pushstring(L, "TimeStamp");
  lua_pushstring(L, stamp);
  lua_rawset(L, -3);
  const std::time_t t = static_cast<std::time_t>(stat->writeTime);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  lua_pushstring(L, "WriteTime");
  lua_newtable(L);
  SetField(L, "year", local.tm_year + 1900);
  SetField(L, "month", local.tm_mon + 1);
  SetField(L, "mday", local.tm_mday);
  SetField(L, "wday", local.tm_wday);
  SetField(L, "hour", local.tm_hour);
  SetField(L, "minute", local.tm_min);
  SetField(L, "second", local.tm_sec);
  lua_rawset(L, -3);
  return 1;
}

// RemoveSpecialFile(profile, name, type): the player deleted it in the game's file dialog, which
// asks "Are you sure you want to move this file to the recycle bin?": so it goes to the recycle
// bin (Windows' own, where it can be put back; elsewhere a "recycled" folder beside the kind's).
int l_RemoveSpecialFile(lua_State* L) {
  const std::string file = FileOf(L, 1);
  if (!hostfs::IsFile(file)) return 0;
#ifdef _WIN32
  std::wstring from = PathOf(file).wstring();
  std::replace(from.begin(), from.end(), L'/', L'\\');
  from.push_back(L'\0');  // (the list of names ends in two zeros)
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  if (SHFileOperationW(&op) != 0) Logf(LogLevel::Warning, "moho64ui: RemoveSpecialFile: %s could not go to the recycle bin", file.c_str());
#else
  std::error_code ec;
  const fs::path to = PathOf(file).parent_path().parent_path().parent_path() / "recycled";
  fs::create_directories(to, ec);
  fs::rename(PathOf(file), to / PathOf(file).filename(), ec);
#endif
  return 0;
}

// CopyCurrentReplay(profile, name): the replay of the game running or last played is kept under a
// name (the game's "Save Replay" dialog).
int l_CopyCurrentReplay(lua_State* L) {
  const std::string profile = luaL_checkstring(L, 1), name = luaL_checkstring(L, 2);
  lua_settop(L, 2);
  lua_pushstring(L, "Replay");
  const std::string to = FileOf(L, 1);
  const auto& current = UserState::From(L)->config().currentReplay;
  const std::string from = current ? current() : std::string();
  auto data = from.empty() ? std::nullopt : hostfs::ReadFile(from);
  if (!data) {
    Logf(LogLevel::Warning, "moho64ui: CopyCurrentReplay: there is no replay of a game to keep");
    return 0;
  }
  std::error_code ec;
  fs::create_directories(PathOf(to).parent_path(), ec);
  if (!hostfs::WriteFile(to, *data)) Logf(LogLevel::Warning, "moho64ui: CopyCurrentReplay: cannot write %s", to.c_str());
  else Logf(LogLevel::Info, "moho64ui: the replay is kept as %s", to.c_str());
  return 0;
}

// LaunchReplaySession(file) -> true when the replay's session starts (the dialog then closes
// the front end as for a launched game), false otherwise.
int l_LaunchReplaySession(lua_State* L) {
  const std::string file = luaL_checkstring(L, 1);
  const auto& launch = UserState::From(L)->config().launchReplay;
  std::string why = "this host cannot start a replay here";
  const bool ok = launch && launch(L, file, &why);
  if (!ok) Logf(LogLevel::Warning, "moho64ui: LaunchReplaySession %s: %s", file.c_str(), why.c_str());
  lua_pushboolean(L, ok);
  return 1;
}

}  // namespace

void RegisterSpecialFileBindings(lua_State* L) {
  SetGlobal(L, "GetSpecialFiles", l_GetSpecialFiles);
  SetGlobal(L, "GetSpecialFilePath", l_GetSpecialFilePath);
  SetGlobal(L, "GetSpecialFileInfo", l_GetSpecialFileInfo);
  SetGlobal(L, "RemoveSpecialFile", l_RemoveSpecialFile);
  SetGlobal(L, "CopyCurrentReplay", l_CopyCurrentReplay);
  SetGlobal(L, "LaunchReplaySession", l_LaunchReplaySession);
}

}  // namespace moho::ui
