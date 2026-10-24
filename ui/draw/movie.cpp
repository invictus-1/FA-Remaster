#include "draw/movie.h"

#include <cmath>
#include <cstdio>  // (pl_mpeg.h names FILE before it includes stdio.h)

#include "third_party/pl_mpeg/pl_mpeg.h"

namespace moho::ui {
namespace {
// The decoder hands over each picture it decodes; the movie keeps the last as its own pixels
// (0xAARRGGBB: pl_mpeg's BGRA bytes are those little-endian), with a new serial so a picture drawn
// before counts as changed.
void OnPicture(plm_t*, plm_frame_t* frame, void* user) {
  auto* movie = static_cast<Image*>(user);
  movie->width = static_cast<int>(frame->width), movie->height = static_cast<int>(frame->height);
  movie->pixels.resize(static_cast<size_t>(movie->width) * static_cast<size_t>(movie->height));
  plm_frame_to_bgra(frame, reinterpret_cast<uint8_t*>(movie->pixels.data()), movie->width * 4);
  for (uint32_t& p : movie->pixels) p |= 0xff000000u;  // (pl_mpeg writes the colours only: opaque)
  movie->serial = NewImageSerial();
}
// The frame count the Sofdec header gives (the first MPEG private-stream-2 packet, 00 00 01 BF:
// its data holds "SofdecStream"; the duration in ms at +0xaa, the frames at +0xae, u32 LE;
// checked on Brackman.sfd 5005 / 150, FMV_menu.sfd 65700 / 1971). -1 when there is none.
int SofdecFrames(const std::string& d) {
  const size_t limit = d.size() < 65536 ? d.size() : 65536;
  for (size_t i = 0; i + 6 + 0xb2 <= limit; ++i) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1 || static_cast<uint8_t>(d[i + 3]) != 0xbf) continue;
    const size_t data = i + 6;
    const size_t tag = d.find("SofdecStream", data);  // (14 bytes into the data in the game's files)
    if (tag == std::string::npos || tag > data + 64) continue;
    uint32_t frames = 0;
    for (int k = 3; k >= 0; --k) frames = frames << 8 | static_cast<uint8_t>(d[data + 0xae + static_cast<size_t>(k)]);
    return static_cast<int>(frames);
  }
  return -1;
}
}  // namespace

MovieDecoder::~MovieDecoder() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }
  if (plm_) plm_destroy(plm_);
}

void MovieDecoder::Worker() {
  std::unique_lock<std::mutex> lock(lock_);
  for (;;) {
    wake_.wait(lock, [this] { return stepDue_ || quit_; });
    if (quit_) return;
    stepDue_ = false;
    const double seconds = stepSeconds_;
    lock.unlock();
    plm_decode(plm_, seconds);  // (its pictures to decoded_: OnPicture)
    lock.lock();
    stepping_ = false;
    wake_.notify_all();
  }
}

void MovieDecoder::Collect() {
  if (!worker_.joinable()) return;
  std::unique_lock<std::mutex> lock(lock_);
  wake_.wait(lock, [this] { return !stepping_; });
  // (A picture decoded since: its serial is newer, NewImageSerial counting up. After the swap decoded_ holds the
  // older one, which the next decoded picture overwrites with a newer serial.)
  if (decoded_.valid() && decoded_.serial > picture_.serial) std::swap(picture_, decoded_);
}

bool MovieDecoder::Open(std::string bytes) {
  Collect();
  ahead_ = false;
  if (plm_) plm_destroy(plm_), plm_ = nullptr;
  bytes_ = std::move(bytes);
  if (bytes_.empty()) return false;
  plm_ = plm_create_with_memory(reinterpret_cast<uint8_t*>(bytes_.data()), bytes_.size(), 0);
  if (!plm_) return false;
  plm_set_audio_enabled(plm_, 0);  // (the game's movies carry no sound: its scripts play cues beside them)
  if (!plm_has_headers(plm_) || !plm_get_video_enabled(plm_)) {
    plm_destroy(plm_), plm_ = nullptr;
    return false;
  }
  width_ = plm_get_width(plm_), height_ = plm_get_height(plm_);
  frameRate_ = plm_get_framerate(plm_);
  duration_ = plm_get_duration(plm_);
  frames_ = SofdecFrames(bytes_);
  // The first picture at once, so the control shows something before its first step.
  if (plm_frame_t* first = plm_decode_video(plm_)) OnPicture(plm_, first, &picture_);
  ahead_ = static_cast<long long>(width_) * height_ >= kAheadPixels;
  plm_set_video_decode_callback(plm_, OnPicture, ahead_ ? &decoded_ : &picture_);
  if (ahead_) {
    if (!worker_.joinable()) worker_ = std::thread([this] { Worker(); });
  }
  return true;
}

int MovieDecoder::Frames() const { return frames_ >= 0 ? frames_ : static_cast<int>(std::lround(duration_ * frameRate_)); }

bool MovieDecoder::Advance(double seconds) {
  if (!plm_ || seconds <= 0) return false;
  const uint64_t before = picture_.serial;
  if (ahead_) {
    Collect();
    {
      std::lock_guard<std::mutex> lock(lock_);
      stepSeconds_ = seconds, stepDue_ = true, stepping_ = true;
    }
    wake_.notify_all();
  } else {
    plm_decode(plm_, seconds);
  }
  return picture_.serial != before;
}

bool MovieDecoder::Ended() {
  Collect();
  return !plm_ || plm_has_ended(plm_);
}

void MovieDecoder::SetLoop(bool loop) {
  Collect();
  if (plm_) plm_set_loop(plm_, loop ? 1 : 0);
}

void MovieDecoder::Rewind() {
  Collect();
  if (plm_) plm_rewind(plm_);
}

}  // namespace moho::ui
