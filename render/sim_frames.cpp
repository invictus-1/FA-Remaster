#include "sim_frames.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "core/vfs.h"
#include "sim/sim.h"
#include "sim/sim_view.h"

namespace render {
namespace {

constexpr size_t kBeatsKept = 6;

// A colour of the game's list: "ffRRGGBB".
bool HexColor(const std::string& s, Vec3* out) {
  if (s.size() != 8) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (!end || *end) return false;
  *out = {static_cast<float>((v >> 16) & 0xff) / 255.0f, static_cast<float>((v >> 8) & 0xff) / 255.0f, static_cast<float>(v & 0xff) / 255.0f};
  return true;
}

// The quoted strings of the table `<name> = { ... }` in a Lua file, in order.
std::vector<std::string> QuotedList(const std::string& text, const std::string& name) {
  std::vector<std::string> list;
  size_t at = 0;
  for (;;) {  // the name as a field: followed by '=' and '{'
    at = text.find(name, at);
    if (at == std::string::npos) return list;
    size_t p = at + name.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p < text.size() && text[p] == '=') {
      ++p;
      while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
      if (p < text.size() && text[p] == '{') {
        at = p + 1;
        break;
      }
    }
    at += name.size();
  }
  for (size_t p = at; p < text.size() && text[p] != '}';) {
    if (text[p] == '-' && p + 1 < text.size() && text[p + 1] == '-') {
      while (p < text.size() && text[p] != '\n') ++p;
    } else if (text[p] == '"' || text[p] == '\'') {
      const size_t e = text.find(text[p], p + 1);
      if (e == std::string::npos) break;
      list.push_back(text.substr(p + 1, e - p - 1));
      p = e + 1;
    } else {
      ++p;
    }
  }
  return list;
}

}  // namespace

struct SimFrames::Scratch {
  std::vector<moho::UnitView> views;
  std::vector<moho::UnitPose> poses;
  std::vector<moho::ProjectileView> projectiles;
  std::vector<moho::BeamView> beams;
  std::vector<moho::EffectEvent> effects;
  std::unordered_map<uint32_t, uint32_t> poseOf;  // unit id -> index in poses, this beat
  std::unordered_map<std::string, uint32_t> nameIndex;
};

SimFrames::SimFrames() : scratch_(std::make_unique<Scratch>()) { names_.push_back(""); }
SimFrames::~SimFrames() = default;

