#include "sound/xact.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace moho::ui::sound {
namespace {

// A reader that never leaves the data: past the end it gives zeros and remembers.
struct Reader {
  const uint8_t* d;
  size_t size, o;
  bool bad = false;
  Reader(const uint8_t* data, size_t n, size_t at = 0) : d(data), size(n), o(at) {}
  bool Has(size_t n) {
    if (o > size || size - o < n) bad = true;
    return !bad;
  }
  uint8_t U8() {
    if (!Has(1)) return 0;
    return d[o++];
  }
  uint16_t U16() {
    if (!Has(2)) return 0;
    uint16_t v = static_cast<uint16_t>(d[o] | d[o + 1] << 8);
    o += 2;
    return v;
  }
  int16_t S16() { return static_cast<int16_t>(U16()); }
  uint32_t U32() {
    if (!Has(4)) return 0;
    uint32_t v = static_cast<uint32_t>(d[o]) | static_cast<uint32_t>(d[o + 1]) << 8 | static_cast<uint32_t>(d[o + 2]) << 16 |
                 static_cast<uint32_t>(d[o + 3]) << 24;
    o += 4;
    return v;
  }
  void Skip(size_t n) {
    if (Has(n)) o += n;
  }
  // A text of at most `n` bytes in a field of `n`, ended by a zero.
  std::string Text(size_t n) {
    if (!Has(n)) return std::string();
    size_t len = 0;
    while (len < n && d[o + len]) ++len;
    std::string s(reinterpret_cast<const char*>(d + o), len);
    o += n;
    return s;
  }
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const int kVersion = 43;

}  // namespace

float VolumeByteToDecibels(uint8_t b) {
  if (b == 0) return -96.0f;
  const double a = -96.0, e = 0.432254984608615, c = 80.1748600297963, d = 67.7385212334047;
  return static_cast<float>((a - d) / (1.0 + std::pow(b / c, e)) + d);
}

// "WBND", version, header version, five segments (offset, length): bank data, the waves' entries,
// seek tables, names, wave data. Bank data: flags (bit 0: streaming), wave count, name[64], entry
// size, name size, alignment, compact format, build time. An entry: flags and duration, format
// (tag 2 bits, channels 3, rate 18, block align 8, 16-bit 1), data offset and length (in the wave
// data segment), loop start and length.
bool ParseWaveBank(const uint8_t* data, size_t size, WaveBankInfo* out) {
  Reader r(data, size);
  if (size < 12 + 40 || std::memcmp(data, "WBND", 4) != 0) return false;
  r.Skip(4);
  r.U32(), r.U32();
  uint32_t seg[5][2];
  for (auto& s : seg) s[0] = r.U32(), s[1] = r.U32();
  r.o = seg[0][0];
  uint32_t flags = r.U32(), count = r.U32();
  out->name = r.Text(64);
  uint32_t entrySize = r.U32();
  r.U32(), r.U32();
  uint32_t compact = r.U32();
  if (r.bad || compact || entrySize < 24 || count > 100000) return false;
  out->streaming = (flags & 1) != 0;
  out->entries.clear();
  for (uint32_t i = 0; i < count; ++i) {
    r.o = seg[1][0] + static_cast<size_t>(i) * entrySize;
    r.U32();
    uint32_t format = r.U32(), offset = r.U32(), length = r.U32();
    if (r.bad) return false;
    WaveEntry e;
    e.tag = static_cast<int>(format & 3);
    e.channels = static_cast<int>((format >> 2) & 7);
    e.rate = static_cast<int>((format >> 5) & 0x3ffff);
    e.bits = (format >> 31) ? 16 : 8;
    e.offset = static_cast<uint64_t>(seg[4][0]) + offset;
    e.length = length;
    out->entries.push_back(e);
  }
  return true;
}

// "SDBK", version, tool version, checksum, time, platform; counts (simple cues, complex cues, two
// unknown, wave banks, sounds), the cue names' length; offsets (simple cues, complex
// cues, cue names, unknown, variations, unknown, wave bank names, two hash tables, sounds); the
// bank's name[64].
//   A sound: flags, category, volume byte, pitch, priority, the entry's length; bit 0 of the
// flags: complex (a track count; else one wave: wave, wave bank); bits 1-3: a block of volume
// curves (its length first); bit 4: a block of effect presets (the same); then for a complex
// sound each track's volume byte and the offset of its events.
//   A track's events: a count, then each: 32 bits (type in the low 5, time in the next 16), a
// random offset, the byte 0xff, and by type:
//   1  play a wave:   flags, wave, wave bank, loop count, pan angle, pan arc
//   3  one of several: flags, loop count, pan angle, pan arc; count, variation; 4 bytes; then
//                     per wave: wave, wave bank, weight from, weight to
//   4  as 1, then:    pitch from, pitch to, volume byte from, volume byte to, a flags byte
//   6  as 3, with the five values of 4 before the count
// (No other type occurs in the game's banks; an unknown one ends the sound's reading.)
//   A simple cue: flags, its sound's offset. A complex cue: flags; bit 2 set: its sound's offset
// and 4 bytes, else the offsets of its variations and of its transitions; then instance limit,
// fade in, fade out, a flags byte. Variations: count, flags (bits 3-5: kind), 4 bytes; kind 0:
// wave, wave bank, weight from, to; kind 1: a sound's offset, weight from, to; kind 3: a sound's
// offset, two floats, 4 bytes; kind 4: wave, wave bank.
bool ParseSoundBank(const uint8_t* data, size_t size, SoundBankInfo* out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (size < 0x8a || std::memcmp(data, "SDBK", 4) != 0) return fail("not a sound bank");
  Reader r(data, size, 4);
  int version = r.U16();
  if (version != kVersion) return fail("a sound bank of another version");
  r.Skip(2 + 2 + 8 + 1);
  uint32_t simpleCount = r.U16(), complexCount = r.U16();
  r.U16(), r.U16();  // (the second is the cue count in most banks, 16 in the music's with its three cues)
  uint32_t cueCount = simpleCount + complexCount, bankCount = r.U8(), soundCount = r.U16(), namesLength = r.U16();
  r.U16();
  uint32_t simpleAt = r.U32(), complexAt = r.U32(), namesAt = r.U32();
  r.U32(), r.U32(), r.U32();
  uint32_t banksAt = r.U32();
  r.U32(), r.U32();
  uint32_t soundsAt = r.U32();
  out->name = r.Text(64);
  if (r.bad) return fail("a damaged head");

  r.o = banksAt;
  out->waveBanks.clear();
  for (uint32_t i = 0; i < bankCount; ++i) out->waveBanks.push_back(r.Text(64));

  // The sounds, one after the other; remembered by offset, since cues name them so.
  std::map<uint32_t, int> soundAt;
  out->sounds.clear();
  size_t at = soundsAt;
  for (uint32_t i = 0; i < soundCount; ++i) {
    Reader s(data, size, at);
    SoundDef def;
    uint8_t flags = s.U8();
    def.category = s.U16();
    def.volume = VolumeByteToDecibels(s.U8());
    def.pitch = s.S16();
    def.priority = s.U8();
    uint32_t length = s.U16();
    if (s.bad || length < 9) return fail("a damaged sound");
    int tracks = 0;
    if (flags & 1) {
      tracks = s.U8();
    } else {
      PlayEvent e;
      WaveChoice w;
      w.wave = s.U16(), w.bank = s.U8();
      e.waves.push_back(w);
      def.events.push_back(e);
    }
    if (flags & 0x0e) {  // the block of curves: its length, then (bit 1) a count and the sound's curves
      size_t mark = s.o;
      size_t length = s.U16();
      if (flags & 0x02) {
        int n = s.U8();
        for (int j = 0; j < n && !s.bad; ++j) def.curves.push_back(s.U32());
      }
      s.o = mark + length;
    }
    if (flags & 0x10) {
      size_t mark = s.o;
      s.o = mark + s.U16();
    }
    std::vector<std::pair<float, uint32_t>> trackList;
    for (int t = 0; t < tracks; ++t) {
      float volume = VolumeByteToDecibels(s.U8());
      trackList.push_back({volume, s.U32()});
    }
    for (const auto& track : trackList) {
      Reader t(data, size, track.second);
      int events = t.U8();
      for (int k = 0; k < events && !t.bad; ++k) {
        uint32_t info = t.U32();
        t.U16(), t.U8();
        int type = static_cast<int>(info & 0x1f);
        PlayEvent e;
        e.timeMs = static_cast<int>((info >> 5) & 0xffff);
        e.volumeMin = e.volumeMax = track.first;
        if (type == 0) {
          t.U8();
          continue;
        }
        if (type != 1 && type != 3 && type != 4 && type != 6) break;
        t.U8();
        WaveChoice one;
        bool several = type == 3 || type == 6;
        if (!several) one.wave = t.U16(), one.bank = t.U8();
        e.loops = t.U8();
        t.U16(), t.U16();
        if (type == 4 || type == 6) {
          e.pitchMin = t.S16(), e.pitchMax = t.S16();
          e.volumeMin = track.first + VolumeByteToDecibels(t.U8());
          e.volumeMax = track.first + VolumeByteToDecibels(t.U8());
          t.U8();
        }
        if (several) {
          int n = t.U16();
          t.U16(), t.U32();
          for (int j = 0; j < n && !t.bad; ++j) {
            WaveChoice w;
            w.wave = t.U16(), w.bank = t.U8();
            int from = t.U8(), to = t.U8();
            w.weight = std::max(1, to - from);
            e.waves.push_back(w);
          }
        } else {
          e.waves.push_back(one);
        }
        if (!t.bad && !e.waves.empty()) def.events.push_back(e);
      }
    }
    soundAt[static_cast<uint32_t>(at)] = static_cast<int>(out->sounds.size());
    out->sounds.push_back(def);
    at += length;
  }

  // The cues' names, in the cues' order: the simple ones first.
  std::vector<std::string> names;
  r.bad = false;
  r.o = namesAt;
  if (!r.Has(namesLength)) return fail("damaged cue names");
  for (size_t p = namesAt, end = namesAt + namesLength; p < end && names.size() < cueCount;) {
    size_t len = 0;
    while (p + len < end && data[p + len]) ++len;
    names.emplace_back(reinterpret_cast<const char*>(data + p), len);
    p += len + 1;
  }
  if (names.size() != cueCount) return fail("damaged cue names");

  // A wave played directly by a cue becomes a sound of its own.
  auto waveSound = [&](int wave, int bank) {
    SoundDef def;
    PlayEvent e;
    WaveChoice w;
    w.wave = wave, w.bank = bank;
    e.waves.push_back(w);
    def.events.push_back(e);
    out->sounds.push_back(def);
    return static_cast<int>(out->sounds.size()) - 1;
  };
  auto addSound = [&](CueDef& cue, uint32_t offset, int weight) {
    auto it = soundAt.find(offset);
    if (it == soundAt.end()) return;
    cue.sounds.push_back(it->second);
    cue.weights.push_back(std::max(1, weight));
  };
  out->cues.clear();
  for (uint32_t i = 0; i < cueCount; ++i) {
    CueDef cue;
    cue.name = names[i];
    if (i < simpleCount) {
      Reader c(data, size, simpleAt + 5 * static_cast<size_t>(i));
      c.U8();
      addSound(cue, c.U32(), 1);
    } else {
      Reader c(data, size, complexAt + 15 * static_cast<size_t>(i - simpleCount));
      uint8_t flags = c.U8();
      if (flags & 4) {
        addSound(cue, c.U32(), 1);
      } else {
        Reader v(data, size, c.U32());
        int n = v.U16(), kind = (v.U16() >> 3) & 7;
        v.U32();
        for (int j = 0; j < n && !v.bad; ++j) {
          if (kind == 0) {
            int wave = v.U16(), bank = v.U8(), from = v.U8(), to = v.U8();
            cue.sounds.push_back(waveSound(wave, bank));
            cue.weights.push_back(std::max(1, to - from));
          } else if (kind == 1) {
            uint32_t offset = v.U32();
            int from = v.U8(), to = v.U8();
            addSound(cue, offset, to - from);
          } else if (kind == 3) {
            uint32_t offset = v.U32();
            v.Skip(12);
            addSound(cue, offset, 1);
          } else if (kind == 4) {
            int wave = v.U16(), bank = v.U8();
            cue.sounds.push_back(waveSound(wave, bank));
            cue.weights.push_back(1);
          } else {
            break;
          }
        }
      }
    }
    out->cues[Lower(cue.name)] = cue;
  }
  return true;
}

// "XGSF", version, tool version, checksum, time, platform; counts (categories, variables, two
// more, curves, two for effects); offsets (categories, variables, two more, ..., the categories'
// names seventh). A category: instance limit, fade in, fade out, a flags byte, parent, volume
// byte, one more byte.
bool ParseGlobalSettings(const uint8_t* data, size_t size, std::vector<Category>* out) {
  if (size < 0x4d || std::memcmp(data, "XGSF", 4) != 0) return false;
  Reader r(data, size, 4 + 2 + 2 + 2 + 8 + 1);
  uint32_t count = r.U16();
  r.Skip(12);
  uint32_t offsets[11];
  for (uint32_t& o : offsets) o = r.U32();
  if (r.bad || count > 4096) return false;
  out->clear();
  size_t name = offsets[6];
  for (uint32_t i = 0; i < count; ++i) {
    Reader c(data, size, offsets[0] + 10 * static_cast<size_t>(i));
    Category cat;
    cat.maxInstances = c.U8();
    c.U16(), c.U16();
    cat.replaceOldest = (c.U8() & 1) != 0;
    cat.parent = c.S16();
    cat.volume = VolumeByteToDecibels(c.U8());
    if (c.bad) return false;
    size_t len = 0;
    while (name + len < size && data[name + len]) ++len;
    cat.name.assign(reinterpret_cast<const char*>(data + std::min(name, size)), name < size ? len : 0);
    name += len + 1;
    out->push_back(cat);
  }
  return true;
}

}  // namespace moho::ui::sound

