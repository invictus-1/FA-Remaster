// [logsink] Every line the renderer prints goes through here, so that a host can take it into its own log.
//
// The renderer says what it does with printf (stdout) and fprintf(stderr, ...). A program with a console shows those
// lines; a game started from a shortcut has none, and on another machine everything the renderer said would be lost.
// So a host may install a SINK: one function that is handed each whole line the renderer prints, with a level.
//
// - No sink (the viewer, the regression): LogPrintf is vprintf and LogFprintf is vfprintf on the stream given, nothing
//   else: the same bytes to the same streams with the same buffering as before, and the same return values.
// - A sink installed: what is written to stdout (level 1, information) or stderr (level 2, warning) is gathered per
//   thread until its newline (and per stream), and each whole line is handed to the sink without its newline, a
//   leading "render: " cut off (the host puts its own "render:" before it; a line without that lead is handed over as
//   it is). A line built by several printf calls arrives as ONE line. Nothing goes to the real stdout or stderr then.
//   The sink is called on the thread that printed (the surf's field thread, the background loader, the shader
//   compiles' threads and the rest print on their own), so it must be safe to call from any thread at once.
//   A thread that ends with part of a line hands it over as it ends; LogFlush hands the calling thread's part over
//   at once (before a crash report, at the program's end).
// - LogFprintf on any other stream (a file of the renderer's own) always writes to that file, sink or not.
//
// The levels are those of the engine's log (src/core/log.h, moho::LogLevel): 0 debug, 1 info, 2 warning, 3 error.
//
// The routing: this header defines printf( and fprintf( as macros that call the wrapper. Every render/*.cpp and
// render/*.h that prints includes it as the LAST of its includes, so that the macros never rewrite a system or
// third-party header's declarations (render/tools/check_log_print.py finds a file that forgot). Write printf and
// fprintf unqualified: std::printf( does not compile under the macros (it becomes std::::render::LogPrintf), and in a
// file without this header it would print past the wrapper (the check finds both).
#pragma once

#include <cstdarg>
#include <cstdio>

namespace render {

// The levels (as moho::LogLevel).
enum LogPrintLevel : int { kLogDebug = 0, kLogInfo = 1, kLogWarning = 2, kLogError = 3 };

// A host's sink: `line` is one whole line without its newline, valid during the call only.
using LogSink = void (*)(int level, const char* line);

// Installs the sink (nullptr: none, as at the start). Set it before the first device is made, so that the card's report
// is caught; the pointer is atomic all the same. Hands the calling thread's part of a line to the old sink first.
void SetLogSink(LogSink sink);
LogSink GetLogSink();

// printf and fprintf as the renderer's files call them (through the macros below).
int LogPrintf(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
int LogFprintf(FILE* stream, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
int LogVprintf(const char* format, va_list args);
int LogVfprintf(FILE* stream, const char* format, va_list args);

// For new code: one line (or several, split at their newlines) at the level given, its newline added. No sink: to stdout
// (debug, info) or stderr (warning, error).
void LogLine(int level, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Hands the calling thread's part of a line (stdout's and stderr's) to the sink now (nothing without a sink). Safe in
// a crash handler as far as the renderer goes: no lock, no memory taken; a thread with nothing unfinished calls nothing.
// The sink it calls is the host's: one that takes a lock must not wait for it there (the crashing thread may hold it).
void LogFlush();

}  // namespace render

#ifndef RENDER_LOG_PRINT_NO_MACROS
#define printf(...) ::render::LogPrintf(__VA_ARGS__)
#define fprintf(...) ::render::LogFprintf(__VA_ARGS__)
#endif
