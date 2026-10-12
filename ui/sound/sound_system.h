// The game's sounds for the interface: cues of the game's sound banks, played through the mixer.
//
// A script plays a cue of a bank (PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown"})));
// the cue picks one of its sounds, the sound plays one wave or several, each from a wave bank,
// with the sound's volume and pitch, inside its category. Categories have a volume of their own
// (from the game's SupCom.xgs) and one the player sets (SetVolume), and stand inside each other:
// "Global" holds all, "Interface" holds "Unit Select", and so on.
//
// Banks are read from /sounds of the game's file system when a cue of theirs is first asked for.
// A wave bank made for streaming (the music) is read from its file as it plays; the others are
// taken into memory whole.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sound/device.h"
#include "sound/mixer.h"
#include "sound/xact.h"

namespace moho {
class Vfs;
}

namespace moho::ui::sound {

class SoundSystem {
 public:
  explicit SoundSystem(const Vfs* vfs);
  ~SoundSystem();

  // Play a cue; 0 when there is no such cue (logged once per cue). With `prepareOnly` the sound
  // waits for Start. The number stands for the cue's playing: Stop and Playing take it.
  int Play(const std::string& bank, const std::string& cue, bool prepareOnly = false);
  void Start(int handle);
  void Stop(int handle, bool immediate);
  void StopAll();
  bool Playing(int handle);

  // A category's volume as the player sets it (1: as the game's files have it); false when there
  // is no category of that name. Pausing holds every sound of the category and of those inside.
  bool SetVolume(const std::string& category, float volume);
  bool GetVolume(const std::string& category, float* volume) const;
  bool Pause(const std::string& category, bool pause);
  const std::vector<Category>& categories() const { return categories_; }

  // --- the world's sounds: what the simulation's scripts play at a place ---
  // Where the listener is: the camera's eye, the place it looks at, the direction to the right of
  // the screen along the ground, and its zoom (the width of world it shows). Told every frame; the
  // world's sounds then get louder, softer and move between the ears as the camera moves. How loud
  // a sound is at a distance is its own: the game's files give each sound curves by "Distance"
  // (from the eye to the sound) and by "CameraDistance" (from the eye to where it looks).
  void SetListener(const float eye[3], const float focus[3], const float right[3], float zoom);
  // A sound played once at a place. 0 when it is not heard from here (too far), or there is no such cue.
  int PlayAt(const std::string& bank, const std::string& cue, const float position[3]);
  // The running sound of an entity (its engine, its hum): set, moved with it, ended with an empty
  // cue. It plays only while it can be heard from where the listener is.
  void SetLoop(uint32_t entity, const std::string& bank, const std::string& cue, const float position[3]);
  void MoveLoop(uint32_t entity, const float position[3]);
  bool HasLoops() const { return !loops_.empty(); }
  void StopWorld();
  int worldPlayed() const { return worldPlayed_; }  // the world's sounds that were heard, loops counted when they start

  // The language of the voices (AudioSetLanguage): their banks are in /sounds/voice/<language>.
  void SetLanguage(const std::string& name);

  // Open the sound device and play; without it the sounds are mixed only when Mix is asked
  // (tests). False when there is no device.
  bool OpenDevice();
  Mixer& mixer() { return mixer_; }

  // For tests and the log: how many cues were played, and the last one's wave ("Interface#11").
  int played() const { return played_; }
  const std::string& lastWave() const { return lastWave_; }

 private:
  struct WaveBank {
    WaveBankInfo info;
    std::string hostPath;                         // streaming: the file it is read from
    std::string data;                             // else: the whole file
    std::map<int, std::shared_ptr<WaveData>> waves;  // those used so far (in memory ones)
    bool ok = false;
  };
  struct Bank {
    SoundBankInfo info;
    bool ok = false;
  };
  Bank* GetBank(const std::string& name);
  WaveBank* GetWaveBank(const std::string& name);
  std::shared_ptr<WaveData> GetWave(WaveBank* bank, int index);
  struct Placed {  // a playing sound of the world
    float position[3] = {0, 0, 0};
    std::vector<uint32_t> curves;
  };
  struct Loop {
    std::string bank, cue;
    float position[3] = {0, 0, 0};
    int handle = 0;
  };
  int PlayCue(const std::string& bank, const std::string& cue, bool prepareOnly, const float* position);
  // A world sound's loudness (decibels) and its share for each ear, from the listener's place.
  float Heard(const std::vector<uint32_t>& curves, const float position[3], float* left, float* right) const;
  void UpdateWorld();
  int CategoryByName(const std::string& name) const;
  std::string Find(const std::string& file) const;
  void UpdateCategories();
  uint32_t Random();

  const Vfs* vfs_;
  std::vector<Category> categories_;
  std::vector<float> userVolume_;
  std::vector<bool> paused_;
  std::map<std::string, std::unique_ptr<Bank>> banks_;
  std::map<std::string, std::unique_ptr<WaveBank>> waveBanks_;
  std::map<int, std::vector<int>> handles_;  // a playing cue's voices
  std::map<uint32_t, Curve> curves_;         // the global settings' curves, by offset
  std::map<int, Placed> placed_;             // the world's playing sounds, by handle
  std::map<uint32_t, Loop> loops_;           // the entities' running sounds
  float eye_[3] = {0, 0, 0}, focus_[3] = {0, 0, 0}, right_[3] = {1, 0, 0}, zoom_ = 100;
  bool listening_ = false;  // SetListener was called: before that nothing of the world is heard
  int worldPlayed_ = 0;
  std::map<std::string, bool> complained_;
  Mixer mixer_;
  std::unique_ptr<OutputDevice> device_;
  int nextHandle_ = 1;
  int played_ = 0;
  std::string lastWave_;
  std::string language_ = "us";
  uint32_t seed_ = 0x2545f491u;
};

}  // namespace moho::ui::sound
