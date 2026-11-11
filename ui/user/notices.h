// What the main menu tells the player in one line (ours; Chris's pick of 2026-11-09, the review's idea 1: "when a
// game, a map or a replay fails, the menu says so in a line"): a start that failed or fell back says why, and names
// the log. The original game says nothing on its menu; it has no such line.
#pragma once
#include <string>

struct lua_State;

namespace moho::ui {

// One sentence for the player, kept until the menu has shown it (NoticesSeen). The same sentence twice is kept once.
// From any thread. The caller logs its own line; this is only the menu's.
void AddMenuNotice(const std::string& text);
// The run's log file, named under the menu's line ("" when the run writes none).
void SetMenuNoticeLog(const std::string& file);
// moho64ui_Notices() -> { text, ..., log = file } | nil; moho64ui_NoticesSeen(): the menu showed them, they go;
// moho64ui_AddNoticeForTests(text) (a run with a test script only).
void RegisterNoticeBindings(lua_State* L);

}  // namespace moho::ui
