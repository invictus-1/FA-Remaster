#include "sim_frames.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <iterator>  // [session]
#include <chrono>  // [spikes]

#include "hitch_log.h"  // [spikes]

#include "core/vfs.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/terrain.h"

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
  std::vector<moho::UnitHiddenBones> hidden;
  std::unordered_map<uint32_t, uint32_t> hiddenOf;  // unit id -> index in hidden, this beat
  std::vector<moho::ProjectileView> projectiles;
  std::vector<moho::BeamView> beams;
  std::vector<moho::EffectEvent> effects;
  std::vector<moho::EntityView> entities;
  std::vector<moho::PropEvent> props;
  std::vector<moho::PropEvent> heldProps;  // [session] (HoldPropEvents: the next beat's, ahead of its own)
  std::unordered_map<uint32_t, uint32_t> poseOf;  // unit id -> index in poses, this beat
  std::unordered_map<std::string, uint32_t> nameIndex;
  // [spikes] (AskBuildable, the simulation's thread only) every unit blueprint of the game; the kinds asked by a unit of
  // them, and by a finished one; the kinds named so far; the kinds that build something, in the order found, and the next
  // of them to ask again
  std::vector<std::string> unitIds;
  std::unordered_set<std::string> asked, askedFinished, named;
  std::vector<std::string> builders;
  size_t nextBuilder = 0;
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
  // how the blueprints look (src/sim/sim_view.h, BlueprintDisplays); [spikes] the unit blueprints AskBuildable asks about
  std::vector<BlueprintLook> looks = LooksOf(sim, &scratch_->unitIds);
  std::lock_guard<std::mutex> lock(mutex_);
  armyColors_ = std::move(colors);
  looks_ = std::move(looks);
  seed_ = moho::SessionSeed(sim);
}

// [session] (sim_frames.h) ReadArmies' looks, as a function of their own
std::vector<BlueprintLook> SimFrames::LooksOf(const moho::Sim& sim, std::vector<std::string>* unitIds) {
  std::vector<moho::BlueprintDisplay> displays;
  moho::BlueprintDisplays(sim, &displays);
  unitIds->clear();
  for (const moho::BlueprintDisplay& d : displays)
    if (d.kind == 0 && !d.id.empty()) unitIds->push_back(d.id);
  std::vector<BlueprintLook> looks;
  looks.reserve(displays.size());
  for (moho::BlueprintDisplay& d : displays) {
    BlueprintLook look;
    look.id = std::move(d.id);
    look.kind = d.kind;
    look.scale = d.uniformScale;
    for (moho::DisplayLod& l : d.lods) {
      BlueprintLook::Level level;
      level.mesh = std::move(l.mesh);
      level.albedo = std::move(l.albedo);
      level.normals = std::move(l.normals);
      level.specular = std::move(l.specular);
      level.secondary = std::move(l.secondary);
      level.shader = std::move(l.shader);
      level.cutoff = l.cutoff;
      level.scrolling = l.scrolling;
      look.levels.push_back(std::move(level));
    }
    look.icon = std::move(d.icon);
    look.iconPriority = d.iconSortPriority;
    look.iconFadeIn = d.iconFadeInZoom;
    look.selectionX = d.selectionSizeX;
    look.selectionZ = d.selectionSizeZ;
    look.selectionThickness = d.selectionThickness;
    for (int i = 0; i < 3; ++i) look.selectionOffset[i] = d.selectionCenterOffset[i];
    look.lifeBarSize = d.lifeBarSize;
    look.lifeBarHeight = d.lifeBarHeight;
    look.lifeBarOffset = d.lifeBarOffset;
    looks.push_back(std::move(look));
  }
  return looks;
}

void SimFrames::AdoptArmies(SimFrames& from) {  // [session]
  std::vector<Vec3> colors;
  uint32_t seed = 0;
  {
    std::lock_guard<std::mutex> lock(from.mutex_);
    colors = std::move(from.armyColors_);
    seed = from.seed_;
  }
  scratch_->unitIds = std::move(from.scratch_->unitIds);
  std::lock_guard<std::mutex> lock(mutex_);
  armyColors_ = std::move(colors);
  looks_.clear();
  seed_ = seed;
}

