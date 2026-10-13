#include "sound/sound_system.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/vfs.h"

namespace moho::ui::sound {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
float Linear(float decibels) { return decibels <= -95.0f ? 0.0f : std::pow(10.0f, decibels / 20.0f); }

// Where a file of the game's file system is on disk (the file system names it for scripts'
// error messages: "@" and the path).
std::string HostPath(const Vfs* vfs, const std::string& vpath) {
  std::string chunk = vfs->ChunkName(vpath);
  return chunk.empty() ? chunk : chunk.substr(1);
}

}  // namespace

SoundSystem::SoundSystem(const Vfs* vfs) : vfs_(vfs) {
  if (auto data = vfs_->ReadFile("/sounds/supcom.xgs")) {
    if (!ParseGlobalSettings(reinterpret_cast<const uint8_t*>(data->data()), data->size(), &categories_))
      Logf(LogLevel::Warning, "moho64ui: sound: /sounds/supcom.xgs is not read (another layout)");
    ParseCurves(reinterpret_cast<const uint8_t*>(data->data()), data->size(), &curves_);
  } else {
    Logf(LogLevel::Info, "moho64ui: sound: no /sounds/supcom.xgs: no sound categories");
  }
  userVolume_.assign(categories_.size(), 1.0f);
  paused_.assign(categories_.size(), false);
  UpdateCategories();
}

SoundSystem::~SoundSystem() {
  device_.reset();  // its thread first
  if (played_)
    Logf(LogLevel::Info, "moho64ui: sound: %d cues played, %.1f s of sound mixed", played_,
         static_cast<double>(mixer_.framesMixed()) / Mixer::kRate);
}

// A bank's file: among the game's sounds, or among the voices of the language.
std::string SoundSystem::Find(const std::string& file) const {
  std::string path = "/sounds/" + file;
  if (vfs_->Exists(path)) return path;
  std::string voice = "/sounds/voice/" + language_ + "/" + file;
  return vfs_->Exists(voice) ? voice : path;
}

void SoundSystem::SetLanguage(const std::string& name) { language_ = Lower(name); }

uint32_t SoundSystem::Random() {
  seed_ ^= seed_ << 13, seed_ ^= seed_ >> 17, seed_ ^= seed_ << 5;
  return seed_;
}

SoundSystem::Bank* SoundSystem::GetBank(const std::string& name) {
  std::string key = Lower(name);
  auto it = banks_.find(key);
  if (it != banks_.end()) return it->second->ok ? it->second.get() : nullptr;
  auto& bank = banks_[key];
  bank = std::make_unique<Bank>();
  std::string path = Find(key + ".xsb"), error;
  auto data = vfs_->ReadFile(path);
  if (!data) {
    Logf(LogLevel::Warning, "moho64ui: sound: no sound bank %s", path.c_str());
    return nullptr;
  }
  if (!ParseSoundBank(reinterpret_cast<const uint8_t*>(data->data()), data->size(), &bank->info, &error)) {
    Logf(LogLevel::Warning, "moho64ui: sound: %s: %s", path.c_str(), error.c_str());
    return nullptr;
  }
  bank->ok = true;
  return bank.get();
}

SoundSystem::WaveBank* SoundSystem::GetWaveBank(const std::string& name) {
  std::string key = Lower(name);
  auto it = waveBanks_.find(key);
  if (it != waveBanks_.end()) return it->second->ok ? it->second.get() : nullptr;
  auto& bank = waveBanks_[key];
  bank = std::make_unique<WaveBank>();
  std::string path = Find(key + ".xwb");
  auto info = vfs_->GetFileInfo(path);
  if (!info) {
    Logf(LogLevel::Warning, "moho64ui: sound: no wave bank %s", path.c_str());
    return nullptr;
  }
  // A small bank is taken whole. A large one: its head first; whole only if it is not made for
  // streaming.
  std::string host = HostPath(vfs_, path);
  std::string head;
  if (info->size > (4u << 20)) {
    if (std::FILE* f = std::fopen(host.c_str(), "rb")) {
      head.resize(1u << 20);
      head.resize(std::fread(head.data(), 1, head.size(), f));
      std::fclose(f);
    }
  }
  if (head.empty()) {
    auto data = vfs_->ReadFile(path);
    if (!data) return nullptr;
    bank->data = std::move(*data);
  }
  const std::string& first = head.empty() ? bank->data : head;
  if (!ParseWaveBank(reinterpret_cast<const uint8_t*>(first.data()), first.size(), &bank->info)) {
    Logf(LogLevel::Warning, "moho64ui: sound: %s is not read (another layout)", path.c_str());
    return nullptr;
  }
  if (bank->info.streaming && !head.empty()) {
    bank->hostPath = host;
  } else if (bank->data.empty()) {
    auto data = vfs_->ReadFile(path);
    if (!data) return nullptr;
    bank->data = std::move(*data);
  }
  bank->ok = true;
  return bank.get();
}

