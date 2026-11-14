// The re-test list and its verdicts: see retest.h. Ours, not the original's.
#include "user/retest.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "core/build_stamp.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

struct Item {
  std::string id, what, how;
  std::vector<std::string> where;  // of kScreens; empty: everywhere
};
struct Group {
  std::string title;
  std::vector<Item> items;
};
// What the player said of one item. `group` and `what` are kept with it, so that a verdict whose row has left the
// list is still written back under its heading.
struct Verdict {
  std::string group, what;
  std::string verdict;  // "pass", "fail", "skip", or "" (a note alone)
  std::string note, when, build;
  int order = 0;  // the order they came in (for the rows that are no longer in the list)
};

const char* const kScreens[] = {"menu", "lobby", "game", "replay", "options", "score"};

std::mutex g_lock;  // (a session's Lua state is made on the loading thread: one store for all states, as notices.cpp)
bool g_on = false;
std::string g_file, g_verdictsFile;
std::string g_error;          // why the list could not be read ("" when it was)
std::vector<Group> g_groups;  // the list as last read
int64_t g_time = -1;          // its file's time and size then (-1: not there)
uint64_t g_size = 0;
int g_serial = 0;
std::map<std::string, Verdict> g_verdicts;  // by an item's id
int g_order = 0;

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t at = 0;
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) at = 3;  // (a UTF-8 mark at the file's start)
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    lines.push_back(Trim(text.substr(at, end - at)));
    at = end + 1;
  }
  return lines;
}

// A cell's text as plain text: `backticks` and **bold** without their marks.
std::string Plain(const std::string& cell) {
  std::string out;
  for (size_t i = 0; i < cell.size(); ++i) {
    if (cell[i] == '`') continue;
    if (cell[i] == '*' && i + 1 < cell.size() && cell[i + 1] == '*') {
      ++i;
      continue;
    }
    out.push_back(cell[i] == '\t' ? ' ' : cell[i]);  // (a tab is a blank: the verdicts file's cells have none)
  }
  return Trim(out);
}

// A table row's cells ("| a | b |"; "\|" is a bar inside a cell), each trimmed, marks kept.
std::vector<std::string> Cells(const std::string& line) {
  std::vector<std::string> cells;
  std::string cell;
  size_t i = line.empty() || line[0] != '|' ? 0 : 1;
  bool open = false;  // (text after the last bar: a row without its closing bar)
  for (; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
      cell.push_back('|');
      ++i;
      open = true;
    } else if (line[i] == '|') {
      cells.push_back(Trim(cell));
      cell.clear();
      open = false;
    } else {
      cell.push_back(line[i]);
      if (line[i] != ' ' && line[i] != '\t') open = true;
    }
  }
  if (open) cells.push_back(Trim(cell));
  return cells;
}

// The line between a table's first row and its others: every cell dashes, with or without colons ("|---|:--:|").
bool IsRule(const std::string& line) {
  const std::vector<std::string> cells = Cells(line);
  if (cells.empty()) return false;
  for (const std::string& c : cells) {
    if (c.find('-') == std::string::npos) return false;
    if (c.find_first_not_of("-:") != std::string::npos) return false;
  }
  return true;
}

// A group's heading: "## title" (two marks, no more: "#" is the file's title, "###" a part of a group).
bool IsHeading(const std::string& line, std::string* title) {
  if (line.compare(0, 2, "##") != 0 || line.size() < 3 || (line[2] != ' ' && line[2] != '\t')) return false;
  *title = Plain(line.substr(3));
  return true;
}

// A cell for the verdicts file: one line, its bars written "\|".
std::string CellText(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '|') out += "\\|";
    else if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
    else out.push_back(c);
  }
  return Trim(out);
}

// An item's id: FNV-1a (64 bits) over the group's title, a byte that is in neither, and the first cell, both as
// plain text; sixteen hex digits. The same for a row as long as its heading and its first cell stay as they are.
std::string IdOf(const std::string& group, const std::string& what) {
  uint64_t h = 14695981039346656037ULL;
  auto feed = [&h](const std::string& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ULL;
    }
  };
  feed(group);
  h ^= 0x1f;
  h *= 1099511628211ULL;
  feed(what);
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  return hex;
}

int Column(const std::vector<std::string>& head, std::initializer_list<const char*> names) {
  for (size_t i = 0; i < head.size(); ++i) {
    const std::string name = Lower(Plain(head[i]));
    for (const char* want : names)
      if (name == want) return static_cast<int>(i);
  }
  return -1;
}

