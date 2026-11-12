// A movie of the game's: an .sfd file (CRI Sofdec: an MPEG program stream with MPEG-1 video). Its
// pictures are decoded by pl_mpeg (ui/third_party/pl_mpeg, MIT, Dominic Szablewski); the movie's
// clock is the caller's (Advance). The game's movies carry no sound (only an unused demo has ADX):
// the scripts play sound cues beside them.
#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "draw/draw.h"

struct plm_t;

namespace moho::ui {

// Where a decoded picture goes (the decoder's callback): pictures before `skipBefore` (the film's seconds) are
// decoded but not turned into pixels.
struct MoviePictureSink {
  Image* to = nullptr;
  double skipBefore = -1;
  int converted = 0;  // pictures turned into pixels so far
};

class MovieDecoder {
 public:
  MovieDecoder() = default;
  ~MovieDecoder();
  MovieDecoder(const MovieDecoder&) = delete;
  MovieDecoder& operator=(const MovieDecoder&) = delete;

  // The file's bytes (kept while the movie lives). False when they hold no MPEG-1 video.
  bool Open(std::string bytes);
  bool IsOpen() const { return plm_ != nullptr; }
  int Width() const { return width_; }
  int Height() const { return height_; }
  double FrameRate() const { return frameRate_; }
  double Duration() const { return duration_; }  // seconds, from the stream's time stamps
  int Frames() const;  // the Sofdec header's count, else Duration * FrameRate rounded
  // Moves the movie on by `seconds`: the pictures due in that time are decoded, the last of them
  // is Picture(). True when it changed. At the end it stops on the last picture (Ended), or starts
  // again from the first with SetLoop.
  // Only the last picture due is turned into pixels (2026-11-12, the review's toolkit L15): after a stall of some
  // seconds (a load, the window dragged) a step holds a hundred pictures and more, and every one of them was
  // converted although only the last is shown. They are all still decoded (each needs the one before); the film's
  // clock, its end and what it shows are the same.
  // A big movie (the loading screen's, kAheadPixels and more) is decoded on a thread of its own: Advance hands the
  // step to it and shows what the step before decoded, so its picture comes a frame later than it is decoded (a
  // loading screen's frame at 4K kept some 5 ms shorter). Ended, Rewind and SetLoop wait for the step under way. This
  // program's own; only about time (the movie's clock, its end and its pictures are the same).
  bool Advance(double seconds);
  int Converted() const { return sink_.converted; }  // (pictures turned into pixels so far: tests)
  bool Ended();
  void SetLoop(bool loop);
  void Rewind();
  const Image& Picture() const { return picture_; }

 private:
  static constexpr long long kAheadPixels = 640 * 480;
  void Collect();  // (the step under way finished, its newest picture taken as Picture)
  void Worker();
  void Step(double seconds);  // (on the thread that decodes)
  MoviePictureSink sink_;
  std::thread worker_;
  std::mutex lock_;
  std::condition_variable wake_;
  bool stepDue_ = false, stepping_ = false, quit_ = false;
  double stepSeconds_ = 0;
  Image decoded_;  // the worker's newest picture (taken when its serial is newer than Picture's)
  bool ahead_ = false;  // decoded on the worker
  std::string bytes_;
  plm_t* plm_ = nullptr;
  int width_ = 0, height_ = 0;
  double frameRate_ = 0, duration_ = 0;
  int frames_ = -1;  // the Sofdec header's count (-1: none)
  Image picture_;

};

}  // namespace moho::ui