std::shared_ptr<WaveData> SoundSystem::GetWave(WaveBank* bank, int index) {
  if (index < 0 || index >= static_cast<int>(bank->info.entries.size())) return nullptr;
  const WaveEntry& e = bank->info.entries[static_cast<size_t>(index)];
  if (e.tag != 0 || e.bits != 16 || e.channels < 1 || e.channels > 2) {
    std::string key = bank->info.name + "#" + std::to_string(index);
    if (!complained_[key]) Logf(LogLevel::Warning, "moho64ui: sound: wave %s has a format that is not built (tag %d, %d bits)", key.c_str(), e.tag, e.bits);
    complained_[key] = true;
    return nullptr;
  }
  int64_t frames = static_cast<int64_t>(e.length) / (2 * e.channels);
  if (!bank->hostPath.empty()) {  // a stream of its own for every playing
    auto wave = std::make_shared<FileWave>(bank->hostPath, e.offset);
    if (!wave->ok()) return nullptr;
    wave->channels = e.channels, wave->rate = e.rate, wave->frames = frames;
    return wave;
  }
  auto it = bank->waves.find(index);
  if (it != bank->waves.end()) return it->second;
  if (e.offset + e.length > bank->data.size()) return nullptr;
  auto wave = std::make_shared<MemoryWave>();
  wave->channels = e.channels, wave->rate = e.rate, wave->frames = frames;
  wave->samples.resize(static_cast<size_t>(frames * e.channels));
  std::memcpy(wave->samples.data(), bank->data.data() + e.offset, wave->samples.size() * sizeof(int16_t));
  bank->waves[index] = wave;
  return wave;
}

int SoundSystem::Play(const std::string& bankName, const std::string& cueName, bool prepareOnly) {
  return PlayCue(bankName, cueName, prepareOnly, nullptr);
}

// The quietest a world sound is still played at (decibels by its curves alone).
const float kHeardFrom = -60.0f;

float SoundSystem::Heard(const std::vector<uint32_t>& curves, const float position[3], float* left, float* right) const {
  if (!listening_) {
    *left = *right = 0;
    return -96.0f;
  }
  float d[3] = {position[0] - eye_[0], position[1] - eye_[1], position[2] - eye_[2]};
  float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  float e[3] = {focus_[0] - eye_[0], focus_[1] - eye_[1], focus_[2] - eye_[2]};
  float eyeDistance = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  float decibels = 0;
  for (uint32_t offset : curves) {
    auto it = curves_.find(offset);
    if (it == curves_.end() || it->second.parameter != 0) continue;
    if (it->second.variable == "Distance") decibels += it->second.At(distance);
    else if (it->second.variable == "CameraDistance") decibels += it->second.At(eyeDistance);
  }
  // Between the ears by where it is on screen: at the screen's edge about two thirds to one side. (Ours.)
  float side = ((position[0] - focus_[0]) * right_[0] + (position[2] - focus_[2]) * right_[2]) / std::max(1.0f, zoom_ * 0.5f);
  side = std::max(-1.0f, std::min(1.0f, side));
  float gain = Linear(decibels);
  *left = gain * (1.0f - std::max(0.0f, side) * 0.65f);
  *right = gain * (1.0f + std::min(0.0f, side) * 0.65f);
  return decibels;
}