std::string CellAt(const std::vector<std::string>& cells, int column) {
  return column >= 0 && column < static_cast<int>(cells.size()) ? Plain(cells[column]) : std::string();
}

// The words of Where cells that are no screen's name, each once (a list read): said in ONE line when the list is read.
// (Until 2026-11-13 each had a warning line of its own: Chris's list of that day, whose Where cells say where on the
// map to look, put some three hundred lines at the head of every log.)
std::mutex g_strangersLock;
std::vector<std::string> g_strangers;

// The screens a Where cell names (separated by commas or blanks; a word that is no screen's name is left out: an item
// whose cell names no screen is shown on every screen).
std::vector<std::string> Screens(const std::string& cell) {
  std::vector<std::string> out;
  std::string word;
  auto take = [&]() {
    word = Lower(Trim(word));
    if (!word.empty()) {
      const bool known = std::find_if(std::begin(kScreens), std::end(kScreens), [&](const char* s) { return word == s; }) != std::end(kScreens);
      if (known && std::find(out.begin(), out.end(), word) == out.end()) out.push_back(word);
      std::lock_guard<std::mutex> hold(g_strangersLock);
      if (!known && std::find(g_strangers.begin(), g_strangers.end(), word) == g_strangers.end()) g_strangers.push_back(word);
    }
    word.clear();
  };
  for (char c : cell) {
    if (c == ',' || c == ';' || c == '/' || c == ' ') take();
    else word.push_back(c);
  }
  take();
  return out;
}

// Every table of a Markdown text: its lines' range [from, to) with the heading it stands under; `rows` gets, for a
// table with a first row and a rule under it, that first row's cells and the lines after the rule.
struct Table {
  std::string group;
  std::vector<std::string> head;
  std::vector<std::vector<std::string>> rows;
};
std::vector<Table> Tables(const std::string& text) {
  std::vector<Table> tables;
  const std::vector<std::string> lines = Lines(text);
  std::string group;
  for (size_t i = 0; i < lines.size();) {
    std::string title;
    if (IsHeading(lines[i], &title)) {
      group = title;
      ++i;
      continue;
    }
    if (lines[i].empty() || lines[i][0] != '|') {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < lines.size() && !lines[end].empty() && lines[end][0] == '|') ++end;
    if (end - i >= 2 && IsRule(lines[i + 1])) {
      Table t;
      t.group = group;
      t.head = Cells(lines[i]);
      for (size_t r = i + 2; r < end; ++r)
        if (!IsRule(lines[r])) t.rows.push_back(Cells(lines[r]));
      tables.push_back(std::move(t));
    }
    i = end;
  }
  return tables;
}

// The list's groups from its text. A heading without an item under it makes no group; rows before the first
// heading are a group without a title.
std::vector<Group> ParseList(const std::string& text) {
  std::vector<Group> groups;
  std::set<std::string> taken;  // group's title + what, as shown (the same first cell twice in a group)
  for (const Table& t : Tables(text)) {
    const int what = Column(t.head, {"what", "item"});
    if (what < 0) continue;  // (another table: the keys' sheet, a table of numbers)
    int how = Column(t.head, {"how to see it", "how"});
    const int where = Column(t.head, {"where"});
    // (a table of two columns: the other one says how, whatever its name; the real lists have "| What | Look for |")
    if (how < 0 && t.head.size() == 2) how = 1 - what;
    for (const std::vector<std::string>& cells : t.rows) {
      Item item;
      item.what = CellAt(cells, what);
      if (item.what.empty()) continue;
      item.how = CellAt(cells, how);
      item.where = Screens(CellAt(cells, where));
      // (the second row with the same first cell under one heading is "<what> (2)": each has its own id, and the
      // verdicts file's What cell, which is all a start reads the id from, names it the same way)
      const std::string first = item.what;
      for (int n = 2; !taken.insert(t.group + '\x1f' + item.what).second; ++n) item.what = first + " (" + std::to_string(n) + ")";
      item.id = IdOf(t.group, item.what);
      // (two tables under one heading, or the same heading twice: one group)
      auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.title == t.group; });
      if (it == groups.end()) {
        groups.push_back({t.group, {}});
        it = groups.end() - 1;
      }
      it->items.push_back(std::move(item));
    }
  }
  return groups;
}

