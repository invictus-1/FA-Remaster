// The game's sounds for the interface: cues of the game's sound banks, played through the mixer.
//
// A script plays a cue of a bank (PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown"})));
// the cue picks one of its sounds, the sound plays one wave or several, each from a wave bank,
// with the sound's volume and pitch, inside its category. Categories have a volume of their own
// (from the game's SupCom.xgs) and one the player sets (SetVolume), and stand inside each other:
// "Global" holds all, "Interface" holds "Unit Select", and so on.
//
// The banks of /sounds of the game's file system (and of the voices' folder of the language) are
// taken when the sound system starts, as the original's (sound_system.cpp, at LoadFolder): the
// sound banks read, the wave banks mapped into memory; a wave bank made for streaming (the music)
// is read from its file as it plays.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
  // A voice that ducks the other sounds while it plays (PlayVoice with its second argument).
  // As the original's sound manager (FA exe, CUserSoundManager): the game ramps one variable of
  // its sound settings, "Duck", from 0 to 1 over "DuckLength" seconds (0.5 in SupCom.xgs) when
  // the first ducking voice starts, and back to 0 over the same time when the last one ends. The
  // cut itself is the settings' curve by "Duck" (down to 10 dB less), which the sound banks give
  // nearly every sound of the world and a few ambient loops; music, voices and the interface's
  // sounds have none and so are not ducked.
  void Duck(int handle);
  // Every frame, with the real seconds since the last one (menus too; the game's pause does not
  // hold it): the duck's ramp moves on, and ducking voices that have ended let go of it.
  void Update(float seconds);
  float duck() const { return duck_; }
  void Start(int handle);
  void Stop(int handle, bool immediate);
  void StopAll();
  bool Playing(int handle);
  // False while a wave of the cue is not yet in memory to be read (SoundIsPrepared).
  bool Prepared(int handle);
  // Where a playing cue's first voice is set among the ears (its loudness by its curves and the
  // duck, on top of its own); false when it plays no more. (For tests.)
  bool Placement(int handle, float* left, float* right);

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
  // (from the listener's place to the sound), by "Angle" (how far to the side of it) and by
  // "CameraDistance" (from the eye to where it looks). The listener's place is the camera's
  // eye, 4 lower (sound_system.cpp, at Heard, says what of that is read).
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
  // Whether the game has voices in a language (the folder /sounds/voice/<language>).
  bool HasLanguage(const std::string& name) const;

  // Open the sound device and play; without it the sounds are mixed only when Mix is asked
  // (tests). False when there is no device.
  bool OpenDevice();
  Mixer& mixer() { return mixer_; }

  // For tests and the log: how many cues were played, and the last one's wave ("Interface#11").
  int played() const { return played_; }
  const std::string& lastWave() const { return lastWave_; }

  // --- the waves' bytes ---
  // A wave of a mapped bank is touched (its pages brought from the disk) the first time a cue plays
  // it: with touching on a thread on (a live window), on the sound system's own thread, the wave
  // ready to be mixed once its first part is in; off (the tests, without a window), there and then.
  void SetTouchOnThread(bool on) { touchOnThread_ = on; }
  bool touchOnThread() const { return touchOnThread_; }
  void WaitForTouches();  // (tests: until the thread has touched all it was given)
  // For tests and the log: the wave banks mapped and their files' megabytes, those streamed, those
  // read whole instead (a file that could not be mapped), the megabytes of waves touched on the
  // thread and on the calling thread, and the longest a cue's play took on the calling thread.
  struct BankStats {
    int mapped = 0, streamed = 0, readWhole = 0;
    double mappedMB = 0, readWholeMB = 0, touchedMB = 0, touchedHereMB = 0, longestPlayMs = 0;
  };
  BankStats bankStats() const;
  void ResetLongestPlay() { longestPlayMs_ = 0; }

 private:
  struct WaveBank {
    WaveBankInfo info;
    std::string hostPath;                         // streaming: the file it is read from
    std::shared_ptr<const MappedFile> file;       // else: the file, mapped (or read whole)
    std::map<int, std::shared_ptr<WaveData>> waves;  // those used so far (in memory ones)
    bool ok = false;
    bool voice = false;                           // of the voices' folder of the language
  };
  struct Bank {
    SoundBankInfo info;
    bool ok = false;
    bool voice = false;
  };
  Bank* GetBank(const std::string& name);
  WaveBank* GetWaveBank(const std::string& name);
  Bank* LoadBank(const std::string& key, const std::string& path, bool quiet = false);
  WaveBank* LoadWaveBank(const std::string& key, const std::string& path, bool quiet = false);
  void LoadFolder(const std::string& folder);  // every bank of a folder (not of folders in it) not taken yet
  std::shared_ptr<WaveData> GetWave(WaveBank* bank, int index);
  size_t TouchWave(MappedWave& wave);  // (its pages brought in; ready after its first part; bytes touched)
  void TouchLoop();
  struct Placed {  // a playing sound of the world
    float position[3] = {0, 0, 0};
    std::vector<uint32_t> curves;
  };
  struct Loop {
    std::string bank, cue;
    float position[3] = {0, 0, 0};
    int handle = 0;
    int hum = -1;  // 1: a working building's hum (the "ActiveLoops" categories), 0: not, -1: not looked up yet
  };
  int PlayCue(const std::string& bank, const std::string& cue, bool prepareOnly, const float* position);
  // A world sound's loudness (decibels) and its share for each ear, from the listener's place.
  float Heard(const std::vector<uint32_t>& curves, const float position[3], float* left, float* right) const;
  void UpdateWorld();
  bool MayStartLoop(const float position[3]) const;
  float ByCamera(const std::vector<uint32_t>& curves) const;
  void ApplyGains();
  void PopDuck(bool all);
  int CategoryOfCue(const std::string& bank, const std::string& cue);
  bool IsHumCategory(int category) const;
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
  std::map<int, std::vector<uint32_t>> byCamera_;  // sounds not placed that follow the camera's distance or the duck: those curves
  std::map<uint32_t, Loop> loops_;           // the entities' running sounds
  float eye_[3] = {0, 0, 0}, focus_[3] = {0, 0, 0}, right_[3] = {1, 0, 0}, zoom_ = 100;
  bool listening_ = false;  // SetListener was called: before that nothing of the world is heard
  // The duck (see Duck): its value now (0..1), its ramp (0 still, 1 going down, 2 coming back),
  // the seconds into the ramp, the ramp's length, how many ducking voices hold it, and those voices.
  float duck_ = 0, duckTime_ = 0, duckLength_ = 0.5f;
  int duckState_ = 0, duckCount_ = 0;
  std::vector<int> ducking_;
  int worldPlayed_ = 0;
  std::map<std::string, bool> complained_;
  Mixer mixer_;
  std::unique_ptr<OutputDevice> device_;
  int nextHandle_ = 1;
  int played_ = 0;
  std::string lastWave_;
  std::string language_ = "us";
  uint32_t seed_ = 0x2545f491u;

  bool touchOnThread_ = false;
  bool voicesTaken_ = false;  // the voices' banks of language_ taken (SetLanguage)
  double longestPlayMs_ = 0;
  int banksMapped_ = 0, banksStreamed_ = 0, banksReadWhole_ = 0;
  int64_t bytesMapped_ = 0, bytesReadWhole_ = 0, bytesTouchedHere_ = 0;
  std::atomic<int64_t> bytesTouched_{0};
  // The thread that touches the waves' pages (made at the first wave given), and its line of waves.
  std::thread toucher_;
  std::mutex touchMutex_;
  std::condition_variable touchWake_, touchDone_;
  std::deque<std::shared_ptr<MappedWave>> touchQueue_;
  bool touchQuit_ = false, touchBusy_ = false;
};

}  // namespace moho::ui::sound
