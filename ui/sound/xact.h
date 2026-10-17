// The game's sound files, as the original's sound library (XACT, content version 43) wrote them:
//   <name>.xwb  a wave bank: the sound data, a list of waves with their formats;
//   <name>.xsb  a sound bank: cues by name; a cue plays one of its sounds, a sound plays waves
//               of wave banks with a volume, a pitch and a category;
//   SupCom.xgs  the global settings: the categories ("Music", "Interface", "VO", ...) with their
//               volumes, each inside a parent category.
// Nothing here is read from the original's code: the layouts are the files' own, checked by their
// offsets and lengths (every sound entry of every bank of the game is consumed to its last byte).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace moho::ui::sound {

struct WaveEntry {
  int tag = 0;         // 0 PCM (all of the game's waves), 1 XMA, 2 ADPCM, 3 WMA
  int channels = 1;
  int rate = 32000;    // samples per second
  int bits = 16;       // per sample
  uint64_t offset = 0; // of its data in the file
  uint32_t length = 0; // of its data, bytes
};
struct WaveBankInfo {
  std::string name;
  bool streaming = false;  // made to be read piece by piece (the music)
  std::vector<WaveEntry> entries;
};
// `data` needs to hold the file's head only (to the end of the list of waves).
bool ParseWaveBank(const uint8_t* data, size_t size, WaveBankInfo* out);

// One wave a sound can play: wave bank (index into the sound bank's list), wave, and its weight
// when the sound picks among several.
struct WaveChoice {
  int bank = 0, wave = 0, weight = 1;
};
// A "play wave" event of a sound: one of `waves`, repeated `loops` more times (255: without end),
// with a pitch (hundredths of a semitone) and a volume (decibels) picked from a range.
struct PlayEvent {
  std::vector<WaveChoice> waves;
  int loops = 0;
  int timeMs = 0;  // after the sound's start
  int pitchMin = 0, pitchMax = 0;
  float volumeMin = 0, volumeMax = 0;
  std::vector<uint32_t> curves;  // its track's own curves (offsets in the global settings): the
                                 // fades by "AttackTime" and "ReleaseTime" of the units' loops and the music
};
struct SoundDef {
  int category = 0;
  float volume = 0;  // decibels
  int pitch = 0;
  int priority = 0;
  std::vector<PlayEvent> events;
  std::vector<uint32_t> curves;  // the volume curves it follows: their offsets in the global settings
};
struct CueDef {
  std::string name;
  std::vector<int> sounds;   // indices into SoundBankInfo::sounds
  std::vector<int> weights;  // for the pick among them
};
struct SoundBankInfo {
  std::string name;
  std::vector<std::string> waveBanks;
  std::vector<SoundDef> sounds;
  std::map<std::string, CueDef> cues;  // by lower-case name
};
bool ParseSoundBank(const uint8_t* data, size_t size, SoundBankInfo* out, std::string* error);

struct Category {
  std::string name;
  int parent = -1;
  float volume = 0;  // decibels
  int maxInstances = 255;      // sounds of it at a time (255: no limit)
  bool replaceOldest = false;  // at the limit a new sound takes the oldest one's place (else it is not played)
  int fadeInMs = 0, fadeOutMs = 0;  // over how long the new one comes in and the old one goes when it takes its place
};
bool ParseGlobalSettings(const uint8_t* data, size_t size, std::vector<Category>* out);

// A curve of the global settings: a sound's loudness (hundredths of a decibel) by a variable the
// game sets, such as "Distance" (from the listener to the sound). Straight lines between points.
struct Curve {
  std::string variable;
  int parameter = 0;  // 0: volume
  std::vector<std::pair<float, float>> points;
  float At(float x) const;  // in decibels
};
// The curves by their offset in the file (sounds name them so).
bool ParseCurves(const uint8_t* data, size_t size, std::map<uint32_t, Curve>* out);

// The global settings' variables by name, each with the value it starts at ("DuckLength": 0.5).
bool ParseVariables(const uint8_t* data, size_t size, std::map<std::string, float>* out);

// The files' volume byte in decibels (0 = -96, 180 = 0, 255 = +6). A curve fitted to the sound
// tool's own table, as other readers of these files use it.
float VolumeByteToDecibels(uint8_t b);

}  // namespace moho::ui::sound