int SoundSystem::PlayCue(const std::string& bankName, const std::string& cueName, bool prepareOnly, const float* position) {
  auto complain = [&](const char* what) {
    std::string key = Lower(bankName) + "/" + Lower(cueName);
    if (!complained_[key]) Logf(LogLevel::Warning, "moho64ui: sound: %s (bank %s, cue %s)", what, bankName.c_str(), cueName.c_str());
    complained_[key] = true;
    return 0;
  };
  Bank* bank = GetBank(bankName);
  if (!bank) return complain("no such sound bank");
  auto it = bank->info.cues.find(Lower(cueName));
  if (it == bank->info.cues.end() || it->second.sounds.empty()) return complain("no such cue");
  const CueDef& cue = it->second;

  // One of the cue's sounds, by their weights.
  auto pick = [&](const std::vector<int>& weights) {
    int total = 0;
    for (int w : weights) total += w;
    int r = total > 0 ? static_cast<int>(Random() % static_cast<uint32_t>(total)) : 0;
    for (size_t i = 0; i < weights.size(); ++i) {
      if (r < weights[i]) return i;
      r -= weights[i];
    }
    return static_cast<size_t>(0);
  };
  const SoundDef& sound = bank->info.sounds[static_cast<size_t>(cue.sounds[pick(cue.weights)])];
  int category = sound.category < static_cast<int>(categories_.size()) ? sound.category : -1;
  // A sound of the world that cannot be heard from here is not played at all.
  float left = 1, right = 1;
  float heard = position ? Heard(sound.curves, position, &left, &right) : 0;
  if (position && std::getenv("MOHO64UI_SOUND_LOG"))  // a look at what the world plays and how loud
    Logf(LogLevel::Info, "moho64ui: sound: world %s/%s: %.1f dB by its %d curves, its own %.1f dB, category %s", bankName.c_str(),
         cueName.c_str(), heard, static_cast<int>(sound.curves.size()), sound.volume,
         category >= 0 ? categories_[static_cast<size_t>(category)].name.c_str() : "-");
  if (position && heard < kHeardFrom) return 0;

  // A category that is full: the new sound takes the oldest one's place, or is not played.
  if (category >= 0) {
    const Category& cat = categories_[static_cast<size_t>(category)];
    int oldest = 0;
    if (cat.maxInstances < 255 && mixer_.CountOf(category, &oldest) >= cat.maxInstances) {
      if (!cat.replaceOldest) return 0;
      mixer_.Stop(oldest, false);
    }
  }

  std::vector<int> voices;
  for (const PlayEvent& e : sound.events) {
    if (e.waves.empty()) continue;
    std::vector<int> weights;
    for (const WaveChoice& w : e.waves) weights.push_back(w.weight);
    const WaveChoice& choice = e.waves[pick(weights)];
    if (choice.bank < 0 || choice.bank >= static_cast<int>(bank->info.waveBanks.size())) continue;
    WaveBank* waves = GetWaveBank(bank->info.waveBanks[static_cast<size_t>(choice.bank)]);
    if (!waves) continue;
    std::shared_ptr<WaveData> wave = GetWave(waves, choice.wave);
    if (!wave) continue;
    auto between = [&](float a, float b) { return a + (b - a) * static_cast<float>(Random() % 1001) / 1000.0f; };
    float decibels = sound.volume + between(e.volumeMin, e.volumeMax);
    float cents = static_cast<float>(sound.pitch) + between(static_cast<float>(e.pitchMin), static_cast<float>(e.pitchMax));
    int voice = mixer_.Add(wave, Linear(decibels), std::pow(2.0f, cents / 1200.0f), e.loops == 255 ? -1 : e.loops, category, e.timeMs);
    if (!voice) continue;
    if (position) mixer_.Place(voice, left, right);
    voices.push_back(voice);
    lastWave_ = waves->info.name + "#" + std::to_string(choice.wave);
  }
  if (voices.empty()) return complain("the cue's wave is not there");
  ++played_;
  // Forget the cues that have ended.
  for (auto h = handles_.begin(); h != handles_.end();) {
    bool alive = false;
    for (int v : h->second) alive = alive || mixer_.Alive(v);
    h = alive ? std::next(h) : handles_.erase(h);
  }
  int handle = nextHandle_++;
  handles_[handle] = voices;
  if (position) {
    Placed& p = placed_[handle];
    p.position[0] = position[0], p.position[1] = position[1], p.position[2] = position[2];
    p.curves = sound.curves;
    ++worldPlayed_;
    // (With MOHO64UI_SOUND_LOG: once for each cue, the curves its sound names, to see by what a
    // sound of the world grows quiet.)
    static const bool log = std::getenv("MOHO64UI_SOUND_LOG") != nullptr;
    std::string key = "curves " + Lower(bankName) + "/" + Lower(cueName);
    if (log && !complained_[key]) {
      complained_[key] = true;
      std::string text;
      for (uint32_t offset : sound.curves) {
        auto c = curves_.find(offset);
        if (c == curves_.end()) {
          text += " [unknown curve]";
          continue;
        }
        text += " [" + c->second.variable + " -> parameter " + std::to_string(c->second.parameter) + ":";
        char point[48];
        for (const auto& xy : c->second.points) {
          std::snprintf(point, sizeof point, " %.0f=%.1f", xy.first, xy.second / 100.0f);
          text += point;
        }
        text += "]";
      }
      Logf(LogLevel::Info, "moho64ui: sound: %s/%s: volume %.1f dB, category %d, curves:%s", bankName.c_str(), cueName.c_str(),
           sound.volume, category, text.empty() ? " none" : text.c_str());
    }
  }
  if (!prepareOnly) Start(handle);
  return handle;
}

void SoundSystem::SetListener(const float eye[3], const float focus[3], const float right[3], float zoom) {
  for (int i = 0; i < 3; ++i) eye_[i] = eye[i], focus_[i] = focus[i], right_[i] = right[i];
  zoom_ = zoom;
  listening_ = true;
  UpdateWorld();
}

int SoundSystem::PlayAt(const std::string& bank, const std::string& cue, const float position[3]) {
  return PlayCue(bank, cue, false, position);
}

