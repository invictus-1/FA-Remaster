// A movie of the game's: an .sfd file (CRI Sofdec: an MPEG program stream with MPEG-1 video). Its
// pictures are decoded by pl_mpeg (ui/third_party/pl_mpeg, MIT, Dominic Szablewski); the movie's
// clock is the caller's (Advance). The sound some movies carry (ADX) is not played yet.
#pragma once
#include <string>

#include "draw/draw.h"

struct plm_t;

namespace moho::ui {

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
  int Frames() const;                            // Duration * FrameRate, rounded
  // Moves the movie on by `seconds`: the pictures due in that time are decoded, the last of them
  // is Picture(). True when it changed. At the end it stops on the last picture (Ended), or starts
  // again from the first with SetLoop.
  bool Advance(double seconds);
  bool Ended() const;
  void SetLoop(bool loop);
  void Rewind();
  const Image& Picture() const { return picture_; }

 private:
  std::string bytes_;
  plm_t* plm_ = nullptr;
  int width_ = 0, height_ = 0;
  double frameRate_ = 0, duration_ = 0;
  Image picture_;

};

}  // namespace moho::ui