// The verdicts file read back (a start): its rows by the id their heading and What cell give.
void ParseVerdicts(const std::string& text) {
  for (const Table& t : Tables(text)) {
    const int verdict = Column(t.head, {"verdict"});
    const int what = Column(t.head, {"what"});
    if (verdict < 0 || what < 0) continue;
    const int note = Column(t.head, {"note"}), when = Column(t.head, {"when"}), build = Column(t.head, {"build"});
    for (const std::vector<std::string>& cells : t.rows) {
      Verdict v;
      v.group = t.group;
      // (the cells as they were written: plain already; a note's own marks are the player's and stay)
      v.what = what < static_cast<int>(cells.size()) ? cells[what] : std::string();
      if (v.what.empty()) continue;
      const std::string word = Lower(CellAt(cells, verdict));
      v.verdict = word == "pass" || word == "fail" || word == "skip" ? word : std::string();
      if (note >= 0 && note < static_cast<int>(cells.size())) v.note = cells[note];
      if (when >= 0 && when < static_cast<int>(cells.size())) v.when = cells[when];
      if (build >= 0 && build < static_cast<int>(cells.size())) v.build = cells[build];
      if (v.verdict.empty() && v.note.empty()) continue;
      v.order = ++g_order;
      g_verdicts[IdOf(v.group, v.what)] = std::move(v);
    }
  }
}

// The list read (again) when its file's time or size is not what it was. g_lock held.
void ReadListIfChanged() {
  const auto stat = hostfs::Stat(g_file);
  const int64_t time = stat && !stat->isFolder ? stat->writeTime : -1;
  const uint64_t size = stat && !stat->isFolder ? stat->size : 0;
  if (g_serial > 0 && time == g_time && size == g_size) return;
  g_time = time;
  g_size = size;
  ++g_serial;
  g_groups.clear();
  g_error.clear();
  const auto text = time < 0 ? std::nullopt : hostfs::ReadFile(g_file);
  if (!text) {
    g_error = "the list " + g_file + " could not be read";
    Logf(LogLevel::Warning, "moho64ui: the re-test list: %s", g_error.c_str());
    return;
  }
  {
    std::lock_guard<std::mutex> hold(g_strangersLock);
    g_strangers.clear();
  }
  g_groups = ParseList(*text);
  {
    std::lock_guard<std::mutex> hold(g_strangersLock);
    if (!g_strangers.empty()) {
      std::string first;
      for (size_t i = 0; i < g_strangers.size() && i < 5; ++i) first += (i ? ", " : "") + ("\"" + g_strangers[i] + "\"");
      Logf(LogLevel::Info,
           "moho64ui: the re-test list: %zu word(s) in Where cells are no screen (menu, lobby, game, replay, options, score) and are left out "
           "(the first: %s); an item whose cell names no screen is shown on every screen",
           g_strangers.size(), first.c_str());
    }
  }
  size_t items = 0;
  for (const Group& g : g_groups) items += g.items.size();
  Logf(LogLevel::Info, "moho64ui: the re-test list %s: %zu items in %zu groups (read %d)", g_file.c_str(), items, g_groups.size(), g_serial);
}

void AddRow(std::string& out, const Verdict& v) {
  const char* word = v.verdict == "pass" ? "Pass" : v.verdict == "fail" ? "Fail" : v.verdict == "skip" ? "Skip" : "-";
  out += std::string("| ") + word + " | " + CellText(v.what) + " | " + CellText(v.note) + " | " + CellText(v.when) + " | " + CellText(v.build) + " |\n";
}

