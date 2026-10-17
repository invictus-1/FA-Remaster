// Mixing: every sound that plays is a voice; the mixer adds the voices up into one stereo stream
// for the sound device. Our own: the original hands this to its sound library.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sound/xact.h"

namespace moho::ui::sound {

// A voice's loudness by time, as the sound files give it with curves by two variables of a playing
// sound: "AttackTime", the milliseconds since it began to sound (the units' running sounds come in
// over half a second or a second), and "ReleaseTime", the milliseconds since it was stopped softly
// (the music goes out over 6 s, the units' loops over 1 or 3). Several curves add (decibels). A
// voice stopped softly goes on until `releaseMs`, the furthest end of the sound's release curves;
// without them a soft stop is a short fade (ours, see Mixer::Stop).
// As the sound library does it (the game's own program hands this to it; read in FNA's FACT, a
// reimplementation of that library, 2026-10-16: the release's length the furthest last point of
// the release curves of the sound's tracks; the attack timed from the track's first event; all
// volume curves added in decibels).
struct TimeCurves {
  std::vector<Curve> attack, release;
  float releaseMs = 0;
  float Decibels(float attackMs, float releaseMs /* below 0: not stopped */) const;
};

// A wave's samples (16 bits, channels interleaved).
class WaveData {
 public:
  virtual ~WaveData() = default;
  int channels = 1, rate = 32000;
  int64_t frames = 0;
  // `count` frames from `frame` on into `out`; past the end, silence.
  virtual void Read(int64_t frame, int count, int16_t* out) = 0;
};
// Held in memory (the short sounds).
class MemoryWave : public WaveData {
 public:
  std::vector<int16_t> samples;
  void Read(int64_t frame, int count, int16_t* out) override;
};
// Read from its file as it plays (the music: tens of megabytes a piece).
class FileWave : public WaveData {
 public:
  FileWave(const std::string& hostPath, uint64_t offset);
  ~FileWave() override;
  bool ok() const { return file_ != nullptr; }
  void Read(int64_t frame, int count, int16_t* out) override;

 private:
  std::FILE* file_ = nullptr;
  uint64_t offset_ = 0;
  std::vector<int16_t> buffer_;  // a piece of the file
  int64_t bufferFrame_ = 0, bufferFrames_ = 0;
};

class Mixer {
 public:
  static const int kRate = 48000;  // what goes to the device: 48000 frames a second, stereo, 16 bits

  // A new voice; it waits until it is started. `gain` is its own loudness (1 = as recorded),
  // `pitch` a factor on its speed, `loops` how often it plays again (-1: without end),
  // `delayMs` how long after its start it begins to sound. Returns its number (never 0).
  int Add(std::shared_ptr<WaveData> wave, float gain, float pitch, int loops, int category, int delayMs);
  void Start(int voice);
  // A voice's curves by time (TimeCurves); set before it is started.
  void SetTimeCurves(int voice, std::shared_ptr<const TimeCurves> curves);
  // Stopped softly: through its release curves when it has them, else fading out over a moment (a
  // tenth of a second: ours; the sound library ends it at once). Or at once (still a few
  // milliseconds, so that nothing clicks).
  void Stop(int voice, bool immediate);
  // Fading out over `ms` (at least the few milliseconds of an immediate stop): a sound whose place
  // a new one of its category takes.
  void FadeOut(int voice, float ms);
  void StopAll();
  bool Alive(int voice);
  // A voice's place among the listener's ears: a factor for the left and one for the right, on
  // top of its own loudness (a sound of the world, as the listener moves; 1 and 1 at first).
  void Place(int voice, float left, float right);
  bool PlaceOf(int voice, float* left, float* right);  // (false: no such voice)
  // How many voices of a category sound or wait, and the oldest of them (0: none). Those being
  // stopped are not counted.
  int CountOf(int category, int* oldest);

  // The loudness and the pause of each category, as the sound system worked them out.
  void SetCategories(const std::vector<float>& gains, const std::vector<bool>& paused);

  // The next `frames` frames (stereo: 2 * frames values).
  void Mix(int16_t* out, int frames);
  int64_t framesMixed() const { return framesMixed_; }
  // For a look at what is playing: one line per voice (its category, loudness, place, how far in).
  std::string Describe();

 private:
  struct Voice {
    int id = 0, category = 0;
    std::shared_ptr<WaveData> wave;
    double position = 0, step = 1;  // in the wave's frames
    float gain = 1;
    int loops = 0;
    int64_t delay = 0;       // output frames before it sounds
    bool started = false;
    float fade = 1, fadeStep = 0;  // going down to 0 when it is being stopped
    float left = 1, right = 1;     // its place (Place)
    std::shared_ptr<const TimeCurves> curves;  // (none: always 1)
    int64_t sounded = 0;     // output frames it has sounded
    int64_t releasing = -1;  // output frames since its release began (-1: not stopped softly)
    float envelope = 1, envelopeStep = 0;  // what its curves give now, and how that moves per frame
    int envelopeLeft = 0;    // frames until it is worked out again
  };
  float EnvelopeAt(const Voice& v, int64_t ahead) const;
  std::mutex mutex_;
  std::vector<Voice> voices_;
  std::vector<float> gains_;
  std::vector<bool> paused_;
  std::vector<float> sum_;
  std::vector<int16_t> piece_;
  int nextId_ = 1;
  int64_t framesMixed_ = 0;
  float limit_ = 1;  // below 1 while the sum of the sounds would go over the top
};

}  // namespace moho::ui::sound
