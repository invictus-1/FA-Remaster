// Engine log. Lines are written in the same "level: message" form as the original
// game log so the two can be compared line by line.
#pragma once
#include <string>
#include <string_view>

namespace moho {

enum class LogLevel { Debug, Info, Warning, Error };

// False when the file could not be made (its folder is missing, no right to write there).
bool LogOpenFile(const std::string& path);
// The log a program with a window writes when no --log names one: `<folder>/<program>_YYYYMMDD_HHMMSS.log` (the
// PC's own time; the folder is made if it is missing), with the build (core/build_stamp.h) as its first line. Of
// that program's logs there only the newest `keep` stay, this one counted: older ones go, each with its crash dump
// (`<the same name>.dmp`, core/crash.h); a file of any other name in the folder is never touched. Returns the
// file's name, "" when it could not be made (then nothing is open, as after a failed LogOpenFile).
std::string LogOpenDefault(const std::string& folder, const std::string& program, int keep = 10);
void LogSetEcho(bool echoToStdout);
void LogWrite(LogLevel level, std::string_view message);
void Logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
// For the crash handler (core/crash.h): one "error: " line to the log file and to stderr, without waiting for the
// log's lock (the crashing thread may hold it), then flushed; and the log file's buffer written out.
void LogEmergency(const char* line);
void LogFlush();

}  // namespace moho
