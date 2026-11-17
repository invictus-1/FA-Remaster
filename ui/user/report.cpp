// The report (ours; the report bundle's part (c), Chris's "logs extremely detailed so we don't have to keep coming back
// and forth", 2026-11-14): one console command, remaster_report [a note], writes ONE zip that holds what a bug report
// needs, into the folder "reports" beside the preferences, and says where:
//   README.txt         what is inside, when, our build, the player's note
//   log_now.txt        this run's log so far (its head names the machine; the last 400 interface events written into
//                      it just before, at its end)
//   log_before.txt     the log of the run before this one: the newest older moho64ui_*.log in the same folder, or, for a
//                      log a launcher named with --log (game.log), the <name>_prev<.ext> the launcher kept
//   dump_before.dmp    that run's crash dump when it crashed (log_crash.txt + dump_crash.dmp: the newest older crash
//                      among the kept logs when the run before did not crash): the environment's values, the user's and the PC's names,
//                      the user's name in paths masked with '_' (same length:
//                      the dump stays readable; Chris, 2026-11-15, on what a dump carries: "Yes, masked")
//   LastGame.SCFAReplay  the game being played or last played, as far as it is written (its own format, untouched)
//   game.prefs         the preferences file
// In the text files the user's own folder (C:\Users\<name>, in any case and with either slash) is written
// "<user folder>": a report may be passed around. Nothing is sent anywhere: the file stays on the PC until the player
// passes it on. moho64ui_Report([note]) -> the zip's file | nil, why (the same, for a script; the test uses it).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
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

// The user's own folder in the forms it can stand in a file (the Fable lead's check of 2026-11-17: the first version
// took the folder from getenv alone, the system's ANSI code page, so a folder with a letter outside ASCII, "Jürgen" or
// any name not in Latin letters, was hidden nowhere: the logs are UTF-8 and a dump's paths UTF-16):
//   wide    as the system has it (Windows: _wgetenv; a dump's module paths and the Windows environment block);
//   narrow  UTF-8 as given, UTF-8 in small letters (the engine's display form of a path is in small letters), and the
//           ANSI code page's bytes (what the C runtime's own copy of the environment holds), each once.
// Without a trailing slash; a value shorter than 4 (a root) gives none.
struct HomeForms {
  std::wstring wide;
  std::vector<std::string> narrow;
};

const HomeForms& Home() {
  static const HomeForms forms = [] {
    HomeForms f;
    auto trim = [](auto& s) {
      while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    };
    auto add = [&](std::string s) {
      trim(s);
      if (s.size() < 4) return;
      for (const std::string& have : f.narrow)
        if (have == s) return;
      f.narrow.push_back(std::move(s));
    };
#ifdef _WIN32
    if (const wchar_t* w = _wgetenv(L"USERPROFILE")) f.wide = w;
    trim(f.wide);
    if (f.wide.size() < 4) f.wide.clear();
    auto utf8 = [](const std::wstring& w) {
      std::string s;
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
      if (n > 0) s.resize(static_cast<size_t>(n)), WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
      return s;
    };
    if (!f.wide.empty()) {
      add(utf8(f.wide));
      std::wstring lower = f.wide;
      for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
      add(utf8(lower));
    }
    if (const char* a = std::getenv("USERPROFILE")) add(a);
    // (and the SYSTEM's ANSI code page's bytes, both ways Windows makes them: with its "best fit" letters ("Jürgen" as
    // "Jurgen" on a Japanese system) and without (a '?' for each such letter). This program's own code page is
    // UTF-8, so its getenv gives UTF-8 again (seen 2026-11-17); the system's page is what a file written by another
    // program, read into this one's memory, holds. Asked for by its number: CP_ACP here means UTF-8.)
    if (!f.wide.empty()) {
      UINT page = 0;
      if (GetLocaleInfoW(LOCALE_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&page),
                         sizeof(page) / sizeof(wchar_t)) > 0 && page != 0 && page != CP_UTF8) {
        for (const DWORD flags : {DWORD(0), DWORD(WC_NO_BEST_FIT_CHARS)}) {
          std::string s;
          const int n = WideCharToMultiByte(page, flags, f.wide.c_str(), static_cast<int>(f.wide.size()), nullptr, 0, nullptr, nullptr);
          if (n > 0) s.resize(static_cast<size_t>(n)), WideCharToMultiByte(page, flags, f.wide.c_str(), static_cast<int>(f.wide.size()), s.data(), n, nullptr, nullptr);
          add(std::move(s));
        }
      }
    }
#else
    if (const char* h = std::getenv("HOME")) add(h);
#endif
    return f;
  }();
  return forms;
}