// The armies' colours: a colour a script set, else the lobby's colour number in the game's list.
void SimFrames::ReadArmies(const moho::Sim& sim, const moho::Vfs& vfs) {
  std::vector<Vec3> palette;
  if (auto text = vfs.ReadFile("/lua/gamecolors.lua")) {
    for (const std::string& s : QuotedList(*text, "ArmyColors")) {
      Vec3 c;
      if (HexColor(s, &c)) palette.push_back(c);
    }
  }
  std::vector<Vec3> colors;
  for (const moho::ArmyView& a : moho::ArmyViews(sim)) {
    Vec3 c{0.6f, 0.6f, 0.6f};
    const int index = a.armyColor > 0 ? a.armyColor : a.playerColor;
    if (a.rgb[0] >= 0) c = {static_cast<float>(a.rgb[0]) / 255.0f, static_cast<float>(a.rgb[1]) / 255.0f, static_cast<float>(a.rgb[2]) / 255.0f};
    else if (index >= 1 && index <= static_cast<int>(palette.size())) c = palette[static_cast<size_t>(index) - 1];
    else if (a.civilian) c = {0.87f, 0.72f, 0.53f};  // the list's CivilianArmyColor, "BurlyWood"
    colors.push_back(c);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  armyColors_ = std::move(colors);
}

// Copies what the renderer needs of the beat just computed.
void SimFrames::Beat(const moho::Sim& sim) {
  Scratch& e = *scratch_;
  moho::UnitViews(sim, &e.views);
  moho::UnitPoses(sim, &e.poses);
  moho::ProjectileViews(sim, &e.projectiles);
  moho::BeamViews(sim, &e.beams);
  moho::EffectEvents(sim, &e.effects);
  std::vector<FrameEffectEvent> events;
  events.reserve(e.effects.size());
  for (moho::EffectEvent& v : e.effects) {
    FrameEffectEvent f;
    f.kind = v.kind == moho::EffectEvent::Create ? FrameEffectEvent::Create
             : v.kind == moho::EffectEvent::Call ? FrameEffectEvent::Call : FrameEffectEvent::Destroy;
    f.tick = sim.tick();
    f.id = v.id;
    f.name = std::move(v.name);
    f.blueprint = std::move(v.blueprint);
    f.army = v.army;
    f.entity = v.entity;
    f.entity2 = v.entity2;
    f.bone = v.bone;
    f.bone2 = v.bone2;
    std::memcpy(f.position, v.position, sizeof f.position);
    f.numbers = std::move(v.numbers);
    f.text = std::move(v.text);
    f.text2 = std::move(v.text2);
    events.push_back(std::move(f));
  }
  e.poseOf.clear();
  for (uint32_t i = 0; i < e.poses.size(); ++i) e.poseOf.emplace(e.poses[i].id, i);
  Beat_ beat;
  beat.tick = sim.tick();
  beat.units.reserve(e.views.size());
  std::vector<std::string> fresh;
  auto nameOf = [&](const std::string& blueprint) {
    auto it = e.nameIndex.find(blueprint);
    if (it == e.nameIndex.end()) {
      it = e.nameIndex.emplace(blueprint, static_cast<uint32_t>(e.nameIndex.size() + 1)).first;
      fresh.push_back(blueprint);
    }
    return it->second;
  };
  for (const moho::UnitView& v : e.views) {
    FrameUnit u;
    u.id = v.id;
    u.blueprint = nameOf(v.blueprint);
    std::memcpy(u.position, v.position, sizeof u.position);
    std::memcpy(u.orientation, v.orientation, sizeof u.orientation);
    u.health = v.health;
    u.fractionComplete = v.fractionComplete;
    u.dead = v.dead;
    u.beingBuilt = v.beingBuilt;
    auto posed = e.poseOf.find(v.id);
    if (posed != e.poseOf.end()) {  // its bones that are away from rest
      u.firstBone = static_cast<uint32_t>(beat.bones.size());
      for (const moho::BonePose& b : e.poses[posed->second].bones) {
        if (b.bone < 0) continue;
        FrameBone f;
        f.bone = static_cast<uint32_t>(b.bone);
        std::memcpy(f.position, b.position, sizeof f.position);
        std::memcpy(f.orientation, b.orientation, sizeof f.orientation);
        beat.bones.push_back(f);
      }
      u.boneCount = static_cast<uint32_t>(beat.bones.size()) - u.firstBone;
    }
    beat.units.push_back(u);
  }
  beat.projectiles.reserve(e.projectiles.size());
  for (const moho::ProjectileView& v : e.projectiles) {
    FrameProjectile p;
    p.id = v.id;
    p.blueprint = v.blueprint.empty() ? 0 : nameOf(v.blueprint);
    p.army = v.army > 0 ? static_cast<uint32_t>(v.army - 1) : 0xff;
    std::memcpy(p.position, v.position, sizeof p.position);
    std::memcpy(p.orientation, v.orientation, sizeof p.orientation);
    std::memcpy(p.velocity, v.velocity, sizeof p.velocity);
    beat.projectiles.push_back(p);
  }
  beat.beams.reserve(e.beams.size());
  for (const moho::BeamView& v : e.beams) {
    FrameBeam b;
    b.id = v.id;
    b.army = v.army > 0 ? static_cast<uint32_t>(v.army - 1) : 0xff;
    std::memcpy(b.start, v.start, sizeof b.start);
    std::memcpy(b.end, v.end, sizeof b.end);
    beat.beams.push_back(b);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::string& n : fresh) names_.push_back(std::move(n));
  for (FrameEffectEvent& f : events) effectEvents_.push_back(std::move(f));
  beats_.push_back(std::move(beat));
  while (beats_.size() > kBeatsKept) beats_.pop_front();
}

uint32_t SimFrames::FirstTick() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return beats_.empty() ? 0 : beats_.front().tick;
}

uint32_t SimFrames::LastTick() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return beats_.empty() ? 0 : beats_.back().tick;
}

bool SimFrames::FrameAt(double tick, Frame* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->units.clear();
  out->bones.clear();
  out->projectiles.clear();
  out->beams.clear();
  out->blend = 0;
  for (size_t i = namesForFrames_.size(); i < names_.size(); ++i) namesForFrames_.push_back(names_[i]);
  out->blueprints = &namesForFrames_;
  if (beats_.empty()) return false;
  tick = std::clamp(tick, static_cast<double>(beats_.front().tick), static_cast<double>(beats_.back().tick));
  size_t i = 0;
  while (i + 1 < beats_.size() && static_cast<double>(beats_[i + 1].tick) <= tick) ++i;
  const Beat_& a = beats_[i];
  const Beat_* b = i + 1 < beats_.size() ? &beats_[i + 1] : nullptr;
  const float t = b ? static_cast<float>((tick - a.tick) / static_cast<double>(b->tick - a.tick)) : 0;
  out->tick = a.tick;
  // the bones of both beats, this beat's first: the units' spans for the next beat are moved behind them
  out->bones = a.bones;
  if (b) out->bones.insert(out->bones.end(), b->bones.begin(), b->bones.end());
  out->blend = t;
  BlendUnits(a.units.data(), a.units.size(), b ? b->units.data() : nullptr, b ? b->units.size() : 0, t, &out->units,
             static_cast<uint32_t>(a.bones.size()));
  // projectiles fly on along their velocity for the part of the beat that has passed (a beat is a tenth of a second)
  out->projectiles = a.projectiles;
  const float ahead = b ? t * 0.1f * static_cast<float>(b->tick - a.tick) : 0;
  for (FrameProjectile& p : out->projectiles)
    for (int k = 0; k < 3; ++k) p.position[k] += p.velocity[k] * ahead;
  out->beams = a.beams;
  return true;
}

void SimFrames::TakeEffectEvents(std::vector<FrameEffectEvent>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  out->swap(effectEvents_);
}

std::vector<Vec3> SimFrames::ArmyColors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return armyColors_;
}

}  // namespace render
