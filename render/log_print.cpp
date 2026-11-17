// [logsink] The wrapper every print of the renderer goes through (log_print.h). This file calls the real vprintf and
// vfprintf, and so must not see the header's macros.
#define RENDER_LOG_PRINT_NO_MACROS 1
#include "log_print.h"

#include <atomic>
#include <cstring>
#include <string>

namespace render {
namespace {

std::atomic<LogSink> gSink{nullptr};

constexpr const char kLead[] = "render: ";

// Hands `line` (without its newline) to the sink: a '\r' before the newline and the lead "render: " cut off.
void Hand(LogSink sink, int level, std::string& line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const size_t lead = sizeof(kLead) - 1;
  const char* text = line.compare(0, lead, kLead) == 0 ? line.c_str() + lead : line.c_str();
  sink(level, text);
}

// Hands each whole line of `text` over and keeps the rest (part of a line) in `pending`.
void Gather(LogSink sink, int level, std::string& pending, const char* text, size_t size) {
  size_t from = 0;
  for (size_t i = 0; i < size; ++i) {
    if (text[i] != '\n') continue;
    pending.append(text + from, i - from);
    Hand(sink, level, pending);
    pending.clear();
    from = i + 1;
  }
  pending.append(text + from, size - from);
}

// A thread's parts of a line, stdout's [0] and stderr's [1], until their newlines. When the thread ends, what is left
// goes to the sink as a line of its own.
struct Pending {
  std::string text[2];
  ~Pending();
};
thread_local bool tGone = false;  // (trivial: still readable while and after the thread's Pending is destroyed)
// (trivial too) The thread's Pending once it has printed through a sink, null before and after: LogFlush and SetLogSink
// look here and never MAKE one (making it registers the thread's destructor, which may take memory: a host calls
// LogFlush from its crash handler, on a thread that may never have printed and whose heap may be the thing that broke).
thread_local Pending* tMine = nullptr;

Pending* Mine() {
  if (tGone) return nullptr;
  thread_local Pending pending;
  tMine = &pending;
  return &pending;
}

void FlushInto(LogSink sink, Pending& p) {
  for (int s = 0; s < 2; ++s) {
    if (p.text[s].empty()) continue;
    if (sink) {
      Hand(sink, s == 0 ? kLogInfo : kLogWarning, p.text[s]);
    } else {  // (the sink was taken away after this part was gathered: to the stream it was meant for)
      std::fputs(p.text[s].c_str(), s == 0 ? stdout : stderr);
    }
    p.text[s].clear();
  }
}

Pending::~Pending() {
  FlushInto(gSink.load(std::memory_order_acquire), *this);
  tMine = nullptr;
  tGone = true;
}

// Formats into `out` (appended); returns what vprintf would (the count of characters, or negative on an error).
int Format(std::string& out, const char* format, va_list args) {
  char small[512];
  va_list again;
  va_copy(again, args);
  const int n = std::vsnprintf(small, sizeof(small), format, args);
  if (n < 0) {
    va_end(again);
    return n;
  }
  if (static_cast<size_t>(n) < sizeof(small)) {
    out.append(small, static_cast<size_t>(n));
  } else {
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(n) + 1);
    std::vsnprintf(&out[at], static_cast<size_t>(n) + 1, format, again);
    out.resize(at + static_cast<size_t>(n));
  }
  va_end(again);
  return n;
}

// The sink's way for text to stdout (stream 0) or stderr (stream 1).
int ToSink(LogSink sink, int stream, const char* format, va_list args) {
  std::string text;
  const int n = Format(text, format, args);
  if (n <= 0) return n;
  const int level = stream == 0 ? kLogInfo : kLogWarning;
  if (Pending* p = Mine()) {
    Gather(sink, level, p->text[stream], text.data(), text.size());
  } else {  // (printed while the thread ends, after its Pending: each piece as a line of its own)
    std::string rest;
    Gather(sink, level, rest, text.data(), text.size());
    if (!rest.empty()) Hand(sink, level, rest);
  }
  return n;
}

}  // namespace

void SetLogSink(LogSink sink) {
  if (Pending* p = tMine) FlushInto(gSink.load(std::memory_order_acquire), *p);
  gSink.store(sink, std::memory_order_release);
}

LogSink GetLogSink() { return gSink.load(std::memory_order_acquire); }

int LogVprintf(const char* format, va_list args) {
  const LogSink sink = gSink.load(std::memory_order_acquire);
  if (!sink) return std::vprintf(format, args);
  return ToSink(sink, 0, format, args);
}

int LogVfprintf(FILE* stream, const char* format, va_list args) {
  const LogSink sink = gSink.load(std::memory_order_acquire);
  if (!sink || (stream != stdout && stream != stderr)) return std::vfprintf(stream, format, args);
  return ToSink(sink, stream == stdout ? 0 : 1, format, args);
}

int LogPrintf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int n = LogVprintf(format, args);
  va_end(args);
  return n;
}

int LogFprintf(FILE* stream, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int n = LogVfprintf(stream, format, args);
  va_end(args);
  return n;
}

void LogLine(int level, const char* format, ...) {
  std::string text;
  va_list args;
  va_start(args, format);
  const int n = Format(text, format, args);
  va_end(args);
  if (n < 0) return;
  if (text.empty() || text.back() != '\n') text += '\n';
  const LogSink sink = gSink.load(std::memory_order_acquire);
  if (!sink) {
    std::fputs(text.c_str(), level >= kLogWarning ? stderr : stdout);
    return;
  }
  std::string rest;
  Gather(sink, level, rest, text.data(), text.size());
}

// (takes no lock and no memory of its own: with nothing unfinished on this thread it does nothing at all; with a part of
// a line it calls the sink once or twice, and what the sink takes is the host's)
void LogFlush() {
  if (Pending* p = tMine) FlushInto(gSink.load(std::memory_order_acquire), *p);
}

}  // namespace render
