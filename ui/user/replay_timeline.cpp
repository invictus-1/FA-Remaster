// The replay timeline's functions for the interface's scripts (ours, not the original's: see replay_timeline.h). The
// bar itself is /moho64ui/lua/timeline.lua (ui/data/lua/timeline.lua); the host's side is GameSession's.
#include "user/replay_timeline.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "core/hostfs.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui {

namespace {

ReplayTimelineSource* Source(lua_State* L) { return UserState::From(L)->config().timeline; }

void SetNumber(lua_State* L, const char* key, double v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}
void SetBool(lua_State* L, const char* key, bool v) {
  lua_pushstring(L, key);
  lua_pushboolean(L, v ? 1 : 0);
  lua_rawset(L, -3);
}
void SetString(lua_State* L, const char* key, const std::string& v) {
  lua_pushstring(L, key);
  lua_pushstring(L, v.c_str());
  lua_rawset(L, -3);
}
size_t FirstIndex(lua_State* L, int idx) {  // (the scripts count from 1)
  const double first = lua_isnumber(L, idx) ? lua_tonumber(L, idx) : 1.0;
  return first < 1.0 ? 0 : static_cast<size_t>(first) - 1;
}

// moho64ui_ReplayTimeline() -> {ticks, tick, seeking, seekTo, scanning, scanned, moments, buckets, kept, jumpsBack,
// backLoadingScreens, lastBackSeconds} while a replay plays (ticks: its length in beats; seekTo: the beat a jump goes
// to; scanned: how far the moments' scan has got; moments and buckets: how many there are so far; kept: a jump back's
// kept table waits, moho64ui_ReplayTakeKept; jumpsBack: the jumps back that reached their beat, backLoadingScreens the
// loading screens shown during them, lastBackSeconds the last one's time on the PC's clock; bookmarksFile: the file
// the replay's bookmarks are kept in, ReplayBookmarksFile); nil otherwise.
int l_ReplayTimeline(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  const ReplayTimelineSource::State s = source->Now();
  lua_newtable(L);
  SetNumber(L, "ticks", s.ticks);
  SetNumber(L, "tick", s.tick);
  SetBool(L, "seeking", s.seekTo != 0);
  SetNumber(L, "seekTo", s.seekTo);
  SetBool(L, "scanning", s.scanning);
  SetNumber(L, "scanned", s.scanned);
  SetNumber(L, "moments", static_cast<double>(s.moments));
  SetNumber(L, "buckets", static_cast<double>(s.buckets));
  SetBool(L, "kept", s.kept);
  SetNumber(L, "jumpsBack", s.jumpsBack);
  SetNumber(L, "backLoadingScreens", s.backLoadingScreens);
  SetNumber(L, "lastBackSeconds", s.lastBackSeconds);
  SetString(L, "bookmarksFile", source->BookmarksFile());
  return 1;
}

// moho64ui_ReplayBookmarks() -> the player's bookmarks of the replay, in the order of their beats: {tick, name} each.
int l_ReplayBookmarks(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  const std::vector<ReplayBookmark> list = source->Bookmarks();
  lua_newtable(L);
  for (size_t i = 0; i < list.size(); ++i) {
    lua_newtable(L);
    SetNumber(L, "tick", list[i].tick);
    SetString(L, "name", list[i].name);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  return 1;
}

// moho64ui_ReplaySetBookmarks({{tick, name}, ...}) -> true, or false and the reason (the file could not be written;
// they are kept for this run of the replay all the same): the bookmarks put in place of the ones there (made plain and
// ordered: CleanBookmarks) and written beside the replay. An entry without a number for its tick is left out.
int l_ReplaySetBookmarks(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no replay plays");
    return 2;
  }
  luaL_checktype(L, 1, LUA_TTABLE);
  std::vector<ReplayBookmark> list;
  for (int i = 1;; ++i) {
    lua_rawgeti(L, 1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_pushstring(L, "tick");
    lua_rawget(L, -2);
    const bool hasTick = lua_isnumber(L, -1) != 0;
    const double tick = hasTick ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    lua_pushstring(L, "name");
    lua_rawget(L, -2);
    const std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    if (hasTick) list.push_back({tick < 0 ? 0u : static_cast<uint32_t>(tick), name});
  }
  std::string why;
  if (!source->SetBookmarks(list, &why)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

// The moments from the first-th on as the scripts get them: a list of {tick, kind, army (1-based, as the scripts count
// armies; 0 none), position {x, y, z}, unit (a blueprint id), entity (nil for none)}.
void PushMoments(lua_State* L, ReplayTimelineSource* source, size_t first) {
  std::vector<ReplayTimelineSource::Moment> moments;
  source->MomentsFrom(first, &moments);
  lua_newtable(L);
  for (size_t i = 0; i < moments.size(); ++i) {
    const ReplayTimelineSource::Moment& m = moments[i];
    lua_newtable(L);
    SetNumber(L, "tick", m.tick);
    SetString(L, "kind", m.kind);
    SetNumber(L, "army", m.army + 1);
    lua_pushstring(L, "position");
    lua_newtable(L);
    for (int k = 0; k < 3; ++k) {
      lua_pushnumber(L, m.pos[k]);
      lua_rawseti(L, -2, k + 1);
    }
    lua_rawset(L, -3);
    SetString(L, "unit", m.unit);
    if (m.entity != 0xffffffffu) SetNumber(L, "entity", m.entity);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
}

// The fighting from the first-th 5 s on: a list of {tick (the first beat of the 5 s), damage, hits, deaths}.
void PushFight(lua_State* L, ReplayTimelineSource* source, size_t first) {
  std::vector<ReplayTimelineSource::Fight> fight;
  source->FightFrom(first, &fight);
  lua_newtable(L);
  for (size_t i = 0; i < fight.size(); ++i) {
    lua_newtable(L);
    SetNumber(L, "tick", fight[i].tick0);
    SetNumber(L, "damage", fight[i].damage);
    SetNumber(L, "hits", fight[i].hits);
    SetNumber(L, "deaths", fight[i].deaths);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
}

// moho64ui_ReplayMoments([first]) -> the moments from the first-th on (1 ..): {tick, kind, army (1-based, as the
// scripts count armies; 0 none), position {x, y, z}, unit (a blueprint id), entity (the unit's or the missile's
// entity id, as GetUnitById takes it; nil for none)}.
int l_ReplayMoments(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  PushMoments(L, source, FirstIndex(L, 1));
  return 1;
}

// moho64ui_ReplayFight([first]) -> the fighting from the first-th 5 s on (1 ..), every army's summed: {tick (the
// first beat of the 5 s), damage, hits, deaths}.
int l_ReplayFight(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  PushFight(L, source, FirstIndex(L, 1));
  return 1;
}

// moho64ui_GameMoments([start]) -> the game's moments for its score screen (ours: the score screen's graphs mark them,
// /lua/ui/dialogs/hotstats.lua's hook): {ticks (the game's length in game ticks, as far as it is known), scanning
// (still being looked for), scanned (the game tick the look has got to), moments = {...} (as moho64ui_ReplayMoments),
// fight = {...} (as moho64ui_ReplayFight)}. A replay's are its timeline's; a game played's are looked for in its own
// replay once the game is over (or now, with `start`: the score screen opened while the game goes on).
// GameSession::GameMoments. nil: none (no replay of the game: a campaign operation; or not begun).
int l_GameMoments(lua_State* L) {
  const auto& ask = UserState::From(L)->config().gameMoments;
  ReplayTimelineSource* source = ask ? ask(lua_toboolean(L, 1) != 0) : nullptr;
  if (!source) return 0;
  const ReplayTimelineSource::State s = source->Now();
  lua_newtable(L);
  SetNumber(L, "ticks", s.ticks);
  SetBool(L, "scanning", s.scanning);
  SetNumber(L, "scanned", s.scanned);
  lua_pushstring(L, "moments");
  PushMoments(L, source, 0);
  lua_rawset(L, -3);
  lua_pushstring(L, "fight");
  PushFight(L, source, 0);
  lua_rawset(L, -3);
  return 1;
}

// moho64ui_GameMomentsCaptureForTests(path): the next frame the window shows saved as a PNG at `path` while there are
// game moments (the score screen's pictures for Chris; the host's --window-capture way, nothing without a window).
int l_GameMomentsCaptureForTests(lua_State* L) {
  const auto& ask = UserState::From(L)->config().gameMoments;
  ReplayTimelineSource* source = ask ? ask(false) : nullptr;
  if (source && lua_isstring(L, 1)) source->CaptureForTests(lua_tostring(L, 1));
  return 0;
}

// moho64ui_ReplaySeek(tick [, keep]) -> true, or false and the reason: the replay jumps to that beat (forward it runs
// ahead; back it starts over, the interface made anew, and runs ahead). `keep` (a table of plain values: the camera's
// settings and the focus army) waits for the new interface across a jump back (moho64ui_ReplayTakeKept).
int l_ReplaySeek(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "no replay plays");
    return 2;
  }
  const double tick = luaL_checknumber(L, 1);
  std::string keep;
  if (lua_istable(L, 2)) SerializeLua(L, 2, &keep);
  std::string why;
  if (!source->Seek(tick < 0 ? 0u : static_cast<uint32_t>(tick), keep, &why)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

// moho64ui_ReplayTakeKept() -> what the last jump back kept (moho64ui_ReplaySeek's `keep`), once; nil when nothing.
int l_ReplayTakeKept(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  const std::string kept = source->TakeKept();
  if (kept.empty()) return 0;
  const int top = lua_gettop(L);
  if (!PushSerializedLua(L, kept)) {
    lua_settop(L, top);
    return 0;
  }
  return 1;
}

// moho64ui_ReplayWatchForTests({tick, ...}): the simulation's state line after each of those ticks is taken as the
// beats run (ReplayTimelineSource::Watch); moho64ui_ReplayWatchedForTests() -> {[tick] = line} of those taken so far.
// For tests only (ui/tests/rewind.lua).
int l_ReplayWatchForTests(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  std::vector<uint32_t> ticks;
  if (lua_istable(L, 1))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 1, i);
      const bool number = lua_isnumber(L, -1) != 0;
      if (number && lua_tonumber(L, -1) >= 0) ticks.push_back(static_cast<uint32_t>(lua_tonumber(L, -1)));
      lua_pop(L, 1);
      if (!number) break;
    }
  source->Watch(ticks);
  return 0;
}

int l_ReplayWatchedForTests(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  lua_newtable(L);
  for (const auto& [tick, line] : source->Watched()) {
    lua_pushnumber(L, tick);
    lua_pushstring(L, line.c_str());
    lua_rawset(L, -3);
  }
  return 1;
}

// moho64ui_ReplayCaptureForTests(path): the next frame the window shows saved as a PNG at `path`
// (ReplayTimelineSource::CaptureForTests). For tests only (a jump back's picture against a straight playback's).
int l_ReplayCaptureForTests(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (source && lua_isstring(L, 1)) source->CaptureForTests(lua_tostring(L, 1));
  return 0;
}

}  // namespace

// --- the bookmarks' file ---

namespace {
const char kBookmarksHeader[] = "FA Remastered replay bookmarks";
const size_t kBookmarkNameBytes = 64;
const size_t kMostBookmarks = 500;

std::string TickTime(uint32_t tick) {  // (as the bar writes a time: timeline.lua TimeText)
  const uint32_t seconds = tick / 10;
  char text[32];
  if (seconds >= 3600)
    std::snprintf(text, sizeof text, "%u:%02u:%02u", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
  else
    std::snprintf(text, sizeof text, "%u:%02u", seconds / 60, seconds % 60);
  return text;
}

// A name on one line: control characters made blanks, blanks run together, none at either end, at most
// kBookmarkNameBytes bytes, cut where a UTF-8 character starts.
std::string PlainName(const std::string& name) {
  std::string out;
  bool blank = false;
  for (char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7f || c == ' ') {
      blank = !out.empty();
      continue;
    }
    if (blank) out.push_back(' ');
    blank = false;
    out.push_back(ch);
  }
  if (out.size() > kBookmarkNameBytes) {
    size_t cut = kBookmarkNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;  // (not inside a character)
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}
}  // namespace

std::string ReplayBookmarksFile(const std::string& replay) { return replay + ".bookmarks.txt"; }

std::string ReplayDigest(const std::string& bytes) {
  uint64_t h = 0xcbf29ce484222325ull;  // (FNV-1a, 64 bits: its offset basis and prime)
  for (char ch : bytes) {
    h ^= static_cast<unsigned char>(ch);
    h *= 0x100000001b3ull;
  }
  char text[48];
  std::snprintf(text, sizeof text, "%llu %016llx", static_cast<unsigned long long>(bytes.size()), static_cast<unsigned long long>(h));
  return text;
}

std::vector<ReplayBookmark> CleanBookmarks(std::vector<ReplayBookmark> list, uint32_t ticks) {
  for (ReplayBookmark& b : list) {
    if (ticks > 0 && b.tick > ticks) b.tick = ticks;
    b.name = PlainName(b.name);
    if (b.name.empty()) b.name = TickTime(b.tick);
  }
  std::stable_sort(list.begin(), list.end(), [](const ReplayBookmark& a, const ReplayBookmark& b) { return a.tick < b.tick; });
  if (list.size() > kMostBookmarks) list.resize(kMostBookmarks);
  return list;
}

std::string FormatReplayBookmarks(const std::string& replayName, const std::string& digest, const std::vector<ReplayBookmark>& list) {
  std::string out = std::string(kBookmarksHeader) + " 1\n";
  out += "# The bookmarks of the replay " + PlainName(replayName) + " beside this file (FA Remastered's replay timeline).\n";
  out += "# One a line: the game tick (ten a second), a tab, the name. The replay line: the replay's size and hash.\n";
  out += "replay " + digest + "\n";
  for (const ReplayBookmark& b : list) out += std::to_string(b.tick) + "\t" + PlainName(b.name) + "\n";
  return out;
}

bool ParseReplayBookmarks(const std::string& text, const std::string& digest, std::vector<ReplayBookmark>* out, std::string* why) {
  out->clear();
  std::vector<ReplayBookmark> list;
  size_t at = 0;
  bool first = true;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line.compare(0, sizeof kBookmarksHeader - 1, kBookmarksHeader) != 0) {
        *why = "not a bookmarks file (its first line)";
        return false;
      }
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 7, "replay ") == 0) {
      std::string theirs = line.substr(7);
      while (!theirs.empty() && theirs.back() == ' ') theirs.pop_back();
      if (theirs != digest) {
        *why = "they belong to another replay (" + theirs + ", this one " + digest + ")";
        return false;
      }
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(line[0]))) continue;  // (a line of no bookmark: left out)
    char* rest = nullptr;
    const unsigned long long tick = std::strtoull(line.c_str(), &rest, 10);
    if (!rest || (*rest != '\t' && *rest != ' ' && *rest != '\0')) continue;
    list.push_back({tick > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(tick), rest});
  }
  if (first) {
    *why = "an empty file";
    return false;
  }
  *out = CleanBookmarks(std::move(list), 0);
  return true;
}