void SimFrames::HoldPropEvents(std::vector<moho::PropEvent>* events) {
  std::vector<moho::PropEvent>& held = scratch_->heldProps;
  if (held.empty()) held.swap(*events);  // ([session] the map's props, tens of thousands: taken whole)
  else held.insert(held.end(), std::make_move_iterator(events->begin()), std::make_move_iterator(events->end()));
  events->clear();
}

void SimFrames::HoldEffectEvents(std::vector<FrameEffectEvent>* events) {  // [deposits]
  std::lock_guard<std::mutex> lock(mutex_);
  effectEvents_.insert(effectEvents_.begin(), std::make_move_iterator(events->begin()), std::make_move_iterator(events->end()));
  events->clear();
}

void SimFrames::SetSightGrid(float mapWidth, float mapHeight, int cells) {
  std::lock_guard<std::mutex> lock(mutex_);
  sightWidth_ = mapWidth;
  sightHeight_ = mapHeight;
  sightCells_ = cells;
}

bool SimFrames::TakeSight(std::vector<uint8_t>* grid, int* cells) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!sightNew_) return false;
  sightNew_ = false;
  *grid = sight_;
  *cells = sight_.empty() ? 0 : sightCells_;
  return true;
}

uint32_t SimFrames::Seed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seed_;
}

// [spikes] (WatchBuildable) After a beat: the kinds there for the first time asked what they may build, then a finished one
// of each, and one that builds asked again every 10th beat.
void SimFrames::AskBuildable(const moho::Sim& sim) {
  Scratch& e = *scratch_;
  const auto start = std::chrono::steady_clock::now();
  int asked = 0;
  std::vector<std::string> found;
  auto ask = [&](const moho::UnitView& u) {
    ++asked;
    if (e.named.insert(u.blueprint).second) found.push_back(u.blueprint);
    bool builds = false;
    for (const std::string& id : e.unitIds) {
      if (!moho::UnitMayBuild(sim, u.id, id)) continue;
      builds = true;
      if (e.named.insert(id).second) found.push_back(id);
    }
    if (builds && std::find(e.builders.begin(), e.builders.end(), u.blueprint) == e.builders.end()) e.builders.push_back(u.blueprint);
  };
  for (const moho::UnitView& u : e.views) {
    if (u.blueprint.empty() || u.dead) continue;
    if (e.asked.insert(u.blueprint).second) ask(u);
    else if (!u.beingBuilt && e.askedFinished.insert(u.blueprint).second) ask(u);
  }
  if (!e.builders.empty() && sim.tick() % 10 == 0) {  // (one that builds, again, by a unit of it there now)
    const std::string kind = e.builders[e.nextBuilder++ % e.builders.size()];
    for (const moho::UnitView& u : e.views)
      if (u.blueprint == kind && !u.dead && !u.beingBuilt) {
        ask(u);
        break;
      }
  }
  if (HitchLog::on && asked) {  // (its cost on the simulation's thread, in the hitch log: elsewhere)
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 1) HitchLog::Add("what the armies may build asked: " + std::to_string(asked) + " kinds, " + std::to_string(found.size()) + " new", ms);
  }
  if (found.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  buildable_.insert(buildable_.end(), found.begin(), found.end());
}

void SimFrames::TakeBuildable(std::vector<std::string>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  out->swap(buildable_);
}

void SimFrames::TakeLooks(std::vector<BlueprintLook>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  out->swap(looks_);
}

