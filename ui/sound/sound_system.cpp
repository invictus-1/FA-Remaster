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
    std::map<std::string, float> variables;
    ParseVariables(reinterpret_cast<const uint8_t*>(data->data()), data->size(), &variables);
    auto length = variables.find("DuckLength");
    duckLength_ = length != variables.end() && variables.count("Duck") ? length->second : 0;  // (0: no duck, as the original without them)
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
bool SoundSystem::HasLanguage(const std::string& name) const {
  const std::string folder = "/sounds/voice/" + Lower(name);
  return vfs_->IsFolder(folder) || !vfs_->FindFiles(folder, "*").empty();
}

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

// SET BY EAR, NOT READ FROM THE ORIGINAL. The hums of working buildings (the game's sound
// categories "ActiveLoops...": a power generator's crackle, a mass extractor's thump) come out
// far louder here than Chris hears them in the original, by everything that could be read:
// the cue's and the track's volume, the categories down the chain (the game's sound library
// itself was read for this, xactengine2_10.dll: a cue's volume is multiplied by its category's
// and every parent's, each the authored volume times what the game last set, as here), the
// distance and angle curves, the sharing between the ears, the rule for starting. What still
// differs is not found. Chris, 2026-10-13, with ours beside the original: at the zoom where a
// commander is a thumbnail the hums "should be pretty much unheard"; only with the camera
// right at the building "even slightly at all", and then "like 2-3 out of 100. A very very
// subtle noise"; guns are right as they are. So, until the cause is found, for these
// categories only: 16 dB less, and they run only while the camera's eye is within kHumWithin
// of the place it looks at (they end beyond kHumEnds).
const float kHumTrim = -16.0f;
const float kHumWithin = 40.0f, kHumEnds = 50.0f;

