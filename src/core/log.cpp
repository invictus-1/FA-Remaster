#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <utility>

namespace moho {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_echo = true;
LogObserverScope::Observer g_observer;

const char* Prefix(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug: ";
    case LogLevel::Info: return "info: ";
    case LogLevel::Warning: return "warning: ";
    case LogLevel::Error: return "error: ";
  }
  return "";
}

void VLog(LogLevel level, LogKind kind, const char* format, va_list arguments) {
  char buffer[4096];
  std::vsnprintf(buffer, sizeof buffer, format, arguments);
  LogWrite(level, buffer, kind);
}
}  // namespace

LogObserverScope::LogObserverScope(Observer observer) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_observer = std::move(observer);
}
LogObserverScope::~LogObserverScope() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_observer = nullptr;
}

void LogOpenFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path.c_str(), "wb");
}

void LogSetEcho(bool echo) { g_echo = echo; }

void LogWrite(LogLevel level, std::string_view message, LogKind kind) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_observer) g_observer(level, kind, message);
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
  va_list ap;
  va_start(ap, fmt);
  VLog(level, LogKind::Message, fmt, ap);
  va_end(ap);
}

void LogScriptErrorf(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  VLog(LogLevel::Warning, LogKind::ScriptError, format, arguments);
  va_end(arguments);
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
