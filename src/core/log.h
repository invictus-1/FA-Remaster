// Engine log. Lines are written in the same "level: message" form as the original
// game log so the two can be compared line by line.
#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace moho {

enum class LogLevel { Debug, Info, Warning, Error };
enum class LogKind { Message, ScriptError };

// Observers run under the log lock and must not log or access Lua states.
class LogObserverScope {
 public:
  using Observer = std::function<void(LogLevel, LogKind, std::string_view)>;
  explicit LogObserverScope(Observer observer);
  ~LogObserverScope();
  LogObserverScope(const LogObserverScope&) = delete;
  LogObserverScope& operator=(const LogObserverScope&) = delete;
};

void LogOpenFile(const std::string& path);
void LogSetEcho(bool echoToStdout);
void LogWrite(LogLevel level, std::string_view message, LogKind kind = LogKind::Message);
void Logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void LogScriptErrorf(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
// For the crash handler (core/crash.h): one "error: " line to the log file and to stderr, without waiting for the
// log's lock (the crashing thread may hold it), then flushed; and the log file's buffer written out.
void LogEmergency(const char* line);
void LogFlush();

}  // namespace moho