void SoundSystem::SetLoop(uint32_t entity, const std::string& bank, const std::string& cue, const float position[3]) {
  auto it = loops_.find(entity);
  if (it != loops_.end() && it->second.handle) Stop(it->second.handle, false);
  if (bank.empty() || cue.empty()) {
    if (it != loops_.end()) loops_.erase(it);
    return;
  }
  Loop& loop = loops_[entity];
  loop.bank = bank, loop.cue = cue, loop.handle = 0;
  loop.position[0] = position[0], loop.position[1] = position[1], loop.position[2] = position[2];
}

void SoundSystem::MoveLoop(uint32_t entity, const float position[3]) {
  auto it = loops_.find(entity);
  if (it == loops_.end()) return;
  for (int i = 0; i < 3; ++i) it->second.position[i] = position[i];
  auto placed = placed_.find(it->second.handle);
  if (placed != placed_.end())
    for (int i = 0; i < 3; ++i) placed->second.position[i] = position[i];
}

void SoundSystem::StopWorld() {
  for (auto& entry : placed_) Stop(entry.first, true);
  placed_.clear();
  loops_.clear();
  listening_ = false;
}

// Every playing sound of the world gets its loudness and its side again; a running sound that has
// come into hearing starts, one that has gone out of it ends.
void SoundSystem::UpdateWorld() {
  for (auto it = placed_.begin(); it != placed_.end();) {
    auto voices = handles_.find(it->first);
    bool alive = false;
    float left = 1, right = 1;
    float decibels = Heard(it->second.curves, it->second.position, &left, &right);
    if (voices != handles_.end())
      for (int v : voices->second) {
        if (!mixer_.Alive(v)) continue;
        alive = true;
        mixer_.Place(v, left, right);
      }
    (void)decibels;
    it = alive ? std::next(it) : placed_.erase(it);
  }
  for (auto& entry : loops_) {
    Loop& loop = entry.second;
    auto placed = placed_.find(loop.handle);
    if (placed == placed_.end()) {
      loop.handle = PlayCue(loop.bank, loop.cue, false, loop.position);  // 0 while it cannot be heard
      continue;
    }
    float left = 1, right = 1;
    if (Heard(placed->second.curves, loop.position, &left, &right) < kHeardFrom) {
      Stop(loop.handle, false);
      placed_.erase(placed);
      loop.handle = 0;
    }
  }
}

void SoundSystem::Start(int handle) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  for (int v : it->second) mixer_.Start(v);
}

void SoundSystem::Stop(int handle, bool immediate) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  for (int v : it->second) mixer_.Stop(v, immediate);
}

void SoundSystem::StopAll() { mixer_.StopAll(); }

bool SoundSystem::Playing(int handle) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return false;
  for (int v : it->second)
    if (mixer_.Alive(v)) return true;
  return false;
}

int SoundSystem::CategoryByName(const std::string& name) const {
  std::string key = Lower(name);
  for (size_t i = 0; i < categories_.size(); ++i)
    if (Lower(categories_[i].name) == key) return static_cast<int>(i);
  return -1;
}

void SoundSystem::UpdateCategories() {
  std::vector<float> gains(categories_.size(), 1.0f);
  std::vector<bool> paused(categories_.size(), false);
  for (size_t i = 0; i < categories_.size(); ++i) {
    float g = 1;
    bool p = false;
    int guard = 0;
    for (int c = static_cast<int>(i); c >= 0 && c < static_cast<int>(categories_.size()) && guard < 64; c = categories_[static_cast<size_t>(c)].parent, ++guard) {
      g *= Linear(categories_[static_cast<size_t>(c)].volume) * userVolume_[static_cast<size_t>(c)];
      p = p || paused_[static_cast<size_t>(c)];
    }
    gains[i] = g, paused[i] = p;
  }
  mixer_.SetCategories(gains, paused);
}

bool SoundSystem::SetVolume(const std::string& category, float volume) {
  int c = CategoryByName(category);
  if (c < 0) return false;
  userVolume_[static_cast<size_t>(c)] = std::max(0.0f, volume);
  UpdateCategories();
  return true;
}

bool SoundSystem::GetVolume(const std::string& category, float* volume) const {
  int c = CategoryByName(category);
  if (c < 0) return false;
  *volume = userVolume_[static_cast<size_t>(c)];
  return true;
}

bool SoundSystem::Pause(const std::string& category, bool pause) {
  int c = CategoryByName(category);
  if (c < 0) return false;
  paused_[static_cast<size_t>(c)] = pause;
  UpdateCategories();
  return true;
}

bool SoundSystem::OpenDevice() {
  if (!device_) device_ = OpenOutputDevice(&mixer_);
  return device_ != nullptr;
}

}  // namespace moho::ui::sound