// How loud a sound of the world is, as read from the original (FA exe):
// - The listener's place is the place it is given with its height 4 lower
//   (AudioEngine::SetListenerTransform, 0x4d9890). "Distance" is from there to the sound (the
//   sound library's own measure, handed on at 0x4d82ca).
// - "Angle" (set for every running sound, 0x8aa57f..0x8aa5e7, and for every sound played once,
//   0x8ac5d1..0x8ac65e): with v from the listener to the sound, a quarter turn less the angle
//   by which v points DOWN, so 0 for a sound straight below the listener, 90 for one level with
//   it, 180 for one straight above. The original takes the arc cosine of v's upward share by a
//   short formula (0x50b710) that is only good for what lies above: for a sound straight below
//   it gives 28 degrees and not 0. That is kept, formula and all: by the game's curve (0 dB at
//   0 and at 180, -6 at 45, -20 at 90) a sound in the middle of the view loses about 4 dB and
//   one off to the side more. (The constant the original takes off the arc cosine is a quarter
//   turn: 0xfb7914, set at start as pi x 0.5, 0xbedc60.)
// - "CameraDistance" is how far the camera's eye is from the place it looks at (0x8ac226: the
//   camera's LODMetric of its own target).
// - A unit's running sound STARTS only while that camera distance is at most 200 and the unit
//   is among those the camera has in view (0x8ac9e4 .. 0x8acbbf, over
//   CameraImpl::GetAllSoundEntitiesInFrustum); once it runs it goes on until its sound is
//   filtered (the place no longer seen, 0x8abba0) or ended. See MayStartLoop.
// INFERRED, not read: that the place given to the listener is the camera's EYE. The call that
// hands it over was not found. (For some hours of 2026-10-13 this took the place the camera
// looks at instead: wrong, the camera's height then made no difference, and Chris heard at
// once that it does in the original.)
float SoundSystem::Heard(const std::vector<uint32_t>& curves, const float position[3], float* left, float* right) const {
  if (!listening_) {
    *left = *right = 0;
    return -96.0f;
  }
  float d[3] = {position[0] - eye_[0], position[1] - (eye_[1] - 4.0f), position[2] - eye_[2]};
  float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  float angle = 180.0f;
  if (distance > 0) {
    const float x = d[1] / distance;
    const float arc = std::sqrt(std::max(0.0f, 1.0f - x)) * (1.5707288f + x * (-0.2121144f + x * (0.0742610f + x * -0.0187293f)));
    angle = (3.14159265f - arc) * 57.29578f;
  }
  float e[3] = {focus_[0] - eye_[0], focus_[1] - eye_[1], focus_[2] - eye_[2]};
  float eyeDistance = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  float decibels = 0, ducked = 0;
  for (uint32_t offset : curves) {
    auto it = curves_.find(offset);
    if (it == curves_.end() || it->second.parameter != 0) continue;
    if (it->second.variable == "Distance") decibels += it->second.At(distance);
    else if (it->second.variable == "Angle") decibels += it->second.At(angle);
    else if (it->second.variable == "CameraDistance") decibels += it->second.At(eyeDistance);
    else if (it->second.variable == "Duck") ducked += it->second.At(duck_);
  }
  // Between the ears by where it is on screen: at the screen's edge about two thirds to one side. (Ours.)
  // The two ears SHARE the sound: one in the middle has half in each, as a sound placed in the
  // world has in the original (its sound library spreads one channel over the two speakers; the
  // game's own curve for that spreading is flat, 0xf61398, so nothing else is taken off there).
  // INFERRED that the share is half and half, not read: the library is not part of the game's
  // program; Chris, 2026-10-13, hearing ours beside the original with a full share in each ear:
  // "cut it in half and I think we got the og sound".
  float side = ((position[0] - focus_[0]) * right_[0] + (position[2] - focus_[2]) * right_[2]) / std::max(1.0f, zoom_ * 0.5f);
  side = std::max(-1.0f, std::min(1.0f, side));
  float gain = Linear(decibels + ducked);  // (the duck makes it softer; whether it is heard at all goes by the rest)
  *left = gain * 0.5f * (1.0f - side * 0.65f);
  *right = gain * 0.5f * (1.0f + side * 0.65f);
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

  // A category that is full: the new sound takes the oldest one's place, or is not played. The
  // old one goes over the category's fade out (the music's 200 ms; none: at once). (The new one's
  // fade in is not built: only the Ambient category has one, and it has no limit.)
  if (category >= 0) {
    const Category& cat = categories_[static_cast<size_t>(category)];
    int oldest = 0;
    if (cat.maxInstances < 255 && mixer_.CountOf(category, &oldest) >= cat.maxInstances) {
      if (!cat.replaceOldest) return 0;
      mixer_.FadeOut(oldest, static_cast<float>(cat.fadeOutMs));
    }
  }

  // The sound's loudness by time (see TimeCurves): its own curves by "AttackTime" and
  // "ReleaseTime" and its tracks'. A soft stop lasts until the furthest end of its TRACKS' release
  // curves (the music's 6.06 s), for all of its tracks; the sound's own release curves count for the
  // loudness but not for the length (FACT reads only the tracks' for it).
  float releaseMs = 0;
  for (const PlayEvent& e : sound.events)
    for (uint32_t offset : e.curves) {
      auto c = curves_.find(offset);
      if (c != curves_.end() && c->second.parameter == 0 && c->second.variable == "ReleaseTime" && !c->second.points.empty())
        releaseMs = std::max(releaseMs, std::floor(c->second.points.back().first));
    }
  auto timeCurves = [&](const PlayEvent& e) -> std::shared_ptr<const TimeCurves> {
    auto t = std::make_shared<TimeCurves>();
    t->releaseMs = releaseMs;
    t->attackFromMs = static_cast<float>(std::max(0, e.timeMs - e.trackStartMs));
    auto add = [&](uint32_t offset) {
      auto c = curves_.find(offset);
      if (c == curves_.end() || c->second.parameter != 0) return;
      if (c->second.variable == "AttackTime") t->attack.push_back(c->second);
      else if (c->second.variable == "ReleaseTime") t->release.push_back(c->second);
    };
    for (uint32_t offset : sound.curves) add(offset);
    for (uint32_t offset : e.curves) add(offset);
    if (t->attack.empty() && t->release.empty() && t->releaseMs <= 0) return nullptr;
    return t;
  };

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
    if (position && IsHumCategory(category)) decibels += kHumTrim;  // (set by ear: see kHumTrim)
    float cents = static_cast<float>(sound.pitch) + between(static_cast<float>(e.pitchMin), static_cast<float>(e.pitchMax));
    int voice = mixer_.Add(wave, Linear(decibels), std::pow(2.0f, cents / 1200.0f), e.loops == 255 ? -1 : e.loops, category, e.timeMs);
    if (!voice) continue;
    if (auto t = timeCurves(e)) mixer_.SetTimeCurves(voice, std::move(t));
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
  // (With MOHO64UI_SOUND_LOG: every sound that is not of the world, the interface's own, the
  // music and the scripts' PlaySound, by its cue, category and wave.)
  if (!position && std::getenv("MOHO64UI_SOUND_LOG"))
    Logf(LogLevel::Info, "moho64ui: sound: plain %s/%s, category %s, %.1f dB, wave %s%s", bankName.c_str(), cueName.c_str(),
         category >= 0 ? categories_[static_cast<size_t>(category)].name.c_str() : "none", sound.volume, lastWave_.c_str(),
         prepareOnly ? " (prepared)" : "");
  // A sound that is not placed in the world but follows the camera's distance (the game's
  // curve by "CameraDistance": the planet's rumble that the game starts with, full from far up
  // and 20 dB less from near) or the duck (the menu's ambient loop) is kept, to be set again as
  // the camera moves and the duck goes.
  if (!position) {
    std::vector<uint32_t> byCamera;
    for (uint32_t offset : sound.curves) {
      auto curve = curves_.find(offset);
      if (curve != curves_.end() && curve->second.parameter == 0 &&
          (curve->second.variable == "CameraDistance" || curve->second.variable == "Duck"))
        byCamera.push_back(offset);
    }
    if (!byCamera.empty()) {
      byCamera_[handle] = byCamera;
      const float g = Linear(ByCamera(byCamera));
      for (int v : voices) mixer_.Place(v, g, g);
    }
  }
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

// What the game's curves by "CameraDistance" and "Duck" give now (decibels). The camera's
// distance is to the place it looks at (0x8ac226); before a game's camera has said where it is,
// the value the game's settings file starts it with (576.76: about -12 dB by the one curve there is).
float SoundSystem::ByCamera(const std::vector<uint32_t>& curves) const {
  float distance = 576.76f;
  if (listening_) {
    const float e[3] = {focus_[0] - eye_[0], focus_[1] - eye_[1], focus_[2] - eye_[2]};
    distance = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  }
  float decibels = 0;
  for (uint32_t offset : curves) {
    auto it = curves_.find(offset);
    if (it != curves_.end()) decibels += it->second.At(it->second.variable == "Duck" ? duck_ : distance);
  }
  return decibels;
}

// Every playing sound that follows the camera or the duck, and every playing sound of the world,
// gets its loudness (and its side) again.
void SoundSystem::ApplyGains() {
  for (auto it = byCamera_.begin(); it != byCamera_.end();) {
    auto voices = handles_.find(it->first);
    bool alive = false;
    const float g = Linear(ByCamera(it->second));
    if (voices != handles_.end())
      for (int v : voices->second) {
        if (!mixer_.Alive(v)) continue;
        alive = true;
        mixer_.Place(v, g, g);
      }
    it = alive ? std::next(it) : byCamera_.erase(it);
  }
  for (auto it = placed_.begin(); it != placed_.end();) {
    auto voices = handles_.find(it->first);
    bool alive = false;
    float left = 1, right = 1;
    Heard(it->second.curves, it->second.position, &left, &right);
    if (voices != handles_.end())
      for (int v : voices->second) {
        if (!mixer_.Alive(v)) continue;
        alive = true;
        mixer_.Place(v, left, right);
      }
    it = alive ? std::next(it) : placed_.erase(it);
  }
}

// The sounds' loudness again (ApplyGains); then a running sound of the world that has come into
// hearing starts, one that has gone out of it ends.
void SoundSystem::UpdateWorld() {
  ApplyGains();
  for (auto& entry : loops_) {
    Loop& loop = entry.second;
    auto placed = placed_.find(loop.handle);
    if (loop.hum < 0) loop.hum = IsHumCategory(CategoryOfCue(loop.bank, loop.cue)) ? 1 : 0;
    const float e[3] = {focus_[0] - eye_[0], focus_[1] - eye_[1], focus_[2] - eye_[2]};
    const float eyeDistance = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (placed == placed_.end()) {
      const bool may = MayStartLoop(loop.position) && !(loop.hum == 1 && eyeDistance > kHumWithin);
      loop.handle = may ? PlayCue(loop.bank, loop.cue, false, loop.position) : 0;  // 0 while it cannot be heard
      continue;
    }
    float left = 1, right = 1;
    if (Heard(placed->second.curves, loop.position, &left, &right) < kHeardFrom || (loop.hum == 1 && eyeDistance > kHumEnds)) {
      Stop(loop.handle, false);
      placed_.erase(placed);
      loop.handle = 0;
    }
  }
}

// The category of a cue's first sound (-1: none), and whether a category is one of the game's
// "ActiveLoops...": the hums of working buildings (see kHumTrim).
int SoundSystem::CategoryOfCue(const std::string& bankName, const std::string& cueName) {
  Bank* bank = GetBank(bankName);
  if (!bank) return -1;
  auto it = bank->info.cues.find(Lower(cueName));
  if (it == bank->info.cues.end() || it->second.sounds.empty()) return -1;
  int sound = it->second.sounds[0];
  if (sound < 0 || sound >= static_cast<int>(bank->info.sounds.size())) return -1;
  int category = bank->info.sounds[static_cast<size_t>(sound)].category;
  return category < static_cast<int>(categories_.size()) ? category : -1;
}
bool SoundSystem::IsHumCategory(int category) const {
  return category >= 0 && category < static_cast<int>(categories_.size()) &&
         categories_[static_cast<size_t>(category)].name.compare(0, 11, "ActiveLoops") == 0;
}

// A unit's running sound may start only while the camera is near and has the unit in view (see
// at Heard). "In view" is taken here as within six tenths of the shown width of the place the
// camera looks at: ours, in place of the original's list of what the camera's view holds.
bool SoundSystem::MayStartLoop(const float position[3]) const {
  if (!listening_) return false;
  const float e[3] = {focus_[0] - eye_[0], focus_[1] - eye_[1], focus_[2] - eye_[2]};
  if (std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) > 200.0f) return false;
  const float dx = position[0] - focus_[0], dz = position[2] - focus_[2];
  return std::sqrt(dx * dx + dz * dz) <= std::max(1.0f, zoom_ * 0.6f);
}

