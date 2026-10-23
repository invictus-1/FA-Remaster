// The replay timeline's functions for the interface's scripts (ours, not the original's: see replay_timeline.h). The
// bar itself is /moho64ui/lua/timeline.lua (ui/data/lua/timeline.lua); the host's side is GameSession's.
#include "user/replay_timeline.h"

#include <cctype>

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

// moho64ui_ReplayTimeline() -> {ticks, tick, seeking, seekTo, scanning, scanned, moments, buckets, kept} while a
// replay plays (ticks: its length in beats; seekTo: the beat a jump goes to; scanned: how far the moments' scan has
// got; moments and buckets: how many there are so far; kept: a jump back's kept table waits, moho64ui_ReplayTakeKept);
// nil otherwise.
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
  return 1;
}

// moho64ui_ReplayMoments([first]) -> the moments from the first-th on (1 ..): {tick, kind, army (1-based, as the
// scripts count armies), position {x, y, z}, unit (a blueprint id)}.
int l_ReplayMoments(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  std::vector<ReplayTimelineSource::Moment> moments;
  source->MomentsFrom(FirstIndex(L, 1), &moments);
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
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  return 1;
}

// moho64ui_ReplayFight([first]) -> the fighting from the first-th 5 s on (1 ..), every army's summed: {tick (the
// first beat of the 5 s), damage, hits, deaths}.
int l_ReplayFight(lua_State* L) {
  ReplayTimelineSource* source = Source(L);
  if (!source) return 0;
  std::vector<ReplayTimelineSource::Fight> fight;
  source->FightFrom(FirstIndex(L, 1), &fight);
  lua_newtable(L);
  for (size_t i = 0; i < fight.size(); ++i) {
    lua_newtable(L);
    SetNumber(L, "tick", fight[i].tick0);
    SetNumber(L, "damage", fight[i].damage);
    SetNumber(L, "hits", fight[i].hits);
    SetNumber(L, "deaths", fight[i].deaths);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  return 1;
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

}  // namespace

void RegisterTimelineBindings(lua_State* L) {
  lua_register(L, "moho64ui_ReplayTimeline", l_ReplayTimeline);
  lua_register(L, "moho64ui_ReplayMoments", l_ReplayMoments);
  lua_register(L, "moho64ui_ReplayFight", l_ReplayFight);
  lua_register(L, "moho64ui_ReplaySeek", l_ReplaySeek);
  lua_register(L, "moho64ui_ReplayTakeKept", l_ReplayTakeKept);
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
