#include "sound/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace moho::ui::sound {
namespace {
float Linear(float decibels) { return decibels <= -95.0f ? 0.0f : std::pow(10.0f, decibels / 20.0f); }
const int kEnvelopeFrames = 64;  // what the curves give is worked out this often, straight between
}  // namespace

float TimeCurves::Decibels(float attackMs, float releasingMs) const {
  float decibels = 0;
  for (const Curve& c : attack) decibels += c.At(attackMs);
  if (releasingMs >= 0)
    for (const Curve& c : release) decibels += c.At(releasingMs);
  return decibels;
}

void MemoryWave::Read(int64_t frame, int count, int16_t* out) {
  int64_t have = std::max<int64_t>(0, std::min<int64_t>(count, frames - frame));
  if (frame < 0) have = 0;
  if (have > 0) std::memcpy(out, samples.data() + frame * channels, static_cast<size_t>(have * channels) * sizeof(int16_t));
  if (have < count) std::memset(out + have * channels, 0, static_cast<size_t>((count - have) * channels) * sizeof(int16_t));
}

FileWave::FileWave(const std::string& hostPath, uint64_t offset) : offset_(offset) { file_ = std::fopen(hostPath.c_str(), "rb"); }
FileWave::~FileWave() {
  if (file_) std::fclose(file_);
}

void FileWave::Read(int64_t frame, int count, int16_t* out) {
  for (int done = 0; done < count;) {
    int64_t at = frame + done;
    if (!file_ || at < 0 || at >= frames) {
      std::memset(out + done * channels, 0, static_cast<size_t>((count - done) * channels) * sizeof(int16_t));
      return;
    }
    if (at < bufferFrame_ || at >= bufferFrame_ + bufferFrames_) {
      // Half a second from here on. (The file's samples are little-endian 16 bits, as ours.)
      int64_t want = std::min<int64_t>(rate / 2, frames - at);
      buffer_.resize(static_cast<size_t>(want * channels));
#ifdef _WIN32
      _fseeki64(file_, static_cast<long long>(offset_ + static_cast<uint64_t>(at) * channels * 2), SEEK_SET);
#else
      fseeko(file_, static_cast<off_t>(offset_ + static_cast<uint64_t>(at) * channels * 2), SEEK_SET);
#endif
      size_t got = std::fread(buffer_.data(), sizeof(int16_t) * channels, static_cast<size_t>(want), file_);
      if (got < static_cast<size_t>(want))
        std::memset(buffer_.data() + got * channels, 0, (static_cast<size_t>(want) - got) * channels * sizeof(int16_t));
      bufferFrame_ = at, bufferFrames_ = want;
    }
    int64_t n = std::min<int64_t>(count - done, bufferFrame_ + bufferFrames_ - at);
    std::memcpy(out + done * channels, buffer_.data() + (at - bufferFrame_) * channels, static_cast<size_t>(n * channels) * sizeof(int16_t));
    done += static_cast<int>(n);
  }
}

int Mixer::Add(std::shared_ptr<WaveData> wave, float gain, float pitch, int loops, int category, int delayMs) {
  if (!wave || wave->frames <= 0 || wave->channels < 1) return 0;
  Voice v;
  v.wave = std::move(wave);
  v.gain = gain;
  v.step = static_cast<double>(v.wave->rate) / kRate * std::max(0.05f, pitch);
  v.loops = loops;
  v.category = category;
  v.delay = static_cast<int64_t>(delayMs) * kRate / 1000;
  std::lock_guard<std::mutex> lock(mutex_);
  v.id = nextId_++;
  voices_.push_back(std::move(v));
  return voices_.back().id;
}

void Mixer::Start(int voice) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_)
    if (v.id == voice) v.started = true;
}

void Mixer::SetTimeCurves(int voice, std::shared_ptr<const TimeCurves> curves) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_)
    if (v.id == voice) {
      v.curves = std::move(curves);
      v.envelope = v.curves ? Linear(v.curves->Decibels(v.curves->attackFromMs, -1)) : 1.0f;
      v.envelopeLeft = 0;
    }
}

