// Engine log. Lines are written in the same "level: message" form as the original
// game log so the two can be compared line by line.
#pragma once
#include <string>
#include <string_view>

namespace moho {

enum class LogLevel { Debug, Info, Warning, Error };

void LogOpenFile(const std::string& path);
void LogSetEcho(bool echoToStdout);
void LogWrite(LogLevel level, std::string_view message);
void Logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace moho
