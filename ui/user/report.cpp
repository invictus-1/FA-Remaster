// The report (ours; the report bundle's part (c), Chris's "logs extremely detailed so we don't have to keep coming back
// and forth", 2026-11-14): one console command, remaster_report [a note], writes ONE zip that holds what a bug report
// needs, into the folder "reports" beside the preferences, and says where:
//   README.txt         what is inside, when, our build, the player's note
//   log_now.txt        this run's log so far (its head names the machine; the last 400 interface events written into
//                      it just before, at its end)
//   log_before.txt     the log of the run before this one, when there is one in the same folder
//   dump_before.dmp    that run's crash dump when it crashed: the environment's values, the user's and the PC's names,
//                      the user's name in paths masked with '_' (same length:
//                      the dump stays readable; Chris, 2026-11-15, on what a dump carries: "Yes, masked")
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
#include <cstring>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "zlib.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
extern char** environ;
#endif

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

// The user's name masked where the user's folder stands in a binary file (a crash dump; Chris, 2026-11-15: "Yes,
// masked"): wherever the folder occurs, in ASCII or in UTF-16 (the dump's module paths are UTF-16, its stacks hold
// both), in any letter case and with either slash, its last part (the name) is overwritten with '_' of the same
// length. Every string keeps its length and place, so the dump stays readable by a debugger; "C:\Users\" stays.
// Returns how many places were masked.
size_t MaskUserNameInPaths(std::string& data) {
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  std::string h = home ? home : "";
  while (!h.empty() && (h.back() == '\\' || h.back() == '/')) h.pop_back();
  const size_t slash = h.find_last_of("\\/");
  if (h.size() < 4 || slash == std::string::npos || slash + 1 >= h.size()) return 0;
  const size_t nameAt = slash + 1;
  auto same = [](unsigned char a, char b) {
    if ((a == '\\' || a == '/') && (b == '\\' || b == '/')) return true;
    return std::tolower(a) == std::tolower(static_cast<unsigned char>(b));
  };
  auto boundary = [](unsigned char c) { return !(std::isalnum(c) || c == '_' || c == '-' || c == '.'); };
  size_t masked = 0;
  for (int wide = 0; wide <= 1; ++wide) {
    const size_t step = wide ? 2 : 1, span = h.size() * step;
    for (size_t i = 0; i + span <= data.size(); ++i) {
      bool match = true;
      for (size_t k = 0; match && k < h.size(); ++k) {
        match = same(static_cast<unsigned char>(data[i + k * step]), h[k]);
        if (match && wide) match = data[i + k * step + 1] == 0;
      }
      if (!match) continue;
      if (i + span + step <= data.size()) {  // (only the whole folder: "...\chris" in "...\christine" is not it)
        const unsigned char next = static_cast<unsigned char>(data[i + span]);
        if (!boundary(next) && (!wide || data[i + span + 1] == 0)) continue;
      }
      for (size_t k = nameAt; k < h.size(); ++k) data[i + k * step] = '_';
      ++masked;
      i += span - 1;
    }
  }
  return masked;
}

// Every place `needle` (bytes) stands in `data` overwritten from byte `keep` on with '_' (`wide`: the needle is UTF-16,
// each character's two bytes become '_' and 0). `caseless`: ASCII letters match in either case. `whole`: only where
// no letter or digit stands right before and after. Returns how many places.
size_t MaskEvery(std::string& data, const std::string& needle, size_t keep, bool wide, bool caseless, bool whole) {
  if (needle.size() <= keep) return 0;
  auto low = [&](unsigned char c) { return caseless ? static_cast<unsigned char>(std::tolower(c)) : c; };
  auto word = [](unsigned char c) { return std::isalnum(c) != 0; };
  const size_t step = wide ? 2 : 1;
  size_t masked = 0;
  for (size_t i = 0; i + needle.size() <= data.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && low(static_cast<unsigned char>(data[i + k])) == low(static_cast<unsigned char>(needle[k]))) ++k;
    if (k < needle.size()) continue;
    if (whole) {
      if (i >= step && word(static_cast<unsigned char>(data[i - step]))) continue;
      if (i + needle.size() < data.size() && word(static_cast<unsigned char>(data[i + needle.size()]))) continue;
    }
    for (size_t j = keep; j < needle.size(); j += step) {
      data[i + j] = '_';
      if (wide) data[i + j + 1] = 0;
    }
    ++masked;
    i += needle.size() - 1;
  }
  return masked;
}