// The verdicts file written whole: never in place (a temporary file, read back, then renamed over the file before,
// as SavePreferencesOf writes the preferences). g_lock held. False and why when it could not be written.
bool WriteVerdicts(std::string* why) {
  const char* const head = "| Verdict | What | Note | When | Build |\n|---|---|---|---|---|\n";
  const size_t slash = g_file.find_last_of("/\\");
  std::string out = "# Verdicts: " + (slash == std::string::npos ? g_file : g_file.substr(slash + 1)) + "\n\n";
  out += "Written by FA Remastered's re-test panel (moho64ui --retest): one row for every item of the list that has a\n"
         "verdict or a note. The panel reads this file back at its next start.\n";
  std::map<std::string, bool> written;  // ids
  std::vector<const Verdict*> rest;     // rows whose item is not in the list (any more), in the order they came
  for (const auto& [id, v] : g_verdicts) rest.push_back(&v);
  auto section = [&](const std::string& title, const std::vector<const Verdict*>& rows) {
    if (rows.empty()) return;
    out += "\n";
    if (!title.empty()) out += "## " + title + "\n";
    out += head;
    for (const Verdict* v : rows) AddRow(out, *v);
  };
  std::vector<std::string> titles;
  for (const Group& g : g_groups) {
    std::vector<const Verdict*> rows;
    for (const Item& item : g.items) {
      auto it = g_verdicts.find(item.id);
      if (it == g_verdicts.end()) continue;
      rows.push_back(&it->second);
      written[item.id] = true;
    }
    titles.push_back(g.title);
    // (with them the group's rows that have left the list)
    std::vector<const Verdict*> gone;
    for (const auto& [id, v] : g_verdicts)
      if (!written.count(id) && v.group == g.title && std::none_of(g.items.begin(), g.items.end(), [&](const Item& i) { return i.id == id; }))
        gone.push_back(&v);
    std::sort(gone.begin(), gone.end(), [](const Verdict* a, const Verdict* b) { return a->order < b->order; });
    for (const Verdict* v : gone) {
      rows.push_back(v);
      written[IdOf(v->group, v->what)] = true;
    }
    section(g.title, rows);
  }
  // the rows of headings the list does not have (any more), heading by heading in the order they came
  rest.erase(std::remove_if(rest.begin(), rest.end(), [&](const Verdict* v) { return written.count(IdOf(v->group, v->what)) != 0; }), rest.end());
  std::sort(rest.begin(), rest.end(), [](const Verdict* a, const Verdict* b) { return a->order < b->order; });
  while (!rest.empty()) {
    const std::string title = rest.front()->group;
    std::vector<const Verdict*> rows;
    for (const Verdict* v : rest)
      if (v->group == title) rows.push_back(v);
    rest.erase(std::remove_if(rest.begin(), rest.end(), [&](const Verdict* v) { return v->group == title; }), rest.end());
    section(title, rows);
  }
  const std::string tmp = g_verdictsFile + ".tmp";
  const auto back = hostfs::WriteFile(tmp, out) ? hostfs::ReadFile(tmp) : std::nullopt;
  if (!back || *back != out) {
    *why = tmp + " could not be written whole (the file before is kept)";
    return false;
  }
  if (hostfs::RenameOver(tmp, g_verdictsFile)) return true;
  // (the name could not be changed, a file held open by another program: written in place, so that no verdict is lost)
  if (hostfs::WriteFile(g_verdictsFile, out)) return true;
  *why = g_verdictsFile + " could not be written";
  return false;
}

// The PC's clock, as the file's When cell shows it.
std::string Now() {
  const std::time_t t = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &local);
  return stamp;
}

void SetString(lua_State* L, const char* key, const std::string& value) {
  lua_pushstring(L, key);
  lua_pushlstring(L, value.data(), value.size());
  lua_rawset(L, -3);
}

int l_RetestList(lua_State* L) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (!g_on) return lua_pushnil(L), 1;
  ReadListIfChanged();
  lua_newtable(L);
  SetString(L, "file", g_file);
  SetString(L, "verdictsFile", g_verdictsFile);
  lua_pushstring(L, "serial");
  lua_pushnumber(L, g_serial);
  lua_rawset(L, -3);
  if (!g_error.empty()) SetString(L, "error", g_error);
  lua_pushstring(L, "groups");
  lua_newtable(L);
  for (size_t g = 0; g < g_groups.size(); ++g) {
    lua_newtable(L);
    SetString(L, "title", g_groups[g].title);
    lua_pushstring(L, "items");
    lua_newtable(L);
    for (size_t i = 0; i < g_groups[g].items.size(); ++i) {
      const Item& item = g_groups[g].items[i];
      lua_newtable(L);
      SetString(L, "id", item.id);
      SetString(L, "what", item.what);
      SetString(L, "how", item.how);
      lua_pushstring(L, "where");
      lua_newtable(L);
      for (size_t w = 0; w < item.where.size(); ++w) {
        lua_pushstring(L, item.where[w].c_str());
        lua_rawseti(L, -2, static_cast<int>(w) + 1);
      }
      lua_rawset(L, -3);
      auto it = g_verdicts.find(item.id);
      if (it != g_verdicts.end()) {
        if (!it->second.verdict.empty()) SetString(L, "verdict", it->second.verdict);
        SetString(L, "note", it->second.note);
        SetString(L, "when", it->second.when);
        SetString(L, "build", it->second.build);
      } else {
        SetString(L, "note", "");
      }
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_rawset(L, -3);
    lua_rawseti(L, -2, static_cast<int>(g) + 1);
  }
  lua_rawset(L, -3);
  return 1;
}

