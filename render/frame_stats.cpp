// [rbench] The viewer's --timings frame by frame (frame_stats.h).
#include "frame_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace render {

namespace {

// 120 and 60 frames a second
constexpr double kFast = 1000.0 / 120.0, kSmooth = 1000.0 / 60.0;

struct Summary {
  int n = 0;
  double low = 0, median = 0, p99 = 0, p999 = 0, longest = 0;
  int overFast = 0, overSmooth = 0;
};

// (the nearest rank: the p-th percentile is the smallest value that share p of the frames do not exceed; the median as
// --timings' "the middle one", the upper of the two middle values)
Summary Summarise(std::vector<double> v) {
  Summary s;
  s.n = static_cast<int>(v.size());
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const auto rank = [&](double p) {
    const long i = static_cast<long>(std::ceil(p * static_cast<double>(v.size()))) - 1;
    return v[static_cast<size_t>(std::clamp(i, 0L, static_cast<long>(v.size()) - 1))];
  };
  s.low = v.front();
  s.median = v[v.size() / 2];
  s.p99 = rank(0.99);
  s.p999 = rank(0.999);
  s.longest = v.back();
  for (double x : v) {
    s.overFast += x > kFast;
    s.overSmooth += x > kSmooth;
  }
  return s;
}

}  // namespace

void FrameStats::Add(const Frame& frame, const std::vector<std::pair<std::string, double>>& stages) {
  frames_.push_back(frame);
  std::vector<std::pair<int, double>> row;
  row.reserve(stages.size());
  for (const auto& s : stages) {
    int column = -1;
    for (size_t c = 0; c < columns_.size(); ++c)
      if (columns_[c] == s.first) {
        column = static_cast<int>(c);
        break;
      }
    if (column < 0) {
      column = static_cast<int>(columns_.size());
      columns_.push_back(s.first);
    }
    // (a stage met twice in one frame is added up, as --timings' averages do)
    auto same = std::find_if(row.begin(), row.end(), [&](const auto& r) { return r.first == column; });
    if (same != row.end()) same->second += s.second;
    else row.emplace_back(column, s.second);
  }
  values_.push_back(std::move(row));
}