// The raw memory a minidump holds, as (file offset, size) ranges: each thread's stack (ThreadList, stream 3) and the
// memory near the stacks (MemoryList, stream 5). Empty when the file is not a dump this reads.
std::vector<std::pair<size_t, size_t>> DumpRanges(const std::string& d) {
  std::vector<std::pair<size_t, size_t>> out;
  auto u32 = [&](size_t at) -> uint32_t {
    if (at + 4 > d.size()) return 0;
    return uint32_t(uint8_t(d[at])) | uint32_t(uint8_t(d[at + 1])) << 8 | uint32_t(uint8_t(d[at + 2])) << 16 | uint32_t(uint8_t(d[at + 3])) << 24;
  };
  if (d.size() < 32 || d.compare(0, 4, "MDMP") != 0) return out;
  const uint32_t streams = u32(8), dir = u32(12);
  auto add = [&](uint32_t size, uint32_t rva) {
    if (size && size_t(rva) + size <= d.size()) out.emplace_back(rva, size);
  };
  for (uint32_t i = 0; i < streams && i < 256; ++i) {
    const uint32_t type = u32(dir + 12 * i), rva = u32(dir + 12 * i + 8);
    if (type == 3)  // MINIDUMP_THREAD: 48 bytes, its Stack descriptor (start u64, size u32, rva u32) at +24
      for (uint32_t n = u32(rva), k = 0; k < n && k < 4096; ++k) add(u32(rva + 4 + 48 * k + 32), u32(rva + 4 + 48 * k + 36));
    if (type == 5)  // MINIDUMP_MEMORY_DESCRIPTOR: 16 bytes (start u64, size u32, rva u32)
      for (uint32_t n = u32(rva), k = 0; k < n && k < 65536; ++k) add(u32(rva + 4 + 16 * k + 8), u32(rva + 4 + 16 * k + 12));
  }
  return out;
}

// A crash dump's memory near the stacks can hold a piece of the program's environment block (seen 2026-11-15 in the
// crash test's dump: PATH, SHELL, a trace id; often cut off where a captured range ends), and an environment may hold
// anything: folders, the PC's name, a token. So wherever, inside the dump's memory, one of this program's environment
// variables begins an entry ("NAME=", ASCII or UTF-16, the name in any case, no letter / digit / '_' before it), its
// value is overwritten with '_' up to its NUL or the end of that range (never past it); the name is kept. Our own
// MOHO64* switches stay (the log names them anyway). The crashed run's environment is not known here: this run's,
// started the same way, stands in for its names (INFERRED: the same launcher gives the same names). Then the user's
// and the computer's names wherever they stand alone (whole words of 3 letters or more). Returns how many places.
size_t MaskEnvironment(std::string& data) {
  size_t masked = 0;
  std::vector<std::string> names;
  std::string user, host;
#ifdef _WIN32
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      const std::wstring w(p);
      const size_t eq = w.find(L'=', 1);  // (Windows keeps entries like "=C:=C:\..." whose name begins with '=')
      if (eq == std::wstring::npos) continue;
      std::string name, value;
      for (size_t i = 0; i < w.size(); ++i) (i < eq ? name : value).push_back(i == eq ? '\0' : static_cast<char>(w[i] & 0x7f));
      if (!value.empty() && value[0] == '\0') value.erase(0, 1);
      names.push_back(name);
      std::string upper = name;
      for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (upper == "USERNAME") user = value;
      if (upper == "COMPUTERNAME") host = value;
    }
    FreeEnvironmentStringsW(block);
  }