bool LoadReplayBookmarks(const std::string& replay, const std::string& digest, std::vector<ReplayBookmark>* out, std::string* why) {
  out->clear();
  const std::string file = ReplayBookmarksFile(replay);
  if (!hostfs::IsFile(file)) return true;
  const std::optional<std::string> text = hostfs::ReadFile(file);
  if (!text) {
    *why = "cannot read " + hostfs::DisplayPath(file);
    return false;
  }
  return ParseReplayBookmarks(*text, digest, out, why);
}

bool SaveReplayBookmarks(const std::string& replay, const std::string& digest, const std::vector<ReplayBookmark>& list, std::string* why) {
  const std::string file = ReplayBookmarksFile(replay);
  if (list.empty() && !hostfs::IsFile(file)) return true;
  const size_t slash = replay.find_last_of("/\\");
  const std::string name = slash == std::string::npos ? replay : replay.substr(slash + 1);
  if (!hostfs::WriteFile(file, FormatReplayBookmarks(name, digest, list))) {
    *why = "cannot write " + hostfs::DisplayPath(file);
    return false;
  }
  return true;
}

void RegisterTimelineBindings(lua_State* L) {
  lua_register(L, "moho64ui_ReplayTimeline", l_ReplayTimeline);
  lua_register(L, "moho64ui_ReplayMoments", l_ReplayMoments);
  lua_register(L, "moho64ui_ReplayFight", l_ReplayFight);
  lua_register(L, "moho64ui_ReplaySeek", l_ReplaySeek);
  lua_register(L, "moho64ui_ReplayTakeKept", l_ReplayTakeKept);
  lua_register(L, "moho64ui_ReplayBookmarks", l_ReplayBookmarks);
  lua_register(L, "moho64ui_ReplaySetBookmarks", l_ReplaySetBookmarks);
  lua_register(L, "moho64ui_ReplayWatchForTests", l_ReplayWatchForTests);
  lua_register(L, "moho64ui_ReplayWatchedForTests", l_ReplayWatchedForTests);
  lua_register(L, "moho64ui_ReplayCaptureForTests", l_ReplayCaptureForTests);
  lua_register(L, "moho64ui_GameMoments", l_GameMoments);
  lua_register(L, "moho64ui_GameMomentsCaptureForTests", l_GameMomentsCaptureForTests);
}

// remaster_timeline [on|off]: the bar's Toggle (no word) or Show / Hide (/moho64ui/lua/timeline.lua). Only while a
// replay plays; otherwise the console says so.
void TimelineCommand(lua_State* L, const std::string& value) {
  UserState* user = UserState::From(L);
  if (!user->gameInterface) return ConsoleNoSession(L);
  if (!Source(L)) return ConsolePrint(L, "remaster_timeline: only while a replay plays");
  std::string word;
  for (char ch : value)
    if (!std::isspace(static_cast<unsigned char>(ch))) word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  const char* fn = word.empty() ? "Toggle" : (word == "ON" || word == "1" || word == "TRUE") ? "Show" : "Hide";
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/moho64ui/lua/timeline.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, top + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  } else {
    Logf(LogLevel::Warning, "moho64ui: the replay timeline (/moho64ui/lua/timeline.lua) is not there: %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  }
  lua_settop(L, top);
}

}  // namespace moho::ui