bool SameSlash(unsigned a, unsigned b) { return (a == '\\' || a == '/') && (b == '\\' || b == '/'); }
// (ASCII letters in either case; every other byte as it is)
bool SameByte(unsigned char a, unsigned char b) {
  if (SameSlash(a, b)) return true;
  return a == b || (a < 0x80 && b < 0x80 && std::tolower(a) == std::tolower(b));
}
// Whether what follows a folder's name goes on with that name: a letter, digit, '_' or '-', or a '.' with a letter or
// digit after it ("C:\Users\chris" in "C:\Users\christine" or "C:\Users\chris.old" is another folder; "...\chris."
// at a sentence's end is this one).
bool NameGoesOn(unsigned char next, unsigned char afterNext) {
  if (std::isalnum(next) || next == '_' || next == '-') return true;
  return next == '.' && std::isalnum(afterNext) != 0;
}

// The user's own folder in a text, in each of its narrow forms, ASCII letters in any case and with either slash, made
// "<user folder>".
std::string ShortenUserFolder(const std::string& text) {
  std::string now = text;
  for (const std::string& h : Home().narrow) {
    std::string out;
    out.reserve(now.size());
    for (size_t i = 0; i < now.size();) {
      bool match = i + h.size() <= now.size();
      for (size_t k = 0; match && k < h.size(); ++k) match = SameByte(static_cast<unsigned char>(now[i + k]), static_cast<unsigned char>(h[k]));
      if (match && i + h.size() < now.size()) {
        const size_t n = i + h.size();
        match = !NameGoesOn(static_cast<unsigned char>(now[n]), n + 1 < now.size() ? static_cast<unsigned char>(now[n + 1]) : 0);
      }
      if (match) {
        out += "<user folder>";
        i += h.size();
      } else {
        out.push_back(now[i++]);
      }
    }
    now.swap(out);
  }
  return now;
}

// The user's name masked where the user's folder stands in a binary file (a crash dump; Chris, 2026-11-15: "Yes,
// masked"): wherever the folder occurs, in one of its narrow forms or in UTF-16 at any byte (a string in a dump's
// memory need not lie on an even byte of the file), letters in any case and with either slash, its last part (the
// name) is overwritten with '_' of the same length. Every string keeps its length and place, so the dump stays
// readable by a debugger; "C:\Users\" stays. Returns how many places were masked.
size_t MaskUserNameInPaths(std::string& data) {
  size_t masked = 0;
  for (const std::string& h : Home().narrow) {
    const size_t slash = h.find_last_of("\\/");
    if (slash == std::string::npos || slash + 1 >= h.size()) continue;
    for (size_t i = 0; i + h.size() <= data.size(); ++i) {
      bool match = true;
      for (size_t k = 0; match && k < h.size(); ++k) match = SameByte(static_cast<unsigned char>(data[i + k]), static_cast<unsigned char>(h[k]));
      if (!match) continue;
      const size_t n = i + h.size();
      if (n < data.size() && NameGoesOn(static_cast<unsigned char>(data[n]), n + 1 < data.size() ? static_cast<unsigned char>(data[n + 1]) : 0)) continue;
      for (size_t k = slash + 1; k < h.size(); ++k) data[i + k] = '_';
      ++masked;
      i += h.size() - 1;
    }
  }
  const std::wstring& w = Home().wide;
  const size_t slash = w.find_last_of(L"\\/");
  if (!w.empty() && slash != std::wstring::npos && slash + 1 < w.size()) {
    auto unit = [&](size_t at) -> unsigned { return static_cast<unsigned char>(data[at]) | static_cast<unsigned>(static_cast<unsigned char>(data[at + 1])) << 8; };
    const size_t span = w.size() * 2;
    for (size_t i = 0; i + span <= data.size(); ++i) {
      bool match = true;
      for (size_t k = 0; match && k < w.size(); ++k) {
        const unsigned have = unit(i + 2 * k), want = static_cast<unsigned>(w[k]) & 0xffff;
        match = have == want || SameSlash(have, want) || std::towlower(static_cast<wint_t>(have)) == std::towlower(static_cast<wint_t>(want));
      }
      if (!match) continue;
      if (i + span + 2 <= data.size()) {
        const unsigned next = unit(i + span), after = i + span + 4 <= data.size() ? unit(i + span + 2) : 0;
        if (next < 0x80 && NameGoesOn(static_cast<unsigned char>(next), after < 0x80 ? static_cast<unsigned char>(after) : 0)) continue;
        if (next >= 0x80 && std::iswalnum(static_cast<wint_t>(next))) continue;
      }
      for (size_t k = slash + 1; k < w.size(); ++k) data[i + 2 * k] = '_', data[i + 2 * k + 1] = 0;
      ++masked;
      i += span - 1;
    }
  }
  return masked;
}