#else
  for (char** e = environ; e && *e; ++e) {
    const std::string entry(*e);
    const size_t eq = entry.find('=', 1);
    if (eq == std::string::npos) continue;
    names.push_back(entry.substr(0, eq));
    if (names.back() == "USER") user = entry.substr(eq + 1);
    if (names.back() == "HOSTNAME") host = entry.substr(eq + 1);
  }
#endif
  const auto ranges = DumpRanges(data);
  for (const auto& [start, size] : ranges) {
    const size_t end = start + size;
    for (const std::string& name : names) {
      std::string upper = name;
      for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (name.empty() || upper.rfind("MOHO64", 0) == 0) continue;
      for (int wide = 0; wide <= 1; ++wide) {
        const size_t step = wide ? 2 : 1, len = (name.size() + 1) * step;  // ("NAME=")
        for (size_t i = start; i + len <= end; i += step) {
          // an entry's start: the range's start, or no letter, digit or '_' right before (a NUL in the block itself; the
          // heap's bytes before the C runtime's own copy of each entry; and "PATH=" inside "EXECPATH=" is not one)
          if (i > start) {
            const unsigned char before = static_cast<unsigned char>(data[i - step]);
            if ((std::isalnum(before) || before == '_') && (!wide || data[i - 1] == 0)) continue;
          }
          bool match = true;
          for (size_t k = 0; match && k <= name.size(); ++k) {
            const char want = k < name.size() ? name[k] : '=';
            match = std::toupper(static_cast<unsigned char>(data[i + k * step])) == std::toupper(static_cast<unsigned char>(want)) &&
                    (!wide || data[i + k * step + 1] == 0);
          }
          if (!match) continue;
          size_t j = i + len;
          bool any = false;
          for (; j + step <= end; j += step) {
            if (data[j] == 0 && (!wide || data[j + 1] == 0)) break;
            data[j] = '_';
            if (wide) data[j + 1] = 0;
            any = true;
          }
          if (any) ++masked;
          i = j;
        }
      }
    }
  }
  for (const std::string& n : {user, host}) {
    if (n.size() < 3) continue;
    std::string utf16;
    for (char c : n) utf16.push_back(c), utf16.push_back(0);
    masked += MaskEvery(data, n, 0, false, true, true);
    masked += MaskEvery(data, utf16, 0, true, true, true);
  }
  return masked;
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
      // That run's crash dump, when it crashed (the engine's crash handler writes it beside the log: "<log>.dmp" for
      // a log named by --log, "<log without .log>.dmp" for the default log), the user's name masked in its paths.
      std::string stem = TextOf(before);
      if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".log") == 0) stem.resize(stem.size() - 4);
      for (const std::string& candidate : {TextOf(before) + ".dmp", stem + ".dmp"}) {
        std::string dump;
        if (!ReadWhole(PathOf(candidate), &dump)) continue;
        // (the environment's values first: a value holding the user's folder must still match whole)
        const size_t environment = MaskEnvironment(dump);
        const size_t masked = MaskUserNameInPaths(dump);
        zip.Add("dump_before.dmp", dump);
        inside += "- dump_before.dmp: that run CRASHED: its crash dump (the threads' stacks and the memory near them, the\n"
                  "  modules, the fault and its registers). Masked with '_', every string keeping its length so the\n"
                  "  dump stays readable: the values of the program's environment variables where a piece of that\n"
                  "  block was caught, and the user's and the PC's names (" + std::to_string(environment) + " place(s)), the user's\n"
                  "  name in paths (" + std::to_string(masked) + " place(s)). Reading it needs the moho64ui.pdb of the commit in\n"
                  "  log_before's head.\n";
        Logf(LogLevel::Info, "moho64ui: the report: the run before crashed: its dump put in, masked in %zu + %zu place(s)", environment, masked);
        break;
      }
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