namespace moho::ui::sound {

float Curve::At(float x) const {
  if (points.empty()) return 0;
  if (x <= points.front().first) return points.front().second / 100.0f;
  for (size_t i = 1; i < points.size(); ++i) {
    if (x > points[i].first) continue;
    float span = points[i].first - points[i - 1].first;
    float t = span > 0 ? (x - points[i - 1].first) / span : 1;
    return (points[i - 1].second + (points[i].second - points[i - 1].second) * t) / 100.0f;
  }
  return points.back().second / 100.0f;
}

// In the global settings (the head as in ParseGlobalSettings): the variables at the second offset,
// 13 bytes each, their names at the eighth; the curves at the ninth, one after the other: the
// variable's index, the count of points, the parameter, then per point x, y and a byte for the
// kind of line. The count of curves is the fifth of the head's counts.
bool ParseCurves(const uint8_t* data, size_t size, std::map<uint32_t, Curve>* out) {
  if (size < 0x4d || std::memcmp(data, "XGSF", 4) != 0) return false;
  Reader r(data, size, 4 + 2 + 2 + 2 + 8 + 1);
  r.U16();
  uint32_t variables = r.U16();
  r.U16(), r.U16();
  uint32_t count = r.U16();
  r.U16(), r.U16();
  uint32_t offsets[11];
  for (uint32_t& o : offsets) o = r.U32();
  if (r.bad || variables > 4096 || count > 4096) return false;
  std::vector<std::string> names;
  for (size_t p = offsets[7]; names.size() < variables && p < size;) {
    size_t len = 0;
    while (p + len < size && data[p + len]) ++len;
    names.emplace_back(reinterpret_cast<const char*>(data + p), len);
    p += len + 1;
  }
  out->clear();
  Reader c(data, size, offsets[8]);
  for (uint32_t i = 0; i < count && !c.bad; ++i) {
    uint32_t at = static_cast<uint32_t>(c.o);
    Curve curve;
    uint32_t variable = c.U16();
    int points = c.U8();
    curve.parameter = c.U16();
    if (variable < names.size()) curve.variable = names[variable];
    for (int j = 0; j < points && !c.bad; ++j) {
      uint32_t xb = c.U32(), yb = c.U32();
      c.U8();
      float x, y;
      std::memcpy(&x, &xb, 4), std::memcpy(&y, &yb, 4);
      curve.points.push_back({x, y});
    }
    if (!c.bad) (*out)[at] = curve;
  }
  return !c.bad;
}

// The variables (the head as in ParseCurves): at the second offset, 13 bytes each: a flags byte,
// then the value it starts at, its least and its most (floats); their names at the eighth.
bool ParseVariables(const uint8_t* data, size_t size, std::map<std::string, float>* out) {
  if (size < 0x4d || std::memcmp(data, "XGSF", 4) != 0) return false;
  Reader r(data, size, 4 + 2 + 2 + 2 + 8 + 1);
  r.U16();
  uint32_t count = r.U16();
  r.Skip(10);
  uint32_t offsets[11];
  for (uint32_t& o : offsets) o = r.U32();
  if (r.bad || count > 4096) return false;
  out->clear();
  size_t name = offsets[7];
  for (uint32_t i = 0; i < count && name < size; ++i) {
    Reader v(data, size, offsets[1] + 13 * static_cast<size_t>(i));
    v.U8();
    uint32_t bits = v.U32();
    if (v.bad) return false;
    float value;
    std::memcpy(&value, &bits, 4);
    size_t len = 0;
    while (name + len < size && data[name + len]) ++len;
    (*out)[std::string(reinterpret_cast<const char*>(data + name), len)] = value;
    name += len + 1;
  }
  return true;
}

}  // namespace moho::ui::sound
