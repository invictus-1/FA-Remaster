// The player's own files, as the game's scripts see them: replays and saved games ("special
// files" in the original, FA exe USER_GetSpecialFiles 0x8ca650 and the bindings at 0x844120..
// 0x846280), starting a replay (LaunchReplaySession 0x876e50) and a saved game (LoadSavedGame
// 0x880cc0).
//
// Where they are: in the original, <documents>/My Games/Gas Powered Games/Supreme Commander Forged
// Alliance/replays/<profile>/<name>.SCFAReplay (savegames/ and .SCFASaveGame for saved games). Here the
// same layout under the player's folder the host names (UserState::Config::userDir: the folder of
// the preferences file), so a lab's files stay in the lab.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
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
// SaveGame, CampaignSave). READ 2026-10-18: an extension is "SCFA" (0xe4ddac, kept at 0xf5b9f8)
// and the kind's name, made at start-up: USER_GetReplayExt 0x8ca450 "Replay" (0xe164f4),
// USER_GetSaveGameExt 0x8ca380 "SaveGame" (0xe4e1ac), USER_GetCampaignSaveExt 0x8ca470
// "CampaignSave" (0xe4e1cc); USER_GetSpecialFiles 0x8ca650 puts a campaign save in the saved
// games' folder (USER_GetSaveGameDir 0x8ca2d0, "savegames" 0xe4e1a0). (The player's own saves of
// the original are so named: 1.SCFASaveGame, First expansion.SCFACampaignSave.)
bool KindOf(const std::string& type, const char** folder, const char** extension) {
  if (type == "Replay") return *folder = "replays", *extension = "SCFAReplay", true;
  if (type == "SaveGame") return *folder = "savegames", *extension = "SCFASaveGame", true;
  if (type == "CampaignSave") return *folder = "savegames", *extension = "SCFACampaignSave", true;
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

// GetSpecialFolder(type) (FA exe 0x844cb0, read 2026-10-16): the folder of a kind of the player's
// files, a slash at its end: "SaveGame" and "CampaignSave" the saved games' (savegames/), "Replay"
// the replays' (replays/); "SFT_" before the name may be left out and the case does not count, or
// the kind's number is given; 2, by its number only, the screenshots' (screenshots/, made when it
// is not there). Under the player's folder (as FolderOf). Another kind: the original's error.
int l_GetSpecialFolder(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "string GetSpecialFolder(string type)", 1, lua_gettop(L));
  const std::string type = luaL_checkstring(L, 1);
  std::string name = type;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (name.rfind("sft_", 0) == 0) name = name.substr(4);
  int kind = -1;
  if (name == "savegame") kind = 0;
  else if (name == "replay") kind = 1;
  else if (name == "campaignsave") kind = 3;
  else if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos && name.size() < 4) kind = std::stoi(name);
  if (kind < 0 || kind > 3) return luaL_error(L, "Invalid special file type %s", type.c_str());
  std::string base = UserState::From(L)->config().userDir;
  if (base.empty()) base = ".";
  const char* folder = kind == 1 ? "replays" : kind == 2 ? "screenshots" : "savegames";
  const std::string dir = hostfs::CleanPath(base + "/" + folder);
  if (kind == 2) {
    std::error_code ec;
    fs::create_directories(PathOf(dir), ec);
    if (ec) return luaL_error(L, "Unable to create directory %s", dir.c_str());
  }
  lua_pushstring(L, (dir + "/").c_str());
  return 1;
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

// LoadSavedGame(filename) -> true, or false and why (FA exe 0x880cc0, read 2026-10-18): the game's
// load dialog (/lua/ui/dialogs/saveload.lua DoLoad) shows the why from its SaveErrors. The
// original makes a CSavedGame of the file (0x880330): it opens it (_wfopen "rb" of the name made
// wide), reads its fixed header of 0x2028 bytes in one piece and wants "RGMH" (0x484d4752), the
// number 1 and at +0x18 the game's GUID (0x10a647c, copied at start-up from 0xf8d68c; the
// original's own saves carry it, with the header's size 0x2028 at +8); then it reads the archive
// behind it, whose header's version must be 20 (0x8804fa), and starts the session from it
// (CreateSinglePlayerSessionInfo 0x8807f0, WLD_BeginSession 0x88d060). A SaveLoadError (0x87fdc0, its text "Save/Load error <code>") gives
// false and the code: "CantOpen" (not opened), "InvalidFormat" (a short or wrong header),
// "WrongVersion"; another exception false, "InternalError" and its text; both are logged as
// "Loading save game "<file>" failed: <text>".
// Ours stops after the fixed header: the engine cannot restore a simulation from a saved game
// yet, so a save that passes it gives false, "InternalError" and that reason (OURS, until saved
// games are built: docs/ui-roadmap.md); the archive's version is not read.
int l_LoadSavedGame(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "bool LoadSavedGame(filename)", 1, lua_gettop(L));
  const std::string file = luaL_checkstring(L, 1);
  const auto failed = [&](const char* code, const char* text) {
    Logf(LogLevel::Info, "Loading save game \"%s\" failed: %s", file.c_str(), text);
    lua_pushboolean(L, 0);
    lua_pushstring(L, code);
    if (std::strcmp(code, "InternalError") != 0) return 2;
    lua_pushstring(L, text);
    return 3;
  };
#ifdef _WIN32
  std::FILE* f = _wfopen(PathOf(file).wstring().c_str(), L"rb");
#else
  std::FILE* f = std::fopen(file.c_str(), "rb");
#endif
  if (!f) return failed("CantOpen", "Save/Load error CantOpen");
  std::vector<unsigned char> header(0x2028);
  const bool whole = std::fread(header.data(), header.size(), 1, f) == 1;
  std::fclose(f);
  static const unsigned char kGuid[16] = {0x3a, 0xb4, 0x42, 0xfa, 0x15, 0x1f, 0x70, 0x4f,
                                          0xb0, 0x8f, 0x3e, 0x7c, 0x02, 0x5b, 0xbc, 0x68};
  static const unsigned char kMagic[8] = {'R', 'G', 'M', 'H', 1, 0, 0, 0};  // "RGMH", then 1
  if (!whole || std::memcmp(header.data(), kMagic, sizeof kMagic) != 0 ||
      std::memcmp(header.data() + 0x18, kGuid, sizeof kGuid) != 0)
    return failed("InvalidFormat", "Save/Load error InvalidFormat");
  return failed("InternalError", "this engine cannot load a saved game yet");
}

}  // namespace

void RegisterSpecialFileBindings(lua_State* L) {
  SetGlobal(L, "GetSpecialFiles", l_GetSpecialFiles);
  SetGlobal(L, "GetSpecialFolder", l_GetSpecialFolder);
  SetGlobal(L, "GetSpecialFilePath", l_GetSpecialFilePath);
  SetGlobal(L, "GetSpecialFileInfo", l_GetSpecialFileInfo);
  SetGlobal(L, "RemoveSpecialFile", l_RemoveSpecialFile);
  SetGlobal(L, "CopyCurrentReplay", l_CopyCurrentReplay);
  SetGlobal(L, "LaunchReplaySession", l_LaunchReplaySession);
  SetGlobal(L, "LoadSavedGame", l_LoadSavedGame);
}

}  // namespace moho::ui