void Mixer::Stop(int voice, bool immediate) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& v : voices_) {
      if (v.id != voice || immediate || !v.curves || v.curves->releaseMs <= 0 || !v.started || v.delay > 0) continue;
      if (v.releasing < 0) v.releasing = 0, v.envelopeLeft = 0;  // its release, by its curves
      return;
    }
  }
  FadeOut(voice, immediate ? 5.0f : 100.0f);
}

void Mixer::FadeOut(int voice, float ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < voices_.size(); ++i) {
    Voice& v = voices_[i];
    if (v.id != voice) continue;
    if (!v.started || v.delay > 0) {
      voices_.erase(voices_.begin() + static_cast<long>(i));  // it never sounded
      return;
    }
    v.fadeStep = std::max(v.fadeStep, 1.0f / (kRate * std::max(0.005f, ms / 1000.0f)));
  }
}

// What a voice's curves give `ahead` frames from now.
float Mixer::EnvelopeAt(const Voice& v, int64_t ahead) const {
  const float ms = 1000.0f / kRate;
  return Linear(v.curves->Decibels(v.curves->attackFromMs + static_cast<float>(v.sounded + ahead) * ms,
                                   v.releasing < 0 ? -1.0f : static_cast<float>(v.releasing + ahead) * ms));
}

void Mixer::StopAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_) v.fadeStep = 1.0f / (kRate * 0.005f);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.started || v.delay > 0; }), voices_.end());
}

void Mixer::Place(int voice, float left, float right) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Voice& v : voices_)
    if (v.id == voice) v.left = left, v.right = right;
}

bool Mixer::PlaceOf(int voice, float* left, float* right) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Voice& v : voices_)
    if (v.id == voice) {
      *left = v.left, *right = v.right;
      return true;
    }
  return false;
}

bool Mixer::Alive(int voice) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Voice& v : voices_)
    if (v.id == voice) return true;
  return false;
}

int Mixer::CountOf(int category, int* oldest) {
  std::lock_guard<std::mutex> lock(mutex_);
  int n = 0;
  if (oldest) *oldest = 0;
  for (const Voice& v : voices_) {
    if (v.category != category || v.fadeStep > 0 || v.releasing >= 0) continue;
    if (n++ == 0 && oldest) *oldest = v.id;
  }
  return n;
}

std::string Mixer::Describe() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  char line[160];
  for (const Voice& v : voices_) {
    size_t cat = static_cast<size_t>(v.category);
    float c = v.category >= 0 && cat < gains_.size() ? gains_[cat] : 1.0f;
    std::snprintf(line, sizeof line, "voice %d category %d own %.3f category's %.3f place %.3f %.3f loops %d at %.1f of %.1f s%s", v.id,
                  v.category, v.gain, c, v.left, v.right, v.loops, v.position / v.wave->rate, static_cast<double>(v.wave->frames) / v.wave->rate,
                  v.started ? "" : " (waiting)");
    out += line;
    if (v.curves) {  // what its curves by time give now, and how far into its release it is
      std::snprintf(line, sizeof line, " curves %.3f", v.envelope);
      out += line;
      if (v.releasing >= 0) {
        std::snprintf(line, sizeof line, " releasing %.0f of %.0f ms", static_cast<double>(v.releasing) * 1000.0 / kRate,
                      static_cast<double>(v.curves->releaseMs));
        out += line;
      }
    }
    out += "\n";
  }
  return out;
}

void Mixer::SetCategories(const std::vector<float>& gains, const std::vector<bool>& paused) {
  std::lock_guard<std::mutex> lock(mutex_);
  gains_ = gains;
  paused_ = paused;
}