void SoundSystem::Start(int handle) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  for (int v : it->second) mixer_.Start(v);
}

// Stopped at once, a ducking voice lets go of the duck at once (FA exe 0x8ab471: only one that
// ducked); stopped softly, when its sound has ended (Update).
void SoundSystem::Stop(int handle, bool immediate) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  for (int v : it->second) mixer_.Stop(v, immediate);
  auto ducking = std::find(ducking_.begin(), ducking_.end(), handle);
  if (immediate && ducking != ducking_.end()) ducking_.erase(ducking), PopDuck(false);
}

// Everything stops, and the duck is gone at once (the original's StopAllSounds, 0x8ab624).
void SoundSystem::StopAll() {
  mixer_.StopAll();
  ducking_.clear();
  PopDuck(true);
}

void SoundSystem::Duck(int handle) {
  if (!handle) return;
  if (duckCount_ == 0 && duckLength_ > 0) duckState_ = 1, duckTime_ = 0;  // (PushDuck, 0x8ab020: the next frame moves it)
  ++duckCount_;
  ducking_.push_back(handle);
}

// One ducking voice less (PopDuck, 0x8ab070): with the last one the duck comes back over its
// length. `all`: none at once, the duck gone (the original's StopAllSounds and SetVolume).
void SoundSystem::PopDuck(bool all) {
  if (all) {
    duckCount_ = 0, duckState_ = 0;
    if (duck_ != 0) duck_ = 0, ApplyGains();
    return;
  }
  if (duckCount_ == 0) return;
  if (--duckCount_ == 0 && duckLength_ > 0) duckState_ = 2, duckTime_ = 0;
}

