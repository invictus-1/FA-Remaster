#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace moho {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_echo = true;

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

void LogOpenFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path.c_str(), "wb");
}

void LogSetEcho(bool echo) { g_echo = echo; }

void LogWrite(LogLevel level, std::string_view message) {
  std::lock_guard<std::mutex> lock(g_mutex);
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

void Logf(LogLevel level, const char* fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  LogWrite(level, buf);
}

}  // namespace moho