void Mixer::Mix(int16_t* out, int frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  sum_.assign(static_cast<size_t>(frames) * 2, 0.0f);
  for (Voice& v : voices_) {
    if (!v.started) continue;
    size_t cat = static_cast<size_t>(v.category);
    if (v.category >= 0 && cat < paused_.size() && paused_[cat]) continue;
    float gain = v.gain * (v.category >= 0 && cat < gains_.size() ? gains_[cat] : 1.0f) / 32768.0f;
    int ch = v.wave->channels;
    int at = 0;
    if (v.delay > 0) {
      int64_t wait = std::min<int64_t>(v.delay, frames);
      v.delay -= wait;
      at = static_cast<int>(wait);
    }
    while (at < frames && v.wave) {
      // As many output frames as fit before the wave's end, from one read of the wave's frames.
      double left = static_cast<double>(v.wave->frames) - v.position;
      int n = static_cast<int>(std::min<double>(frames - at, std::ceil(left / v.step)));
      if (n <= 0) {
        if (v.loops == 0) {
          v.wave.reset();
          break;
        }
        if (v.loops > 0) --v.loops;
        v.position = 0;
        continue;
      }
      int64_t first = static_cast<int64_t>(v.position);
      int need = static_cast<int>(static_cast<int64_t>(v.position + v.step * (n - 1)) - first) + 2;
      piece_.resize(static_cast<size_t>(need * ch));
      v.wave->Read(first, need, piece_.data());
      for (int i = 0; i < n; ++i) {
        double p = v.position + v.step * i - static_cast<double>(first);
        int k = static_cast<int>(p);
        float f = static_cast<float>(p - k);
        const int16_t* s = piece_.data() + static_cast<size_t>(k) * ch;
        float l = s[0] + (s[ch] - s[0]) * f;
        float r = ch > 1 ? s[1] + (s[ch + 1] - s[1]) * f : l;
        float g = gain * v.fade;
        if (v.fadeStep > 0) {
          v.fade -= v.fadeStep;
          if (v.fade <= 0) {
            v.fade = 0;
            g = 0;
          }
        }
        if (v.curves) {
          if (v.envelopeLeft == 0) {
            v.envelopeStep = (EnvelopeAt(v, kEnvelopeFrames) - v.envelope) / kEnvelopeFrames;
            v.envelopeLeft = kEnvelopeFrames;
          }
          g *= v.envelope;
          v.envelope = std::max(0.0f, v.envelope + v.envelopeStep);
          --v.envelopeLeft;
          ++v.sounded;
          if (v.releasing >= 0 && ++v.releasing >= static_cast<int64_t>(v.curves->releaseMs * kRate / 1000.0f)) {
            v.fade = 0;  // its release has run its length: it ends
            n = i + 1;
          }
        }
        sum_[static_cast<size_t>(at + i) * 2] += l * g * v.left;
        sum_[static_cast<size_t>(at + i) * 2 + 1] += r * g * v.right;
      }
      v.position += v.step * n;
      at += n;
      if ((v.fadeStep > 0 || v.releasing >= 0) && v.fade <= 0) {
        v.wave.reset();
        break;
      }
    }
  }
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.wave; }), voices_.end());
  // Many sounds at once can add up to more than the output holds (the game's files make its
  // Global category louder than recorded, and a loud select sound over the music goes over).
  // Cutting the tops off is heard as crackle, so the whole is turned down for that moment
  // instead: at once when a frame would go over, back up over a fifth of a second.
  const float kCeiling = 0.98f, kBack = 1.0f / (0.2f * kRate);
  for (int i = 0; i < frames; ++i) {
    float l = sum_[static_cast<size_t>(i) * 2], r = sum_[static_cast<size_t>(i) * 2 + 1];
    float top = std::max(std::fabs(l), std::fabs(r));
    if (top * limit_ > kCeiling) limit_ = kCeiling / top;
    else if (limit_ < 1.0f) limit_ = std::min(1.0f, limit_ + kBack);
    out[i * 2] = static_cast<int16_t>(l * limit_ * 32767.0f);
    out[i * 2 + 1] = static_cast<int16_t>(r * limit_ * 32767.0f);
  }
  framesMixed_ += frames;
}

}  // namespace moho::ui::sound
