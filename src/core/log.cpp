#include "core/log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "core/build_stamp.h"

namespace moho {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_echo = true;
// LogTallyToFile: the counted messages (never freed: the writer runs at the program's end)
std::map<std::string, int>* g_tally = nullptr;
std::string* g_tallyPath = nullptr;
int g_tallyTotal = 0;
// LogTripwire: the messages this process has written already (log only; never freed)
std::set<std::string>* g_tripped = nullptr;

void TallyMessage(std::string_view message, int times = 1) {  // (the caller holds g_mutex)
  const std::string_view first = message.substr(0, message.find('\n'));
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  auto alnum = [&](char c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  auto hex = [&](char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
  std::string key;
  for (size_t i = 0; i < first.size() && key.size() < 200;) {
    if (!alnum(first[i])) {
      key.push_back(first[i] == '\t' || first[i] == '\r' ? ' ' : first[i]);
      ++i;
      continue;
    }
    // a whole word: '#' when it is 6 or more hex digits with a digit among them (an address, a large number), with
    // or without "0x" before it; any other word as it is ("traceback" holds six hex letters in a row)
    size_t j = i;
    while (j < first.size() && alnum(first[j])) ++j;
    std::string_view word = first.substr(i, j - i);
    std::string_view body = word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X') ? word.substr(2) : word;
    bool allHex = body.size() >= 6, anyDigit = false;
    for (char c : body) {
      allHex = allHex && hex(c);
      anyDigit = anyDigit || digit(c);
    }
    if (allHex && anyDigit) key.push_back('#');
    else key.append(word);
    i = j;
  }
  (*g_tally)[key] += times;
  g_tallyTotal += times;
}

void TallyWrite() {
  std::lock_guard<std::mutex> lock(g_mutex);
  FILE* f = std::fopen(g_tallyPath->c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "total %d\n", g_tallyTotal);
  for (const auto& [message, count] : *g_tally) std::fprintf(f, "%d\t%s\n", count, message.c_str());
  std::fclose(f);
}

const char* Prefix(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug: ";
    case LogLevel::Info: return "info: ";
    case LogLevel::Warning: return "warning: ";
    case LogLevel::Error: return "error: ";
  }
  return "";
}
}  // namespace

bool LogOpenFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path.c_str(), "wb");
  return g_file != nullptr;
}

std::string LogOpenDefault(const std::string& folder, const std::string& program, int keep) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::u8path(folder);
  fs::create_directories(dir, ec);
  // this program's earlier logs there: <program>_YYYYMMDD_HHMMSS.log exactly (the name holds the time, so the
  // names' order is the logs' age)
  const std::string head = program + "_";
  auto ours = [&](const std::string& n) {
    if (n.size() != head.size() + 19 || n.compare(0, head.size(), head) != 0 || n.compare(n.size() - 4, 4, ".log") != 0) return false;
    for (size_t i = 0; i < 15; ++i) {
      const char c = n[head.size() + i];
      if (i == 8 ? c != '_' : (c < '0' || c > '9')) return false;
    }
    return true;
  };
  std::vector<std::string> old;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto u = it->path().filename().u8string();
    std::string n(u.begin(), u.end());
    if (ours(n)) old.push_back(std::move(n));
  }
  std::sort(old.begin(), old.end());
  const size_t stay = keep > 1 ? static_cast<size_t>(keep - 1) : 0;
  for (size_t i = 0; i + stay < old.size(); ++i) {
    fs::remove(dir / fs::u8path(old[i]), ec);
    fs::remove(dir / fs::u8path(old[i].substr(0, old[i].size() - 4) + ".dmp"), ec);
  }
  // (two starts within one second: the later one takes the next free second's name)
  std::string name;
  const std::time_t now = std::time(nullptr);
  for (int k = 0; k < 30; ++k) {
    const std::time_t t = now + k;
    char stamp[32] = "";
    if (const std::tm* tm = std::localtime(&t)) std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", tm);
    name = head + stamp + ".log";
    if (!fs::exists(dir / fs::u8path(name), ec)) break;
  }
  const auto u = (dir / fs::u8path(name)).u8string();
  const std::string path(u.begin(), u.end());
  if (!LogOpenFile(path)) return std::string();
  LogWrite(LogLevel::Info, program + ": build " + BuildStamp());
  return path;
}

void LogSetEcho(bool echo) { g_echo = echo; }

void LogTallyToFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tally) return;
  g_tally = new std::map<std::string, int>();
  g_tallyPath = new std::string(path);
  std::atexit(TallyWrite);
}

namespace {
void WriteLines(LogLevel level, std::string_view message);  // (the caller holds g_mutex)
}

void LogWrite(LogLevel level, std::string_view message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tally && (level == LogLevel::Warning || level == LogLevel::Error)) TallyMessage(message);
  WriteLines(level, message);
}

void LogTripwire(std::string_view message, int times) {
  if (times <= 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_tally) TallyMessage(message, times);
  if (!g_tripped) g_tripped = new std::set<std::string>();
  if (g_tripped->insert(std::string(message)).second) WriteLines(LogLevel::Warning, message);
}

namespace {
void WriteLines(LogLevel level, std::string_view message) {
  const char* prefix = Prefix(level);
  // Every line of a multi-line message carries the level prefix, as in the original log.
  size_t start = 0;
  while (true) {
    size_t nl = message.find('\n', start);
    std::string_view raw = message.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    // The original expands tabs at the start of a line to 8 spaces (later tabs stay).
    std::string expanded;
    size_t tabs = 0;
    while (tabs < raw.size() && raw[tabs] == '\t') ++tabs;
    expanded.assign(tabs * 8, ' ');
    expanded.append(raw.substr(tabs));
    std::string_view line = expanded;
    if (g_echo) {
      std::fputs(prefix, stdout);
      std::fwrite(line.data(), 1, line.size(), stdout);
      std::fputc('\n', stdout);
    }
    if (g_file) {
      std::fputs(prefix, g_file);
      std::fwrite(line.data(), 1, line.size(), g_file);
      std::fputc('\n', g_file);
    }
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
}
}  // namespace

void Logf(LogLevel level, const char* fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  LogWrite(level, buf);
}

void LogEmergency(const char* line) {
  const bool locked = g_mutex.try_lock();
  std::fprintf(stderr, "error: %s\n", line);
  std::fflush(stderr);
  if (g_file) {
    std::fprintf(g_file, "error: %s\n", line);
    std::fflush(g_file);
  }
  if (locked) g_mutex.unlock();
}

void LogFlush() {
  const bool locked = g_mutex.try_lock();
  std::fflush(stdout);
  if (g_file) std::fflush(g_file);
  if (locked) g_mutex.unlock();
}

}  // namespace moho
