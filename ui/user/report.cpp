// The report (ours; the report bundle's part (c), Chris's "logs extremely detailed so we don't have to keep coming back
// and forth", 2026-11-14): one console command, remaster_report [a note], writes ONE zip that holds what a bug report
// needs, into the folder "reports" beside the preferences, and says where:
//   README.txt         what is inside, when, our build, the player's note
//   log_now.txt        this run's log so far (its head names the machine; the last 400 interface events written into
//                      it just before, at its end)
//   log_before.txt     the log of the run before this one, when there is one in the same folder
//   LastGame.SCFAReplay  the game being played or last played, as far as it is written (its own format, untouched)
//   game.prefs         the preferences file
// In the text files the user's own folder (C:\Users\<name>, in any case and with either slash) is written
// "<user folder>": a report may be passed around. Nothing is sent anywhere: the file stays on the PC until the player
// passes it on. moho64ui_Report([note]) -> the zip's file | nil, why (the same, for a script; the test uses it).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "zlib.h"

#include "core/log.h"
#include "script/script_state.h"
#include "user/events.h"
#include "user/notices.h"
#include "user/user_state.h"

namespace moho::ui {

extern const char kBuildCommit[];

namespace {

std::filesystem::path PathOf(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string Name(const std::filesystem::path& p) {
  const std::u8string u = p.filename().u8string();
  return std::string(u.begin(), u.end());
}

// (UTF-8, with the system's own slashes)
std::string TextOf(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  std::string s(u.begin(), u.end());
#ifdef _WIN32
  for (char& c : s)
    if (c == '/') c = '\\';
#endif
  return s;
}

bool ReadWhole(const std::filesystem::path& file, std::string* out) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  std::ostringstream all;
  all << f.rdbuf();
  *out = all.str();
  return true;
}

// The user's own folder in a text, in any letter case and with either slash, made "<user folder>". The folder is the
// system's (USERPROFILE on Windows, HOME elsewhere); too short a value (a root) is not used.
std::string ShortenUserFolder(const std::string& text) {
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  std::string h = home ? home : "";
  while (!h.empty() && (h.back() == '\\' || h.back() == '/')) h.pop_back();
  if (h.size() < 4) return text;
  auto same = [](char a, char b) {
    if ((a == '\\' || a == '/') && (b == '\\' || b == '/')) return true;
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    bool match = i + h.size() <= text.size();
    for (size_t k = 0; match && k < h.size(); ++k) match = same(text[i + k], h[k]);
    // (only the whole folder: "C:\Users\chris" in "C:\Users\christine" is not it)
    if (match && i + h.size() < text.size()) {
      const char next = text[i + h.size()];
      match = !(std::isalnum(static_cast<unsigned char>(next)) || next == '_' || next == '-' || next == '.');
    }
    if (match) {
      out += "<user folder>";
      i += h.size();
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

// A zip of whole files in memory (deflated by zlib where that is smaller, else stored), UTF-8 names.
class ZipWriter {
 public:
  ZipWriter() {
    std::time_t now = std::time(nullptr);
    std::tm t = {};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    time_ = static_cast<uint16_t>((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
    date_ = static_cast<uint16_t>(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
  }
  void Add(const std::string& name, const std::string& content) {
    const uint32_t crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size())));
    std::string packed;
    uint16_t method = 0;
    if (!content.empty()) {
      z_stream z = {};
      if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
        packed.resize(deflateBound(&z, static_cast<uLong>(content.size())));
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
        z.avail_in = static_cast<uInt>(content.size());
        z.next_out = reinterpret_cast<Bytef*>(packed.data());
        z.avail_out = static_cast<uInt>(packed.size());
        const int r = deflate(&z, Z_FINISH);
        packed.resize(z.total_out);
        deflateEnd(&z);
        if (r == Z_STREAM_END && packed.size() < content.size()) method = 8;
      }
    }
    const std::string& body = method == 8 ? packed : content;
    const uint32_t offset = static_cast<uint32_t>(data_.size());
    Put32(data_, 0x04034b50);
    Put16(data_, 20), Put16(data_, 0x0800), Put16(data_, method), Put16(data_, time_), Put16(data_, date_);
    Put32(data_, crc), Put32(data_, static_cast<uint32_t>(body.size())), Put32(data_, static_cast<uint32_t>(content.size()));
    Put16(data_, static_cast<uint16_t>(name.size())), Put16(data_, 0);
    data_ += name;
    data_ += body;
    Put32(central_, 0x02014b50);
    Put16(central_, 20), Put16(central_, 20), Put16(central_, 0x0800), Put16(central_, method), Put16(central_, time_), Put16(central_, date_);
    Put32(central_, crc), Put32(central_, static_cast<uint32_t>(body.size())), Put32(central_, static_cast<uint32_t>(content.size()));
    Put16(central_, static_cast<uint16_t>(name.size())), Put16(central_, 0), Put16(central_, 0), Put16(central_, 0), Put16(central_, 0);
    Put32(central_, 0), Put32(central_, offset);
    central_ += name;
    ++count_;
  }
  std::string Finish() const {
    std::string out = data_ + central_;
    Put32(out, 0x06054b50);
    Put16(out, 0), Put16(out, 0), Put16(out, count_), Put16(out, count_);
    Put32(out, static_cast<uint32_t>(central_.size())), Put32(out, static_cast<uint32_t>(data_.size()));
    Put16(out, 0);
    return out;
  }

 private:
  static void Put16(std::string& s, uint16_t v) { s.push_back(char(v & 0xff)), s.push_back(char(v >> 8)); }
  static void Put32(std::string& s, uint32_t v) { Put16(s, uint16_t(v & 0xffff)), Put16(s, uint16_t(v >> 16)); }
  std::string data_, central_;
  uint16_t count_ = 0, time_ = 0, date_ = 0;
};

// The log in the same folder written just before this run's (the newest moho64ui_*.log other than `now`), "" if none
std::filesystem::path LogBefore(const std::filesystem::path& now) {
  std::error_code ec;
  std::filesystem::path best;
  std::string bestName;
  for (const auto& e : std::filesystem::directory_iterator(now.parent_path(), ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = Name(e.path());
    if (name.rfind("moho64ui_", 0) != 0 || e.path().extension() != ".log") continue;
    if (std::filesystem::equivalent(e.path(), now, ec)) continue;
    if (name < Name(now) && name > bestName) bestName = name, best = e.path();
  }
  return best;
}

std::string Stamp(const char* form) {
  std::time_t now = std::time(nullptr);
  std::tm t = {};
#ifdef _WIN32
  localtime_s(&t, &now);
#else
  localtime_r(&now, &t);
#endif
  char text[64];
  std::strftime(text, sizeof(text), form, &t);
  return text;
}

}  // namespace

// The report's zip written; its file, or "" with `why` set.
std::string WriteReport(lua_State* L, const std::string& note, std::string* why) {
  const UserState::Config& cfg = UserState::From(L)->config();
  const std::string log = RunLogFile();
  std::filesystem::path folder;
  if (!cfg.prefsFile.empty()) folder = PathOf(cfg.prefsFile).parent_path() / "reports";
  else if (!log.empty()) folder = PathOf(log).parent_path().parent_path() / "reports";
  if (folder.empty()) return *why = "this run has no preferences file and no log, so no folder to write the report into", "";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::filesystem::path file = folder / ("remaster_report_" + Stamp("%Y-%m-%d_%H-%M-%S") + ".zip");
  for (int n = 2; std::filesystem::exists(file, ec) && n < 100; ++n)
    file = folder / ("remaster_report_" + Stamp("%Y-%m-%d_%H-%M-%S") + "_" + std::to_string(n) + ".zip");

  // (the last events into the log first, then the log to its disk, so that the copy holds them)
  LogLastEvents("the report (remaster_report)", 400);
  Logf(LogLevel::Info, "moho64ui: the report: writing %s", ShortenUserFolder(TextOf(file)).c_str());
  LogFlush();

  ZipWriter zip;
  std::string inside;
  std::string text;
  if (!log.empty() && ReadWhole(PathOf(log), &text)) {
    zip.Add("log_now.txt", ShortenUserFolder(text));
    inside += "- log_now.txt: this run's log so far (its head names the machine; the last 400 interface events are written\n"
              "  into it just before its end)\n";
  } else {
    inside += "- (no log of this run: it writes none)\n";
  }
  if (!log.empty()) {
    const std::filesystem::path before = LogBefore(PathOf(log));
    if (!before.empty() && ReadWhole(before, &text)) {
      zip.Add("log_before.txt", ShortenUserFolder(text));
      inside += "- log_before.txt: the log of the run before this one (" + Name(before) + ")\n";
    } else {
      inside += "- (no log of a run before this one in the same folder)\n";
    }
  }
  const std::string replay = cfg.currentReplay ? cfg.currentReplay() : std::string();
  if (!replay.empty() && ReadWhole(PathOf(replay), &text)) {
    zip.Add("LastGame.SCFAReplay", text);
    inside += "- LastGame.SCFAReplay: the game being played or last played, as far as it is written (the game's own\n"
              "  replay format, as written: it holds the players' names as they were in the game)\n";
  } else {
    inside += "- (no replay: no game played in this run yet, or a game that writes none)\n";
  }
  if (!cfg.prefsFile.empty() && ReadWhole(PathOf(cfg.prefsFile), &text)) {
    zip.Add("game.prefs", ShortenUserFolder(text));
    inside += "- game.prefs: the preferences (the options, the key bindings, the profiles)\n";
  }
  std::string readme = "FA Remastered: a report (the console's remaster_report), written " + Stamp("%Y-%m-%d %H:%M:%S") +
                       ".\nOur build: commit " + std::string(kBuildCommit) + ".\n";
  readme += note.empty() ? std::string("The player's note: none (remaster_report <a note> adds one).\n")
                         : "The player's note: " + note + "\n";
  readme += "\nInside:\n" + inside +
            "\nIn the text files the user's own folder is written <user folder>; nothing else is changed.\n"
            "Nothing was sent anywhere: this file stays on this PC until you pass it on yourself.\n";
  zip.Add("README.txt", readme);

  std::ofstream out(file, std::ios::binary);
  const std::string bytes = zip.Finish();
  if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    *why = "cannot write " + ShortenUserFolder(TextOf(file));
    Logf(LogLevel::Warning, "moho64ui: the report: %s", why->c_str());
    return "";
  }
  out.close();
  Logf(LogLevel::Info, "moho64ui: the report: written, %zu bytes: %s", bytes.size(), ShortenUserFolder(TextOf(file)).c_str());
  NoteEvent("report", "a report written");
  return TextOf(file);
}

namespace {

int l_Report(lua_State* L) {
  const char* note = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
  std::string why;
  const std::string file = WriteReport(L, note ? note : "", &why);
  if (file.empty()) {
    lua_pushnil(L);
    lua_pushstring(L, why.c_str());
    return 2;
  }
  lua_pushstring(L, file.c_str());
  return 1;
}

}  // namespace

void RegisterReportBindings(lua_State* L) { lua_register(L, "moho64ui_Report", l_Report); }

}  // namespace moho::ui
