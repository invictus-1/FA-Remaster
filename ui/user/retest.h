// The re-test list in the program (ours; Chris's pick of 2026-11-09, the review's idea 12: "a small panel of our own
// (off in shipped builds) that reads the re-test list, shows the items for the screen you are on with their keys, and
// takes Pass / Fail and a short note into a file"). The original game has nothing of the kind. Only with the
// command line's --retest <file.md>: without it nothing here is read or written, moho64ui_RetestList() gives nil,
// and the panel, its key action and its console command do not exist.
//
// The list is a Markdown file as the engine chat writes it for a test session: every "## heading" is a group, every
// row of a table under it an item. A table's columns are taken by their names in its first row: "What" or "Item"
// (what to try), "How to see it" or "How" (in a table of two columns: the other column, whatever its name), and
// "Where" (the screens the item belongs to: menu, lobby, game, replay, options, score, comma separated; empty or no
// such column: everywhere). Other columns (Chat, Status ...) are left out; a table without a what-column (the keys'
// sheet "| Key | What it does |") and everything that is no table are skipped. `Backticks` and **bold** lose
// their marks. The file is read at the start and again when its time or size changes; it is never written.
//
// The verdicts go to "<the list's name without .md>.verdicts.md" beside it, written whole at every change (a
// temporary file, read back, then renamed over the old one: as the preferences are, prefs.cpp SavePreferencesOf):
// the list's headings, under each a table "| Verdict | What | Note | When | Build |" with one row per item that has a
// verdict or a note. It is read back at the start, so a session goes on over several runs; a verdict whose row has
// left the list is kept and written back under its heading.
#pragma once
#include <string>

struct lua_State;

namespace moho::ui {

// --retest <file>: the list's file (once, before the first Lua state is made). It and its verdicts file are read.
void SetRetestFile(const std::string& file);
// Whether --retest was given.
bool RetestOn();
// moho64ui_RetestList() -> nil without --retest, else { file, verdictsFile, serial, error, groups = { { title,
//   items = { { id, what, how, where = { "game", ... }, verdict, note, when, build } } } } }: serial goes up with
//   every re-read of the list; error says why the list could not be read (then no groups); verdict is "pass",
//   "fail", "skip" or nil; an item's id is the same for its row across re-reads and runs (a hash of the group's
//   title and the first cell).
// moho64ui_RetestVerdict(id, "pass" | "fail" | "skip" | nil, note) -> true, or false and why: kept and written at
//   once. No verdict with a note keeps the note alone (the file's row says "-"); no verdict and no note takes the
//   row away.
// moho64ui_RetestPanel(window | nil): the panel's window while it is open (its controls get the mouse before a dialog
//   of the game that holds the input: maui/control.h overCapture).
void RegisterRetestBindings(lua_State* L);
// A screen was made (the front end's, a game's): the panel is made again there if it was open
// (/moho64ui/lua/retest.lua Restore). Nothing without --retest.
void RetestScreenMade(lua_State* L);
// The console command remaster_retest [on|off] (the panel's key action): false without --retest (no such command).
bool RetestCommand(lua_State* L, const std::string& words);

}  // namespace moho::ui