void FrameStats::Print(int width, int height) const {
  std::vector<double> processor, card, shown;
  int readBack = 0, waitedFrames = 0;
  double freeFrame = 0, limiter = 0, waited = 0, waitedMost = 0;
  const Frame* longest = nullptr;
  size_t longestAt = 0;
  for (size_t i = 0; i < frames_.size(); ++i) {
    const Frame& f = frames_[i];
    if (f.readBack) {
      ++readBack;
      continue;
    }
    processor.push_back(f.Processor());
    shown.push_back(f.Shown());
    if (f.card >= 0) card.push_back(f.card);
    freeFrame += f.freeFrame;
    limiter += f.limiter;
    if (f.simulation > 0.05) ++waitedFrames;
    waited += f.simulation;
    waitedMost = std::max(waitedMost, f.simulation);
    if (!longest || f.Shown() > longest->Shown()) longest = &f, longestAt = i;
  }
  const int n = static_cast<int>(processor.size());
  if (n == 0) return;
  printf("render: --timings: frame by frame, %d frames", n);
  if (readBack) printf(" (%d read back for a picture left out: the processor waits for the card there)", readBack);
  printf(", ms (the slowest 1 %% and 0.1 %%: the 99th and 99.9th percentiles):\n");
  const Summary sp = Summarise(processor), sc = Summarise(card), ss = Summarise(shown);
  const auto line = [](const char* what, const Summary& s) {
    if (s.n == 0) {
      printf("render: --timings:   %-46s none taken (the device keeps no times)\n", what);
      return;
    }
    printf("render: --timings:   %-46s lowest %7.3f  typical %7.3f  slowest 1 %% %7.3f  0.1 %% %7.3f  longest %7.3f  over 8.3 ms %5d  over 16.7 ms %5d\n",
           what, s.low, s.median, s.p99, s.p999, s.longest, s.overFast, s.overSmooth);
  };
  line("processor (its own work, the waits left out)", sp);
  line("card (its whole frame)", sc);
  line("frame (as shown: one frame's end to the next)", ss);
  printf("render: --timings:   left out: of the processor's, waiting for a free frame %.3f ms a frame and for the latency limiter %.3f; "
         "of both, waiting for the simulation (--fixed-step) %.1f ms in all, in %d frames, the longest %.1f\n",
         freeFrame / n, limiter / n, waited, waitedFrames, waitedMost);
  if (longest) {
    printf("render: --timings:   the longest frame as shown: frame %d (beat %.1f, camera %.0f away) %.1f ms: the processor %.1f, the card %.1f; its stages over 1 ms:",
           longest->number, longest->beat, longest->camera, longest->Shown(), longest->Processor(), longest->card);
    int listed = 0;
    for (const auto& v : values_[longestAt])
      if (v.second >= 1 && columns_[static_cast<size_t>(v.first)] != "cpu: whole frame" && columns_[static_cast<size_t>(v.first)] != "gpu: whole frame")
        printf("%s %s %.1f", listed++ ? ";" : "", columns_[static_cast<size_t>(v.first)].c_str(), v.second);
    printf("%s\n", listed ? "" : " none");
  }
  // (for scripts: one line, key=value)
  printf("render: --timings-stats: frames=%d readback=%d size=%dx%d", n, readBack, width, height);
  const auto keys = [](const char* name, const Summary& s) {
    printf(" %s_n=%d %s_low=%.3f %s_median=%.3f %s_p99=%.3f %s_p999=%.3f %s_max=%.3f %s_over8=%d %s_over16=%d", name, s.n, name, s.low, name, s.median, name,
           s.p99, name, s.p999, name, s.longest, name, s.overFast, name, s.overSmooth);
  };
  keys("processor", sp);
  keys("card", sc);
  keys("frame", ss);
  printf(" freewait_mean=%.3f limiter_mean=%.3f simwait_total=%.1f simwait_frames=%d simwait_max=%.1f", freeFrame / n, limiter / n, waited, waitedFrames,
         waitedMost);
  if (longest) printf(" longest_frame=%d longest_beat=%.1f longest_ms=%.3f", longest->number, longest->beat, longest->Shown());
  printf("\n");
}

bool FrameStats::WriteCsv(const std::string& path) const {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fprintf(f, "frame,beat,camera,interval,simulation_wait,limiter_wait,free_frame_wait,processor,shown,card,read_back");
  for (const std::string& c : columns_) {
    std::string quoted = "\"";
    for (char ch : c) quoted += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    fprintf(f, ",%s\"", quoted.c_str());
  }
  fprintf(f, "\n");
  std::vector<double> row;
  std::vector<bool> has;
  for (size_t i = 0; i < frames_.size(); ++i) {
    const Frame& r = frames_[i];
    fprintf(f, "%d,%.2f,%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,", r.number, r.beat, r.camera, r.interval, r.simulation, r.limiter, r.freeFrame, r.Processor(),
            r.Shown());
    if (r.card >= 0) fprintf(f, "%.4f", r.card);
    fprintf(f, ",%d", r.readBack ? 1 : 0);
    row.assign(columns_.size(), 0);
    has.assign(columns_.size(), false);
    for (const auto& v : values_[i]) {
      row[static_cast<size_t>(v.first)] = v.second;
      has[static_cast<size_t>(v.first)] = true;
    }
    for (size_t c = 0; c < columns_.size(); ++c) {
      if (has[c]) fprintf(f, ",%.4f", row[c]);
      else fprintf(f, ",");
    }
    fprintf(f, "\n");
  }
  return fclose(f) == 0;
}

}  // namespace render