// Every place `needle` (bytes) stands in `data` overwritten with '_' (`wide`: the needle is UTF-16, each character's
// two bytes become '_' and 0; it is looked for at every byte). ASCII letters match in either case; only where no
// letter or digit stands right before and after (a whole word). Returns how many places.
size_t MaskWord(std::string& data, const std::string& needle, bool wide) {
  if (needle.empty()) return 0;
  auto word = [](unsigned char c) { return std::isalnum(c) != 0; };
  auto same = [](unsigned char a, unsigned char b) { return a == b || (a < 0x80 && b < 0x80 && std::tolower(a) == std::tolower(b)); };
  const size_t step = wide ? 2 : 1;
  size_t masked = 0;
  for (size_t i = 0; i + needle.size() <= data.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && same(static_cast<unsigned char>(data[i + k]), static_cast<unsigned char>(needle[k]))) ++k;
    if (k < needle.size()) continue;
    if (i >= step && word(static_cast<unsigned char>(data[i - step])) && (!wide || data[i - 1] == 0)) continue;
    const size_t n = i + needle.size();
    if (n + step <= data.size() && word(static_cast<unsigned char>(data[n])) && (!wide || data[n + 1] == 0)) continue;
    for (size_t j = 0; j < needle.size(); j += step) {
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

// A crash dump's memory can hold a piece of the program's environment block (seen 2026-11-15 in a dump of the
// engine's before its gate 170, which took the memory the stacks point at: PATH, SHELL, a password-like entry; often
// cut off where a captured range ends), and an environment may hold anything: folders, the PC's name, a token. So
// wherever, inside the dump's memory, one of this program's environment variables begins an entry ("NAME=", ASCII or
// UTF-16 at any byte, the name in any case, no letter / digit / '_' before it), its value is overwritten with '_' up
// to its NUL or the end of that range (never past it); the name is kept. Our own MOHO64* switches stay (the log names
// them anyway). The crashed run's environment is not known here: this run's, started the same way, stands in for its
// names (INFERRED: the same launcher gives the same names). Then the user's and the computer's names wherever they
// stand alone as a whole word, anywhere in the file: in UTF-16 from 3 letters, in plain bytes from 4 ASCII letters (a
// 3-letter word of plain bytes turns up by chance in a few hundred KB of stack about once in five dumps, and
// overwriting that would change a value a reader of the dump needs; such a name is still masked in every path).
// Returns how many places.
size_t MaskEnvironment(std::string& data) {
  size_t masked = 0;
  std::vector<std::string> names;  // (ASCII names only: Windows' own and every launcher's are)
  std::wstring user, host;
#ifdef _WIN32
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      const std::wstring w(p);
      const size_t eq = w.find(L'=', 1);  // (Windows keeps entries like "=C:=C:\..." whose name begins with '=')
      if (eq == std::wstring::npos) continue;
      std::string name, upper;
      bool ascii = true;
      for (size_t i = 0; i < eq; ++i) {
        if (w[i] >= 0x80) ascii = false;
        name.push_back(static_cast<char>(w[i] & 0x7f));
        upper.push_back(static_cast<char>(std::toupper(w[i] & 0x7f)));
      }
      if (!ascii || name.empty()) continue;
      names.push_back(name);
      if (upper == "USERNAME") user = w.substr(eq + 1);
      if (upper == "COMPUTERNAME") host = w.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
  }
#else
  for (char** e = environ; e && *e; ++e) {
    const std::string entry(*e);
    const size_t eq = entry.find('=', 1);
    if (eq == std::string::npos) continue;
    names.push_back(entry.substr(0, eq));
    const std::string value = entry.substr(eq + 1);
    if (names.back() == "USER") user.assign(value.begin(), value.end());
    if (names.back() == "HOSTNAME") host.assign(value.begin(), value.end());
  }
#endif
  const auto ranges = DumpRanges(data);
  for (const auto& [start, size] : ranges) {
    const size_t end = start + size;
    for (const std::string& name : names) {
      std::string upper = name;
      for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (upper.rfind("MOHO64", 0) == 0) continue;
      // (a name of one or two letters is not looked for: "_=" (the shell's own variable) stands by chance in a few
      // hundred KB of stack two or three times, and each would overwrite what follows it up to a NUL; found by the
      // report's test, 2026-11-17. Windows' own two-letter name is OS, whose value says nothing)
      if (name.size() < 3) continue;
      for (int wide = 0; wide <= 1; ++wide) {
        const size_t step = wide ? 2 : 1, len = (name.size() + 1) * step;  // ("NAME=")
        for (size_t i = start; i + len <= end; ++i) {
          // an entry's start: the range's start, or no letter, digit or '_' right before (a NUL in the block itself; the
          // heap's bytes before the C runtime's own copy of each entry; and "PATH=" inside "EXECPATH=" is not one)
          if (i >= start + step) {
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
  for (const std::wstring& n : {user, host}) {
    if (n.size() < 3) continue;
    std::string utf16, plain;
    bool ascii = true;
    for (wchar_t c : n) {
      utf16.push_back(static_cast<char>(c & 0xff)), utf16.push_back(static_cast<char>((c >> 8) & 0xff));
      if (c >= 0x80) ascii = false;
      plain.push_back(static_cast<char>(c & 0x7f));
    }
    masked += MaskWord(data, utf16, true);
    if (ascii && plain.size() >= 4) masked += MaskWord(data, plain, false);
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

// The two ways a run's log is named, and where the runs before it are (the Fable lead's check, 2026-11-17: the first
// version knew only the first way, so a report made from Chris's launchers held no log before and no crash dump):
//   the default  moho64ui_<date>_<time>.log in the folder "logs" beside the preferences, the newest ten kept: the
//                earlier runs' logs lie beside it, a crashed run's dump beside its log (LogBefore, LastCrashedBefore);
//   a launcher's a name given with --log (Chris's Play-*.bat: game.log). The launcher copies the log to
//                <name>_prev<.ext> before each start, so that is the run before; a crash's dump lies beside the log
//                as <log>.dmp and stays there until the next crash writes over it, whichever run it was.
bool DefaultLogName(const std::filesystem::path& log) {
  return Name(log).rfind("moho64ui_", 0) == 0 && log.extension() == ".log";
}

// A file's time as "2026-11-17 14:05" (the local clock), "" when it cannot be read
std::string FileTime(const std::filesystem::path& file) {
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(file, ec);
  if (ec) return "";
  // (the file clock's time carried over to the system clock by the two clocks' "now": good to a moment, on every system)
  const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  const std::time_t t = std::chrono::system_clock::to_time_t(system);
  std::tm tm = {};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char text[64];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
  return text;
}

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

// The crash dump beside a log, "" when that run did not crash (the engine's crash handler writes it beside the log:
// "<log>.dmp" for a log named by --log, "<log without .log>.dmp" for the default log).
std::filesystem::path DumpOf(const std::filesystem::path& log) {
  std::error_code ec;
  std::string stem = TextOf(log);
  const std::string whole = stem + ".dmp";
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".log") == 0) stem.resize(stem.size() - 4);
  for (const std::string& candidate : {whole, stem + ".dmp"})
    if (std::filesystem::is_regular_file(PathOf(candidate), ec)) return PathOf(candidate);
  return {};
}

// The newest log older than `now` in the same folder whose run crashed (a dump beside it), "" when none: so that a
// report made a start or two after the crash still has it (the logs folder keeps the newest ten).
std::filesystem::path LastCrashedBefore(const std::filesystem::path& now) {
  std::error_code ec;
  std::filesystem::path best;
  std::string bestName;
  for (const auto& e : std::filesystem::directory_iterator(now.parent_path(), ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = Name(e.path());
    if (name.rfind("moho64ui_", 0) != 0 || e.path().extension() != ".log") continue;
    if (std::filesystem::equivalent(e.path(), now, ec)) continue;
    if (name < Name(now) && name > bestName && !DumpOf(e.path()).empty()) bestName = name, best = e.path();
  }
  return best;
}

// A crashed run's log and its dump into the zip as <part>.txt and <part>.dmp, the dump masked; the README's lines
void AddCrashedRun(ZipWriter& zip, const std::filesystem::path& log, const std::filesystem::path& dumpFile,
                   const std::string& logPart, const std::string& dumpPart, const std::string& which, std::string* inside,
                   bool withLog) {
  std::string text;
  if (withLog && ReadWhole(log, &text)) {
    zip.Add(logPart, ShortenUserFolder(text));
    *inside += "- " + logPart + ": the log of " + which + " (" + Name(log) + ")\n";
  }
  std::string dump;
  if (!ReadWhole(dumpFile, &dump)) return;
  // (the environment's values first: a value holding the user's folder must still match whole)
  const size_t environment = MaskEnvironment(dump);
  const size_t masked = MaskUserNameInPaths(dump);
  zip.Add(dumpPart, dump);
  *inside += "- " + dumpPart + ": the crash dump of " + which + " (the threads' stacks" +
             " whole and their registers, a little code round each thread's place, the modules, the fault;\n"
             "  no heap. The engine's dumps before its gate 170 held more: the memory the stacks point at).\n"
             "  Masked with '_', every string keeping its length so the dump stays readable: the values of the\n"
             "  program's environment variables wherever a piece of that block was caught, the user's and the PC's\n"
             "  names (" + std::to_string(environment) + " place(s)), the user's name in paths (" + std::to_string(masked) +
             " place(s)). Reading it needs\n  the moho64ui.pdb of the commit in that log's head.\n";
  Logf(LogLevel::Info, "moho64ui: the report: a crash dump put in (%s), masked in %zu + %zu place(s)", which.c_str(), environment, masked);
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
  if (!log.empty() && DefaultLogName(PathOf(log))) {
    const std::filesystem::path before = LogBefore(PathOf(log));
    if (!before.empty() && ReadWhole(before, &text)) {
      zip.Add("log_before.txt", ShortenUserFolder(text));
      inside += "- log_before.txt: the log of the run before this one (" + Name(before) + ")\n";
    } else {
      inside += "- (no log of a run before this one in the same folder)\n";
    }
    // The newest crash among the kept logs (the run before, or an older one when the player started again in between;
    // the engine's crash window asks for remaster_report at the next start): its dump, and its log when that is not
    // the log before already in the zip.
    const std::filesystem::path crashed = LastCrashedBefore(PathOf(log));
    if (!crashed.empty()) {
      const bool isBefore = !before.empty() && Name(crashed) == Name(before);
      if (isBefore)
        AddCrashedRun(zip, crashed, DumpOf(crashed), "log_before.txt", "dump_before.dmp", "the run before this one, which CRASHED", &inside, false);
      else
        AddCrashedRun(zip, crashed, DumpOf(crashed), "log_crash.txt", "dump_crash.dmp", "the last run that crashed", &inside, true);
    }
  } else if (!log.empty()) {
    // A log named by a launcher: the run before is <name>_prev<.ext>, a crash's dump <log>.dmp beside this log.
    const std::filesystem::path now = PathOf(log);
    const std::filesystem::path before = now.parent_path() / PathOf(Name(now.stem()) + "_prev" + Name(now.extension()));
    bool beforeCrashed = false;
    if (ReadWhole(before, &text)) {
      beforeCrashed = text.find("error: crash: ") != std::string::npos;
      zip.Add("log_before.txt", ShortenUserFolder(text));
      inside += "- log_before.txt: the log of the run before this one (" + Name(before) + ", kept by the launcher)\n";
    } else {
      inside += "- (no log of a run before this one: the launcher kept none as " + Name(before) + ")\n";
    }
    const std::filesystem::path dump = DumpOf(now);
    if (!dump.empty()) {
      const std::string when = FileTime(dump);
      if (beforeCrashed)
        AddCrashedRun(zip, before, dump, "log_before.txt", "dump_before.dmp",
                      "the run before this one, which CRASHED (the dump's file is of " + when + ")", &inside, false);
      else
        AddCrashedRun(zip, before, dump, "log_crash.txt", "dump_crash.dmp",
                      "an earlier run that crashed (the dump's file is of " + when + "; that run's log is no longer kept)", &inside, false);
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