int l_RetestVerdict(lua_State* L) {
  auto refuse = [L](const std::string& why) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why.c_str());
    return 2;
  };
  std::lock_guard<std::mutex> hold(g_lock);
  if (!g_on) return refuse("no re-test list (the program was started without --retest)");
  if (lua_type(L, 1) != LUA_TSTRING) return refuse("the first argument is an item's id");
  const std::string id = lua_tostring(L, 1);
  std::string verdict;
  if (!lua_isnoneornil(L, 2)) {
    verdict = Lower(lua_isstring(L, 2) ? lua_tostring(L, 2) : "");
    if (verdict != "pass" && verdict != "fail" && verdict != "skip") return refuse("the verdict is \"pass\", \"fail\", \"skip\" or nil");
  }
  const std::string note = Trim(lua_isstring(L, 3) ? lua_tostring(L, 3) : "");
  ReadListIfChanged();
  const Item* item = nullptr;
  const Group* group = nullptr;
  for (const Group& g : g_groups)
    for (const Item& i : g.items)
      if (i.id == id) item = &i, group = &g;
  auto kept = g_verdicts.find(id);
  if (!item && kept == g_verdicts.end()) return refuse("no item of the list has the id " + id);
  const std::map<std::string, Verdict> before = g_verdicts;
  if (verdict.empty() && note.empty()) {
    if (kept == g_verdicts.end()) return lua_pushboolean(L, 1), 1;  // (nothing was kept, nothing to write)
    g_verdicts.erase(kept);
  } else {
    Verdict& v = g_verdicts[id];
    if (item) v.group = group->title, v.what = item->what;
    if (!v.order) v.order = ++g_order;
    if (v.verdict == verdict && v.note == note && !v.when.empty()) return lua_pushboolean(L, 1), 1;  // (said again: the first time stays)
    v.verdict = verdict;
    v.note = note;
    v.when = Now();
    v.build = BuildStamp();
  }
  std::string why;
  if (!WriteVerdicts(&why)) {
    g_verdicts = before;  // (what the file holds)
    Logf(LogLevel::Warning, "moho64ui: the re-test verdict was NOT saved: %s", why.c_str());
    return refuse(why);
  }
  lua_pushboolean(L, 1);
  return 1;
}

// moho64ui_RetestPanel(window | nil): the panel's window while it is open: its controls get the mouse before a dialog
// that holds the input (maui/control.h overCapture; without it the panel could not be clicked beside the Options
// dialog or over the score screen); nil when it closes. Nothing without --retest.
int l_RetestPanel(lua_State* L) {
  {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_on) return 0;
  }
  UserState::From(L)->ui().overCapture = lua_isnoneornil(L, 1) ? nullptr : CheckControl(L, 1);
  return 0;
}

// import("/moho64ui/lua/retest.lua")[function](argument): the panel's script (ui/data/lua/retest.lua).
void CallPanel(lua_State* L, const char* function, const std::string& argument) {
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/moho64ui/lua/retest.lua");
  if (lua_pcall(L, 1, 1, top + 1) != 0) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  } else if (lua_istable(L, -1)) {
    lua_pushstring(L, function);
    lua_gettable(L, -2);
    lua_pushstring(L, argument.c_str());
    if (lua_isfunction(L, -2) && lua_pcall(L, 1, 0, top + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  }
  lua_settop(L, top);
}

}  // namespace

void SetRetestFile(const std::string& file) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (file.empty()) return;
  g_on = true;
  g_file = hostfs::CleanPath(file);
  // "<name>.md" -> "<name>.verdicts.md" (another ending: ".verdicts.md" after the whole name)
  const bool md = g_file.size() > 3 && Lower(g_file.substr(g_file.size() - 3)) == ".md";
  g_verdictsFile = (md ? g_file.substr(0, g_file.size() - 3) : g_file) + ".verdicts.md";
  ReadListIfChanged();
  if (const auto text = hostfs::ReadFile(g_verdictsFile)) {
    ParseVerdicts(*text);
    Logf(LogLevel::Info, "moho64ui: the re-test verdicts %s: %zu read back", g_verdictsFile.c_str(), g_verdicts.size());
  }
}

bool RetestOn() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_on;
}

void RegisterRetestBindings(lua_State* L) {
  SetGlobal(L, "moho64ui_RetestList", l_RetestList);
  SetGlobal(L, "moho64ui_RetestVerdict", l_RetestVerdict);
  SetGlobal(L, "moho64ui_RetestPanel", l_RetestPanel);
}

void RetestScreenMade(lua_State* L) {
  if (RetestOn()) CallPanel(L, "Restore", "");
}

bool RetestCommand(lua_State* L, const std::string& words) {
  if (!RetestOn()) return false;
  CallPanel(L, "Command", words);
  return true;
}

}  // namespace moho::ui
