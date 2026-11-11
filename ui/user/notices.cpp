// The main menu's notices: see notices.h.
#include "user/notices.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

std::mutex g_lock;
std::vector<std::string> g_notices;  // in the order they came
std::string g_log;

// (the name as it was given, with the system's own slashes)
std::string Shown(std::string path) {
#ifdef _WIN32
  for (char& c : path)
    if (c == '/') c = '\\';
#endif
  return path;
}

int l_Notices(lua_State* L) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_notices.empty()) return lua_pushnil(L), 1;
  lua_newtable(L);
  for (size_t i = 0; i < g_notices.size(); ++i) {
    lua_pushstring(L, g_notices[i].c_str());
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  if (!g_log.empty()) {
    lua_pushstring(L, "log");
    lua_pushstring(L, Shown(g_log).c_str());
    lua_rawset(L, -3);
  }
  return 1;
}

int l_NoticesSeen(lua_State*) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_notices.clear();
  return 0;
}

int l_AddNoticeForTests(lua_State* L) {
  if (!TestFileWritesAllowed()) return luaL_error(L, "moho64ui_AddNoticeForTests: only in a run with a test script");
  AddMenuNotice(luaL_checkstring(L, 1));
  return 0;
}

}  // namespace

void AddMenuNotice(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> hold(g_lock);
  if (std::find(g_notices.begin(), g_notices.end(), text) == g_notices.end()) g_notices.push_back(text);
}

void SetMenuNoticeLog(const std::string& file) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_log = file;
}

void RegisterNoticeBindings(lua_State* L) {
  SetGlobal(L, "moho64ui_Notices", l_Notices);
  SetGlobal(L, "moho64ui_NoticesSeen", l_NoticesSeen);
  SetGlobal(L, "moho64ui_AddNoticeForTests", l_AddNoticeForTests);
}

}  // namespace moho::ui
