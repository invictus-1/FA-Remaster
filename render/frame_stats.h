// [rbench] The viewer's --timings frame by frame: every counted frame's times are kept, so that the end of the run can
// say more than the averages: the lowest frame, the typical one (the median), the slowest 1 % and 0.1 % (the 99th and
// 99.9th percentiles: a frame that long or longer is one of the slowest 1 % or 0.1 %), the longest, and how many frames
// took longer than 8.3 ms (120 a second) and 16.7 ms (60 a second); each for the processor's own work, the card's and
// the frame as the screen shows it. With --timings-csv <file> every frame is written as a row and every stage as a
// column (the benchmarks' runner, render/tools/bench_render.py, reads it). Nothing of this runs without --timings.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace render {

class FrameStats {
 public:
  struct Frame {
    int number = 0;         // the frame's count (as the hitch log numbers it)
    double beat = 0;        // the game's beat shown (0 without a game)
    double camera = 0;      // the camera's distance
    double interval = 0;    // ms from the last frame's end to this one's: what the screen shows, waits and all
    double simulation = 0;  // --fixed-step: ms waited for the simulation to reach the beat (a test's wait, not a game's)
    double limiter = 0;     // ms the latency limiter waited before the input was read
    double freeFrame = 0;   // ms waited for a free frame (the card behind)
    double card = -1;       // ms of the card's whole frame, its stages added up (a frame or two before; below 0: none)
    bool readBack = false;  // the picture was read back for a file (the processor then waits for the card)
    // The processor's own work for the frame: everything but the waits.
    double Processor() const { return interval - simulation - limiter - freeFrame; }
    // The frame as a game shows it: a game does not wait for its simulation (it shows the newest beat it has).
    double Shown() const { return interval - simulation; }
  };

  // A counted frame: its times and its stages (as --timings names them: "cpu: ...", "gpu: ...", "cpu part: ...").
  void Add(const Frame& frame, const std::vector<std::pair<std::string, double>>& stages);
  // The lines after --timings' own: the three measures' lowest, typical, slowest 1 % and 0.1 %, longest and the
  // counts over 8.3 and 16.7 ms, the waits left out, the longest frame; then one line of key=value pairs for scripts
  // ("render: --timings-stats: ..."). Frames read back for a picture are left out (said so).
  void Print(int width, int height) const;
  // Every frame a row, the fixed columns first, then every stage in the order first met (empty where a frame had none).
  bool WriteCsv(const std::string& path) const;
  bool Empty() const { return frames_.empty(); }

 private:
  std::vector<Frame> frames_;
  std::vector<std::string> columns_;                        // the stages' names, in the order first met
  std::vector<std::vector<std::pair<int, double>>> values_;  // per frame: (column, ms)
};

}  // namespace render
