// The high-water marks of the renderer's fixed rooms (the viewer's --marks): how full each one got in a run. A room
// that fills is handled quietly where it fills (the oldest scar fades, the farthest box is left out, a texture set is
// taken again), so nothing shows how near a game came to it. With --marks every such place tells its count here and
// the viewer prints the most each one reached when it ends; the regression reads the lines and fails a room past
// four fifths. Without --marks a Mark() is one test of a flag.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace render {

struct MarkBook {
  bool wanted = false;  // (set once by the viewer before anything is drawn)
  std::mutex mutex;
  std::map<std::string, std::pair<uint64_t, uint64_t>> most;  // what -> the most used, the room (0: a count without one)
};

inline MarkBook& Marks() {
  static MarkBook book;
  return book;
}

// `used` of `room` are taken at `what` now (room 0: a plain count, the most of it kept).
inline void Mark(const char* what, uint64_t used, uint64_t room) {
  MarkBook& book = Marks();
  if (!book.wanted) return;
  std::lock_guard<std::mutex> lock(book.mutex);
  std::pair<uint64_t, uint64_t>& m = book.most[what];
  if (used > m.first) m.first = used;
  m.second = room;
}

inline void PrintMarks(FILE* to) {
  MarkBook& book = Marks();
  std::lock_guard<std::mutex> lock(book.mutex);
  for (const auto& m : book.most) {
    if (m.second.second)
      fprintf(to, "render: --marks: %-52s %9llu of %9llu (%3.0f %%)\n", m.first.c_str(), static_cast<unsigned long long>(m.second.first),
              static_cast<unsigned long long>(m.second.second), 100.0 * static_cast<double>(m.second.first) / static_cast<double>(m.second.second));
    else
      fprintf(to, "render: --marks: %-52s %9llu\n", m.first.c_str(), static_cast<unsigned long long>(m.second.first));
  }
}

}  // namespace render