// Copies what the renderer needs of the beat just computed.
void SimFrames::Beat(const moho::Sim& sim) {
  Scratch& e = *scratch_;
  moho::UnitViews(sim, &e.views);
  if (watchBuildable_.load()) AskBuildable(sim);  // [spikes]
  moho::UnitPoses(sim, &e.poses);
  moho::HiddenBones(sim, &e.hidden);
  moho::ProjectileViews(sim, &e.projectiles);
  moho::BeamViews(sim, &e.beams);
  moho::EffectEvents(sim, &e.effects);
  std::vector<FrameEffectEvent> events;
  events.reserve(e.effects.size());
  const int focusNow = focus_.load();
  for (moho::EffectEvent& v : e.effects) {
    FrameEffectEvent f;
    if (focusNow > 0 && v.kind == moho::EffectEvent::Create) f.seen = (moho::ReconAt(sim, focusNow, v.position) & moho::kReconSeenNow) != 0;
    // (a mark on the ground laid for the army the picture is for is its own: shown where it does not see,
    // as the campaign's objective areas are, made for army 1 anywhere on the map)
    if (focusNow > 0 && v.army == focusNow && (v.name == "CreateDecal" || v.name == "CreateSplat")) f.seen = true;
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
    f.hasPosition2 = v.hasPosition2;
    std::memcpy(f.position2, v.position2, sizeof f.position2);
    f.numbers = std::move(v.numbers);
    f.text = std::move(v.text);
    f.text2 = std::move(v.text2);
    events.push_back(std::move(f));
  }
  // hits, impacts and health changes (render REQUESTS 36), when the simulation keeps them
  FrameCombat combat;
  auto seenAt = [&](const float position[3]) {
    return focusNow <= 0 || (moho::ReconAt(sim, focusNow, position) & moho::kReconSeenNow) != 0;
  };
  if (sim.collectHits)
    for (moho::HitEvent& v : sim.TakeHits()) {
      FrameHit h;
      h.tick = sim.tick();
      h.target = v.target;
      h.targetKind = v.targetKind;
      h.shield = v.shield;
      h.shieldId = v.shieldId;
      h.instigator = v.instigator;
      h.army = v.army;
      h.weapon = std::move(v.weapon);
      h.projectile = std::move(v.projectile);
      h.beam = v.beam;
      h.damageType = std::move(v.damageType);
      h.amount = v.amount;
      std::memcpy(h.position, v.position, sizeof h.position);
      h.radius = v.radius;
      std::memcpy(h.direction, v.direction, sizeof h.direction);
      h.seen = seenAt(v.position);
      combat.hits.push_back(std::move(h));
    }
  if (sim.collectImpacts)
    for (moho::ImpactEvent& v : sim.TakeImpacts()) {
      FrameImpact m;
      m.kind = v.kind == moho::ImpactEvent::Beam ? FrameImpact::Beam : v.kind == moho::ImpactEvent::Crash ? FrameImpact::Crash : FrameImpact::Projectile;
      m.tick = sim.tick();
      m.type = std::move(v.type);
      m.source = v.source;
      m.army = v.army;
      m.projectile = std::move(v.projectile);
      m.weapon = std::move(v.weapon);
      m.entity = v.entity;
      std::memcpy(m.position, v.position, sizeof m.position);
      std::memcpy(m.direction, v.direction, sizeof m.direction);
      m.speed = v.speed;
      m.seen = seenAt(v.position);
      combat.impacts.push_back(std::move(m));
    }
  if (sim.collectHealth)
    for (const moho::HealthEvent& v : sim.TakeHealth()) {
      FrameHealth h;
      h.tick = sim.tick();
      h.entity = v.entity;
      h.kind = v.kind;
      h.fraction = v.fraction;
      h.before = v.before;
      combat.health.push_back(h);
    }
  e.poseOf.clear();
  for (uint32_t i = 0; i < e.poses.size(); ++i) e.poseOf.emplace(e.poses[i].id, i);
  e.hiddenOf.clear();
  for (uint32_t i = 0; i < e.hidden.size(); ++i) e.hiddenOf.emplace(e.hidden[i].id, i);
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
  const int focus = focus_.load();
  // what the focus army sees over the map, every fifth beat (and once more, empty, when there is no such army any more)
  {
    int cells;
    bool wasOn;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cells = sightCells_;
      wasOn = sightWasOn_;
    }
    if (focus > 0 && cells > 0 && sightBeat_++ % 5 == 0) {
      const size_t count = static_cast<size_t>(cells) * static_cast<size_t>(cells);
      sightWork_.resize(count);
      // (what it has seen at some time is kept here: the engine does not keep it of places. So it
      // begins when the army becomes the focus army, and a place crossed between two of these
      // looks, half a second apart, can be missed)
      if (sightEver_.size() != count || sightEverOf_ != focus) {
        sightEver_.assign(count, 0);
        sightEverOf_ = focus;
      }
      size_t seenNow = 0;
      // (every cell's ReconAt at its centre at once, the engine's ReconGrid: the same bits, about a tenth of the time;
      // asked as of something on the water's surface: sight is the same as on land there, and sonar is told too)
      moho::ReconGrid(sim, focus, cells, cells, &sightRecon_, 1);
      if (sightRecon_.size() != count) sightRecon_.assign(count, 0);  // (no map: nothing seen)
      for (size_t i = 0; i < count; ++i) {
        const uint32_t recon = sightRecon_[i];
        const bool now = (recon & moho::kReconSeenNow) != 0;
        seenNow += now;
        sightWork_[i] = static_cast<uint8_t>((now ? kSightNow : 0) | ((recon & (moho::kReconRadar | moho::kReconOmni)) ? kSightRadar : 0) |
                                             ((recon & moho::kReconSonar) ? kSightSonar : 0));
      }
      // A look in which EVERY place is seen does not count as having seen the map: that is a
      // script's doing, not exploring (the M28 AI gives its commanders sight of the whole map for a
      // few beats at the start, and an ally's sight is the army's own: docs/engine-answers.md,
      // render 13). It is shown as seen while it lasts.
      const bool counts = seenNow < count;
      for (size_t i = 0; i < count; ++i) {
        if (counts && (sightWork_[i] & kSightNow)) sightEver_[i] = 1;
        if (sightEver_[i]) sightWork_[i] |= kSightEver;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      sight_ = sightWork_;
      sightNew_ = sightWasOn_ = true;
    } else if (focus <= 0 && wasOn) {
      std::lock_guard<std::mutex> lock(mutex_);
      sight_.clear();
      sightNew_ = true;
      sightWasOn_ = false;
      sightBeat_ = 0;
    }
  }
  for (const moho::UnitView& v : e.views) {
    FrameUnit u;
    u.id = v.id;
    if (focus > 0) u.recon = static_cast<uint8_t>(moho::ReconOf(sim, focus, v.id) & 0x7f);
    u.blueprint = nameOf(v.blueprint);
    if (!v.mesh.empty()) u.mesh = nameOf(v.mesh);
    std::memcpy(u.position, v.position, sizeof u.position);
    std::memcpy(u.orientation, v.orientation, sizeof u.orientation);
    u.health = v.health;
    u.maxHealth = v.maxHealth;
    u.fractionComplete = v.fractionComplete;
    u.dead = v.dead;
    u.beingBuilt = v.beingBuilt;
    u.workProgress = v.workProgress;
    u.paused = v.paused;
    u.hasShield = v.hasShield;
    u.shieldRatio = v.shieldRatio;
    std::memcpy(u.scrollPrev, v.scrollPrev, sizeof u.scrollPrev);
    std::memcpy(u.scroll, v.scroll, sizeof u.scroll);
    auto posed = e.poseOf.find(v.id);
    auto hides = e.hiddenOf.find(v.id);
    if (posed != e.poseOf.end() || hides != e.hiddenOf.end()) {
      // its bones that are away from rest, and those its script has hidden (a commander's
      // upgrades not bought yet, a factory's folded arms): one list in rising bone order, a
      // hidden bone said once, as hidden
      u.firstBone = static_cast<uint32_t>(beat.bones.size());
      static const std::vector<moho::BonePose> noneMoved;
      static const std::vector<int> noneHidden;
      const std::vector<moho::BonePose>& moved = posed != e.poseOf.end() ? e.poses[posed->second].bones : noneMoved;
      const std::vector<int>& hidden = hides != e.hiddenOf.end() ? e.hidden[hides->second].bones : noneHidden;
      size_t im = 0, ih = 0;
      for (;;) {
        while (im < moved.size() && moved[im].bone < 0) ++im;
        while (ih < hidden.size() && hidden[ih] < 0) ++ih;
        const bool haveMoved = im < moved.size(), haveHidden = ih < hidden.size();
        if (!haveMoved && !haveHidden) break;
        FrameBone f;
        if (haveHidden && (!haveMoved || hidden[ih] <= moved[im].bone)) {
          f.bone = static_cast<uint32_t>(hidden[ih]);
          f.hidden = true;
          if (haveMoved && moved[im].bone == hidden[ih]) ++im;
          ++ih;
        } else {
          const moho::BonePose& b = moved[im++];
          f.bone = static_cast<uint32_t>(b.bone);
          std::memcpy(f.position, b.position, sizeof f.position);
          std::memcpy(f.orientation, b.orientation, sizeof f.orientation);
        }
        beat.bones.push_back(f);
      }
      u.boneCount = static_cast<uint32_t>(beat.bones.size()) - u.firstBone;
    }
    beat.units.push_back(u);
  }
  // Whom a script has hidden a thing from (SetVizToFocusPlayer / Allies / Enemies / Neutrals, 2 = 'Never'):
  // is a thing of `army` (from 1; 0: none) hidden from the army the picture is for? With no such army
  // it is as its own army sees it.
  std::vector<int> towards;  // how the picture's army stands to each army: 0 enemy, 1 neutral, 2 ally
  if (focus > 0) {
    const std::vector<moho::ArmyView> armies = moho::ArmyViews(sim);
    if (static_cast<size_t>(focus) <= armies.size()) towards = armies[static_cast<size_t>(focus - 1)].alliance;
  }
  auto never = [&](int army, uint8_t own, uint8_t allies, uint8_t enemies, uint8_t neutrals) {
    if (focus <= 0 || army <= 0 || army == focus) return own == 2;
    const int stance = static_cast<size_t>(army - 1) < towards.size() ? towards[static_cast<size_t>(army - 1)] : 0;
    return (stance == 2 ? allies : stance == 1 ? neutrals : enemies) == 2;
  };
  // (and 'Always': seen whatever the army knows, as an objective's arrow over a unit in the fog of war)
  auto always = [&](int army, uint8_t own, uint8_t allies, uint8_t enemies, uint8_t neutrals) {
    if (focus <= 0 || army <= 0 || army == focus) return own == 1;
    const int stance = static_cast<size_t>(army - 1) < towards.size() ? towards[static_cast<size_t>(army - 1)] : 0;
    return (stance == 2 ? allies : stance == 1 ? neutrals : enemies) == 1;
  };
  beat.projectiles.reserve(e.projectiles.size());
  for (const moho::ProjectileView& v : e.projectiles) {
    FrameProjectile p;
    p.id = v.id;
    p.blueprint = v.blueprint.empty() ? 0 : nameOf(v.blueprint);
    p.army = v.army > 0 ? static_cast<uint32_t>(v.army - 1) : 0xff;
    if (focus > 0) p.seen = (moho::ReconOf(sim, focus, v.id) & moho::kReconSeenNow) != 0;  // (a projectile: its place's answer)
    // (a shell its script hides: FAF's ShieldCollider, a box ten across hung on a crashing plane so that
    // the plane can hit a shield, hidden from everybody; drawn, it was a black square at shields under fire)
    if (never(v.army, v.vizFocus, v.vizAllies, v.vizEnemies, v.vizNeutrals)) p.seen = false;
    std::memcpy(p.position, v.position, sizeof p.position);
    std::memcpy(p.orientation, v.orientation, sizeof p.orientation);
    std::memcpy(p.velocity, v.velocity, sizeof p.velocity);
    // its age: since it was first seen here
    auto since = shellSince_.find(v.id);
    const uint32_t first = since != shellSince_.end() ? since->second : sim.tick();
    shellSinceNext_[v.id] = first;
    p.age = static_cast<float>(sim.tick() - first) * 0.1f;
    // its size: the simulation's own account of it
    p.sized = true;
    p.stopped = v.stopped;
    for (int k = 0; k < 3; ++k) {
      p.size[k] = v.size[k];
      p.growth[k] = v.scaleVelocity[k];
    }
    beat.projectiles.push_back(p);
  }
  shellSince_.swap(shellSinceNext_);  // (only those there this beat are kept)
  shellSinceNext_.clear();
  // what scripts made that has a mesh: shields' bubbles and the like
  moho::EntityViews(sim, &e.entities);
  beat.entities.reserve(e.entities.size());
  for (const moho::EntityView& v : e.entities) {
    if (v.mesh.empty()) continue;
    FrameEntity f;
    f.id = v.id;
    f.mesh = nameOf(v.mesh);
    f.army = v.army > 0 ? static_cast<uint32_t>(v.army - 1) : 0xff;
    std::memcpy(f.position, v.position, sizeof f.position);
    std::memcpy(f.orientation, v.orientation, sizeof f.orientation);
    for (int k = 0; k < 3; ++k) f.scale[k] = v.scale[k] * v.drawScale;
    f.shield = v.shield;
    // (for the army the picture is for: where it sees. The script's own say on who sees it,
    // SetVizTo..., is 'Intel' for whoever is not its owner's side, which comes to the same; 'Never' is kept)
    if (focus > 0)
      f.seen = always(v.army, v.vizFocus, v.vizAllies, v.vizEnemies, v.vizNeutrals) ||
               ((moho::ReconOf(sim, focus, v.id) & moho::kReconSeenNow) != 0 && !never(v.army, v.vizFocus, v.vizAllies, v.vizEnemies, v.vizNeutrals));
    beat.entities.push_back(f);
  }
  // how the scenery changed (the engine's list is emptied by taking it, which is why the simulation is not const here)
  moho::PropEvents(const_cast<moho::Sim&>(sim), &e.props);
  if (!e.heldProps.empty()) {  // [session] (taken before the first beat: ahead of this beat's own)
    e.props.insert(e.props.begin(), std::make_move_iterator(e.heldProps.begin()), std::make_move_iterator(e.heldProps.end()));
    e.heldProps.clear();
  }
  std::vector<FramePropEvent> propEvents;
  propEvents.reserve(e.props.size());
  for (moho::PropEvent& v : e.props) {
    FramePropEvent f;
    f.kind = v.kind == moho::PropEvent::Create ? FramePropEvent::Create : v.kind == moho::PropEvent::Change ? FramePropEvent::Change : FramePropEvent::Destroy;
    f.tick = sim.tick();
    f.id = v.id;
    f.mapIndex = v.mapIndex;
    f.blueprint = std::move(v.blueprint);
    f.mesh = std::move(v.mesh);
    std::memcpy(f.position, v.position, sizeof f.position);
    std::memcpy(f.orientation, v.orientation, sizeof f.orientation);
    for (int k = 0; k < 3; ++k) f.scale[k] = v.scale[k] * v.drawScale;
    propEvents.push_back(std::move(f));
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
  // how the simulation changed the ground since the last beat (FlattenMapRect under a structure):
  // each rectangle's heights as they are now
  std::vector<FrameGroundChange> ground;
  if (const moho::TerrainMap* map = sim.map()) {
    const std::vector<moho::TerrainMap::HeightChange>& changes = map->heightChanges();
    if (groundSeen_ > changes.size()) groundSeen_ = 0;  // (another game's map)
    for (; groundSeen_ < changes.size(); ++groundSeen_) {
      const moho::TerrainMap::HeightChange& c = changes[groundSeen_];
      if (c.x0 > c.x1 || c.z0 > c.z1) continue;
      FrameGroundChange g;
      g.x0 = c.x0, g.z0 = c.z0, g.x1 = c.x1, g.z1 = c.z1;
      g.heights.reserve(static_cast<size_t>(c.x1 - c.x0 + 1) * static_cast<size_t>(c.z1 - c.z0 + 1));
      for (int z = c.z0; z <= c.z1; ++z)
        for (int x = c.x0; x <= c.x1; ++x) g.heights.push_back(map->HeightAt(x, z));
      ground.push_back(std::move(g));
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::string& n : fresh) names_.push_back(std::move(n));
  for (FrameEffectEvent& f : events) effectEvents_.push_back(std::move(f));
  for (FrameGroundChange& g : ground) groundChanges_.push_back(std::move(g));
  combatKept_[0] = sim.collectHits;
  combatKept_[1] = sim.collectImpacts;
  combatKept_[2] = sim.collectHealth;
  {  // (a taker far behind loses the oldest hits and impacts; an entity's health changes not taken yet are one)
    auto keep = [](auto& list, auto& more) {
      for (auto& x : more) list.push_back(std::move(x));
      if (list.size() > kMostCombat) list.erase(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(list.size() - kMostCombat));
    };
    keep(combat_.hits, combat.hits);
    keep(combat_.impacts, combat.impacts);
    for (const FrameHealth& h : combat.health) {
      auto waiting = healthWaiting_.find(h.entity);
      if (waiting != healthWaiting_.end()) {
        FrameHealth& was = combat_.health[waiting->second];
        was.tick = h.tick;
        was.kind = h.kind;
        was.fraction = h.fraction;
        continue;
      }
      healthWaiting_.emplace(h.entity, combat_.health.size());
      combat_.health.push_back(h);
    }
  }
  // (a change of a prop that has one waiting, not yet taken, takes its place: a fallen tree sinks a
  // little every beat, and whoever draws only now and then needs where it is, not where it was)
  for (FramePropEvent& f : propEvents) {
    if (f.kind == FramePropEvent::Change) {
      auto waiting = propWaiting_.find(f.id);
      if (waiting != propWaiting_.end() && waiting->second < propEvents_.size() && propEvents_[waiting->second].id == f.id &&
          propEvents_[waiting->second].kind == FramePropEvent::Change) {
        propEvents_[waiting->second] = std::move(f);
        continue;
      }
      propWaiting_[f.id] = propEvents_.size();
    } else {
      propWaiting_.erase(f.id);
    }
    propEvents_.push_back(std::move(f));
  }
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

bool SimFrames::UnitAt(double tick, uint32_t id, float position[3], float orientation[4]) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (beats_.empty()) return false;
  tick = std::clamp(tick, static_cast<double>(beats_.front().tick), static_cast<double>(beats_.back().tick));
  size_t i = 0;
  while (i + 1 < beats_.size() && static_cast<double>(beats_[i + 1].tick) <= tick) ++i;
  const Beat_& a = beats_[i];
  const Beat_* b = i + 1 < beats_.size() ? &beats_[i + 1] : nullptr;
  const float t = b ? static_cast<float>((tick - a.tick) / static_cast<double>(b->tick - a.tick)) : 0;
  // (a beat's units are in the order of their ids, as BlendUnits walks them)
  auto find = [id](const std::vector<FrameUnit>& units) -> const FrameUnit* {
    auto it = std::lower_bound(units.begin(), units.end(), id, [](const FrameUnit& u, uint32_t want) { return u.id < want; });
    return it != units.end() && it->id == id ? &*it : nullptr;
  };
  const FrameUnit* u = find(a.units);
  if (!u) return false;
  std::vector<FrameUnit> blended;
  const FrameUnit* v = b && t > 0 ? find(b->units) : nullptr;
  BlendUnits(u, 1, v, v ? 1 : 0, t, &blended, 0);  // (the same blend as the frame's)
  std::memcpy(position, blended[0].position, sizeof blended[0].position);
  std::memcpy(orientation, blended[0].orientation, sizeof blended[0].orientation);
  return true;
}

bool SimFrames::FrameAt(double tick, Frame* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->units.clear();
  out->bones.clear();
  out->projectiles.clear();
  out->beams.clear();
  out->entities.clear();
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
  for (FrameProjectile& p : out->projectiles) {
    for (int k = 0; k < 3; ++k) {
      // (not one that has hit: it stands where it hit until its script ends it next beat; moved on
      // with the speed it came with, it was drawn through what it hit, a tank's shell up to two and
      // a half units: Chris's "some go through the acu")
      if (!p.stopped) p.position[k] += p.velocity[k] * ahead;
      p.size[k] += p.growth[k] * ahead;
    }
    p.age += ahead;
  }
  out->beams = a.beams;
  // the scripts' entities: each moved toward where it is in the next beat (there are few)
  out->entities = a.entities;
  if (b)
    for (FrameEntity& f : out->entities)
      for (const FrameEntity& next : b->entities) {
        if (next.id != f.id) continue;
        for (int k = 0; k < 3; ++k) f.position[k] += (next.position[k] - f.position[k]) * t;
        break;
      }
  return true;
}

void SimFrames::TakeGroundChanges(std::vector<FrameGroundChange>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  out->swap(groundChanges_);
}

void SimFrames::TakePropEvents(std::vector<FramePropEvent>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  out->swap(propEvents_);
  propWaiting_.clear();
}

void SimFrames::TakeCombat(FrameCombat* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  std::swap(*out, combat_);
  healthWaiting_.clear();
  out->hitsKept = combatKept_[0];
  out->impactsKept = combatKept_[1];
  out->healthKept = combatKept_[2];
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
