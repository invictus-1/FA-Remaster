#include "recording.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "linalg.h"

namespace render {
namespace {

template <class T>
T Get(const std::vector<uint8_t>& d, size_t at) {
  T v;
  std::memcpy(&v, d.data() + at, sizeof(T));
  return v;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize(size > 0 ? static_cast<size_t>(size) : 0);
  bool ok = out->empty() || fread(out->data(), 1, out->size(), f) == out->size();
  fclose(f);
  return ok;
}

}  // namespace

bool Recording::Load(const std::string& hostPath, std::string* error) {
  std::vector<uint8_t> d;
  if (!ReadFile(hostPath, &d)) {
    *error = "cannot read the file";
    return false;
  }
  if (d.size() < 16 || std::memcmp(d.data(), "LREC", 4) != 0) {
    *error = "not a recording";
    return false;
  }
  const uint32_t unitSize = Get<uint32_t>(d, 8), flags = Get<uint32_t>(d, 12);
  if (unitSize < 60) {
    *error = "unknown unit record size";
    return false;
  }
  const bool ours = (flags & 1) != 0;  // the engine stores quaternions x y z w; the original game's recorder w x y z
  ticks_.clear();
  units_.clear();
  created_.clear();
  named_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> named;  // unit id, blueprint: 'U' records since the last tick
  size_t off = 16;
  const size_t n = d.size();
  while (off + 12 <= n) {
    const uint8_t type = d[off];
    if (type == 'D') {
      off += 12;
    } else if (type == 'X') {
      off += 4 + 4 * static_cast<size_t>(d[off + 1]);
    } else if (type == 'E') {
      off += 8 + 32 * static_cast<size_t>(d[off + 1]);
    } else if (type == 'A') {
      off += 56;
    } else if (type == 'R') {
      off += 32;
    } else if (type == 'U') {  // which blueprint a unit is: before the first 'T' that lists it
      const size_t length = d[off + 1];
      if (off + 8 + length > n) break;
      const uint32_t id = Get<uint32_t>(d, off + 4);
      std::string name(reinterpret_cast<const char*>(d.data()) + off + 8, length);
      while (!name.empty() && name.back() == '\0') name.pop_back();
      named.emplace_back(id, BlueprintIndex(name));
      off += 8 + (length + 3) / 4 * 4;
    } else if (type == 'T') {
      Tick t;
      t.tick = Get<uint32_t>(d, off + 4);
      for (const auto& u : named) created_[u.first].push_back({t.tick, u.second});
      named_ += static_cast<int>(named.size());
      named.clear();
      t.count = Get<uint32_t>(d, off + 8);
      t.first = units_.size();
      off += 12;
      if (off + t.count * unitSize > n) break;  // cut short
      for (size_t i = 0; i < t.count; ++i, off += unitSize) {
        FrameUnit u;
        u.id = Get<uint32_t>(d, off);
        float q[4], p[3];
        std::memcpy(q, d.data() + off + 4, sizeof q);
        std::memcpy(p, d.data() + off + 20, sizeof p);
        if (ours) std::memcpy(u.orientation, q, sizeof q);
        else {
          u.orientation[0] = q[1];
          u.orientation[1] = q[2];
          u.orientation[2] = q[3];
          u.orientation[3] = q[0];
        }
        std::memcpy(u.position, p, sizeof p);
        u.states = Get<uint64_t>(d, off + 32);
        u.layer = Get<uint32_t>(d, off + 40);
        u.health = Get<float>(d, off + 44);
        u.fractionComplete = Get<float>(d, off + 48);
        u.dead = d[off + 56] != 0;
        u.beingBuilt = d[off + 57] != 0;
        units_.push_back(u);
      }
      // the same tick can be written again while the game is paused: keep the first
      if (ticks_.empty() || ticks_.back().tick < t.tick) ticks_.push_back(t);
      else units_.resize(t.first);
    } else {
      fprintf(stderr, "render: recording: unknown record 0x%02x at byte %zu, stopping there\n", type, off);
      break;
    }
  }
  if (ticks_.empty()) {
    *error = "the recording holds no ticks";
    return false;
  }
  return true;
}

uint32_t Recording::BlueprintIndex(const std::string& name) {
  auto it = blueprintIndex_.find(name);
  if (it != blueprintIndex_.end()) return it->second;
  blueprintIndex_.emplace(name, static_cast<uint32_t>(blueprints_.size()));
  blueprints_.push_back(name);
  return static_cast<uint32_t>(blueprints_.size() - 1);
}

int Recording::LoadCreateLog(const std::string& hostPath) {
  if (named_ > 0) return 0;  // the recording says it itself
  std::ifstream in(hostPath);
  std::string line;
  int found = 0;
  while (std::getline(in, line)) {
    size_t at = line.find("create ");
    unsigned tick = 0, id = 0;
    char bp[64];
    if (at == std::string::npos || sscanf(line.c_str() + at, "create %u unit %u bp %63s", &tick, &id, bp) != 3) continue;
    created_[id].push_back({tick, BlueprintIndex(bp)});
    ++found;
  }
  return found;
}

uint32_t Recording::BlueprintOf(uint32_t id, uint32_t tick) const {
  auto it = created_.find(id);
  if (it == created_.end()) return 0;
  uint32_t bp = 0;
  for (const Created& c : it->second) {
    if (c.tick > tick && bp) break;
    bp = c.blueprint;
  }
  return bp;
}

void Recording::FrameAt(double tick, Frame* out) const {
  out->units.clear();
  out->blueprints = &blueprints_;
  if (ticks_.empty()) return;
  tick = std::clamp(tick, static_cast<double>(ticks_.front().tick), static_cast<double>(ticks_.back().tick));
  // the last recorded tick at or before the moment, and the next one
  auto next = std::upper_bound(ticks_.begin(), ticks_.end(), tick, [](double t, const Tick& k) { return t < static_cast<double>(k.tick); });
  const Tick& a = *(next - 1);
  const Tick* b = next != ticks_.end() ? &*next : nullptr;
  const float t = b ? static_cast<float>((tick - a.tick) / static_cast<double>(b->tick - a.tick)) : 0;
  out->tick = a.tick;
  out->units.reserve(a.count);
  size_t j = 0;
  for (size_t i = 0; i < a.count; ++i) {
    FrameUnit u = units_[a.first + i];
    u.blueprint = BlueprintOf(u.id, a.tick);
    if (b && t > 0) {
      while (j < b->count && units_[b->first + j].id < u.id) ++j;
      if (j < b->count && units_[b->first + j].id == u.id && BlueprintOf(u.id, b->tick) == u.blueprint) {
        const FrameUnit& v = units_[b->first + j];
        for (int k = 0; k < 3; ++k) u.position[k] += (v.position[k] - u.position[k]) * t;
        Quat q = Nlerp({u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]},
                       {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]}, t);
        u.orientation[0] = q.x;
        u.orientation[1] = q.y;
        u.orientation[2] = q.z;
        u.orientation[3] = q.w;
        u.fractionComplete += (v.fractionComplete - u.fractionComplete) * t;
        u.health += (v.health - u.health) * t;
      }
    }
    out->units.push_back(u);
  }
}

}  // namespace render