// As the original's frame (CUserSoundManager::UpdateDuck 0x8ab670, then its loop over the
// scripts' sounds): the ramp first, straight in time; then the ducking voices that have ended let
// go, so the duck comes back from the next frame. A push while it comes back starts it down again
// from nothing, and a pop while it goes down starts it back from full: so the original.
void SoundSystem::Update(float seconds) {
  if (duckState_ != 0) {
    duckTime_ = std::min(duckTime_ + std::max(0.0f, seconds), duckLength_);
    const float v = duckTime_ / duckLength_;
    duck_ = duckState_ == 2 ? 1 - v : v;
    if (duckTime_ >= duckLength_) duckState_ = 0;
    ApplyGains();
  }
  for (auto it = ducking_.begin(); it != ducking_.end();) {
    if (Playing(*it)) {
      ++it;
      continue;
    }
    it = ducking_.erase(it);
    PopDuck(false);
  }
}

bool SoundSystem::Playing(int handle) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return false;
  for (int v : it->second)
    if (mixer_.Alive(v)) return true;
  return false;
}

bool SoundSystem::Placement(int handle, float* left, float* right) {
  auto it = handles_.find(handle);
  if (it == handles_.end()) return false;
  for (int v : it->second)
    if (mixer_.PlaceOf(v, left, right)) return true;
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

// (As the original, 0x8aaf60: setting a category's volume ends the duck at once, even for a
// category that is not there; the voices that ducked then let go of nothing.)
bool SoundSystem::SetVolume(const std::string& category, float volume) {
  PopDuck(true);
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
