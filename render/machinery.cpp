#include "machinery.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "build_effects.h"
#include "json.h"
#include "particles.h"
#include "ribbons.h"

namespace render {

namespace {

constexpr float kPi = 3.14159265f;

Quat Turned(Vec3 axis, float degrees) {
  const float a = degrees * kPi / 360;  // (half the angle)
  return {axis.x * std::sin(a), axis.y * std::sin(a), axis.z * std::sin(a), std::cos(a)};
}

// [rx, ry, rz] degrees turned X, then Y, then Z
Quat Euler(const JsonValue& v) {
  const float x = static_cast<float>(v.items.size() > 0 ? v.items[0].NumberOr(0) : 0);
  const float y = static_cast<float>(v.items.size() > 1 ? v.items[1].NumberOr(0) : 0);
  const float z = static_cast<float>(v.items.size() > 2 ? v.items[2].NumberOr(0) : 0);
  return Mul(Turned({0, 0, 1}, z), Mul(Turned({0, 1, 0}, y), Turned({1, 0, 0}, x)));
}

Vec3 Vector(const JsonValue& v, Vec3 fallback = {}) {
  if (v.kind != JsonValue::Array || v.items.size() < 3) return fallback;
  return {static_cast<float>(v.items[0].NumberOr(0)), static_cast<float>(v.items[1].NumberOr(0)),
          static_cast<float>(v.items[2].NumberOr(0))};
}

int BoneNamed(const std::vector<ModelBone>& bones, const std::string& name) {
  for (size_t i = 0; i < bones.size(); ++i)
    if (bones[i].name.size() == name.size() &&
        std::equal(name.begin(), name.end(), bones[i].name.begin(), [](char a, char b) { return (a | 0x20) == (b | 0x20); }))
      return static_cast<int>(i);
  return -1;
}

float Fraction(double x) { return static_cast<float>(x - std::floor(x)); }

// How strong a pulse is (1: full) at the unit's clock, in its state by how far the unit is at work (`w`).
float Pulse(const MachinerySpec::Lamp& l, double clock, float offset, float w) {
  const float in = l.state == MachinerySpec::State::Always ? 1.0f : l.state == MachinerySpec::State::Idle ? 1 - w : w;
  const float u = Fraction(clock / l.period + l.phase + offset);
  const float wave = l.beat ? (u < 0.15f ? u / 0.15f : 1 - (u - 0.15f) / 0.85f) : 0.5f - 0.5f * std::cos(u * 2 * kPi);
  return 1 - in * (1 - (l.low + (1 - l.low) * wave));
}

MachinerySpec::State StateNamed(const std::string& s, MachinerySpec::State fallback) {
  if (s == "idle") return MachinerySpec::State::Idle;
  if (s == "always") return MachinerySpec::State::Always;
  if (s == "building") return MachinerySpec::State::Building;
  return fallback;
}

// One of a part's motions from the file ("building" or "idle"; none there: Still).
void ReadMotion(const JsonValue& w, MachinerySpec::Motion* m) {
  const std::string mode = w["mode"].StringOr("");
  m->period = std::max(0.05f, static_cast<float>(w["period"].NumberOr(1)));
  m->phase = static_cast<float>(w["phase"].NumberOr(0));
  const bool sine = w["ease"].StringOr("sine") != "linear";
  if (mode == "keys") {
    m->mode = MachinerySpec::Motion::Keys;
    for (const JsonValue& k : w["keys"].items) {
      if (k.items.size() < 3) continue;
      MachinerySpec::Key key;
      key.t = Fraction(k.items[0].NumberOr(0));
      key.turn = Euler(k.items[1]);
      key.move = Vector(k.items[2]);
      // (a 4th: its scale, a number or [sx, sy, sz], or the ease; a 5th: the ease after a scale)
      size_t ease = 3;
      if (k.items.size() > 3 && k.items[3].kind == JsonValue::Number) {
        const float v = static_cast<float>(k.items[3].number);
        key.scale = {v, v, v};
        ease = 4;
      } else if (k.items.size() > 3 && k.items[3].kind == JsonValue::Array) {
        key.scale = Vector(k.items[3], {1, 1, 1});
        ease = 4;
      }
      key.sine = k.items.size() > ease ? k.items[ease].StringOr(sine ? "sine" : "linear") != "linear" : sine;
      m->keys.push_back(key);
    }
    std::sort(m->keys.begin(), m->keys.end(), [](const MachinerySpec::Key& a, const MachinerySpec::Key& b) { return a.t < b.t; });
    if (m->keys.empty()) m->mode = MachinerySpec::Motion::Still;
  } else if (mode == "loop") {
    m->mode = MachinerySpec::Motion::Loop;
    m->axis = Vector(w["axis"], {0, 0, 1});
    m->from = static_cast<float>(w["range"].items.size() > 1 ? w["range"].items[0].NumberOr(0) : 0);
    m->to = static_cast<float>(w["range"].items.size() > 1 ? w["range"].items[1].NumberOr(0) : 0);
    m->gap = std::clamp(static_cast<float>(w["gap"].NumberOr(0)), 0.0f, 0.9f);
  } else if (mode == "scroll") {
    m->mode = MachinerySpec::Motion::Scroll;
  }
}

// A motion at the point `u` (0..1) of its cycle: its turn and move, and whether it hides (a loop's jump back: then it
// waits where it comes back, so that the frame it shows again it does not streak there).
void Evaluate(const MachinerySpec::Motion& m, float u, Quat* r, Vec3* move, bool* hidden, Vec3* scale) {
  *r = Quat{};
  *move = Vec3{};
  *hidden = false;
  *scale = {1, 1, 1};
  if (m.mode == MachinerySpec::Motion::Keys) {
    const std::vector<MachinerySpec::Key>& k = m.keys;
    size_t j = k.size() - 1;
    for (size_t n = 0; n < k.size(); ++n)
      if (k[n].t <= u) j = n;
    const MachinerySpec::Key& a = k[j];
    const MachinerySpec::Key& b = k[(j + 1) % k.size()];
    float along = 0;
    if (k.size() > 1) {
      const float from = a.t > u ? a.t - 1 : a.t;  // (before the first key: from the last, a cycle back)
      float to = b.t;
      if (to <= from) to += 1;
      along = std::clamp((u - from) / std::max(to - from, 1e-4f), 0.0f, 1.0f);
      if (a.sine) along = 0.5f - 0.5f * std::cos(along * kPi);
    }
    *r = Nlerp(a.turn, b.turn, along);
    *move = Lerp(a.move, b.move, along);
    *scale = Lerp(a.scale, b.scale, along);
  } else if (m.mode == MachinerySpec::Motion::Loop) {
    const float run = 1 - m.gap;
    *hidden = u >= run;
    const float s = *hidden ? 0.0f : std::min(u / run, 1.0f);
    *move = m.axis * (m.from + (m.to - m.from) * s);
  }
}

// A unit's own offset into the cycles (factories side by side do not work in step), 0..1.
float UnitOffset(uint32_t id) { return Fraction(id * 0.6180339887); }

}  // namespace

std::shared_ptr<const MachinerySpec> ReadMachinery(const std::string& text, const std::vector<ModelBone>& bones, std::string* why) {
  JsonValue root;
  if (!ReadJson(text, &root, why)) return nullptr;
  auto spec = std::make_shared<MachinerySpec>();
  spec->blend = std::max(0.05f, static_cast<float>(root["states"]["blend"].NumberOr(0.5)));
  spec->simBones = bones.size();
  const JsonValue& list = root["bones"];
  // (the parts first, so that an emitter's "of" may name a part listed after it)
  for (const JsonValue& b : list.items) {
    MachinerySpec::Part part;
    part.bone = BoneNamed(bones, b["bone"].StringOr(""));
    part.parent = BoneNamed(bones, b["parent"].StringOr(""));
    if (part.bone < 0 || part.parent < 0 || part.parent == part.bone) {
      *why = "bone " + b["bone"].StringOr("?") + " or its parent " + b["parent"].StringOr("?") + " is not in the model (" + std::to_string(bones.size()) + " bones, the last " + (bones.empty() ? std::string() : bones.back().name) + ")";
      return nullptr;
    }
    spec->simBones = std::min(spec->simBones, static_cast<size_t>(part.bone));
    const JsonValue& w = b["building"];
    ReadMotion(w, &part.work);
    ReadMotion(b["idle"], &part.idle);
    if (part.idle.mode == MachinerySpec::Motion::Scroll) part.idle.mode = MachinerySpec::Motion::Still;  // (a belt moves at work only)
    if (part.work.mode == MachinerySpec::Motion::Scroll) {
      const JsonValue& rect = w["uv_rect"];
      if (spec->beltBone < 0 && rect.items.size() >= 4) {
        spec->beltAxis = w["axis"].StringOr("v") == "u" ? 0 : 1;
        const float from = static_cast<float>(rect.items[spec->beltAxis].NumberOr(0));
        const float to = static_cast<float>(rect.items[spec->beltAxis + 2].NumberOr(1));
        spec->beltFrom = std::min(from, to);
        spec->beltLength = std::max(std::fabs(to - from), 1e-5f);
        spec->beltSpeed = static_cast<float>(w["speed"].NumberOr(0));
        spec->beltBone = part.bone;
      }
    }
    // [buildarms] its aim at what is built, and its beams
    auto vec3 = [](const JsonValue& v, Vec3 d) {
      return v.items.size() >= 3 ? Vec3{static_cast<float>(v.items[0].NumberOr(d.x)), static_cast<float>(v.items[1].NumberOr(d.y)), static_cast<float>(v.items[2].NumberOr(d.z))} : d;
    };
    const JsonValue& aim = b["aim"];
    if (aim.kind != JsonValue::Null) {
      part.aim.at = BoneNamed(bones, aim["at"].StringOr(""));
      part.aim.lift = static_cast<float>(aim["lift"].NumberOr(0));
      part.aim.strength = std::clamp(static_cast<float>(aim["strength"].NumberOr(1)), 0.0f, 1.0f);
      part.aim.axis = vec3(aim["axis"], Vec3{0, 0, 1});
      if (Length(part.aim.axis) < 1e-5f) part.aim.axis = Vec3{0, 0, 1};
      part.aim.axis = Normalize(part.aim.axis);
    }
    for (const JsonValue& e : b["beams"].items) {
      MachinerySpec::Beam beam;
      beam.blueprint = e["blueprint"].StringOr("");
      beam.at = BoneNamed(bones, e["at"].StringOr(""));
      beam.lift = static_cast<float>(e["lift"].NumberOr(0));
      beam.width = std::max(0.01f, static_cast<float>(e["width"].NumberOr(0.08)));
      beam.offset = vec3(e["offset"], Vec3{});
      for (const JsonValue& w2 : e["on_keys"].items)
        if (w2.items.size() >= 2) beam.windows.push_back({static_cast<float>(w2.items[0].NumberOr(0)), static_cast<float>(w2.items[1].NumberOr(0))});
      if (beam.at >= 0) part.beams.push_back(beam);
    }
    spec->parts.push_back(part);
  }
  // a part's parent that is a part itself comes before it (the file's order is taken; told when it is not so)
  for (size_t i = 0; i < spec->parts.size(); ++i)
    for (size_t k = i + 1; k < spec->parts.size(); ++k)
      if (spec->parts[i].parent == spec->parts[k].bone) {
        *why = "bone " + bones[spec->parts[i].bone].name + " is listed before its parent";
        return nullptr;
      }
  for (size_t i = 0; i < list.items.size(); ++i)
    for (const JsonValue& e : list.items[i]["emitters"].items) {
      MachinerySpec::Emitter emitter;
      emitter.blueprint = e["emitter"].StringOr("");
      const int of = BoneNamed(bones, e["of"].StringOr(""));
      for (size_t k = 0; k < spec->parts.size(); ++k)
        if (spec->parts[k].bone == of) emitter.of = static_cast<int>(k);
      for (const JsonValue& w : e["on_keys"].items)
        if (w.items.size() >= 2)
          emitter.windows.push_back({static_cast<float>(w.items[0].NumberOr(0)), static_cast<float>(w.items[1].NumberOr(0))});
      if (emitter.of < 0 && e["of"].kind == JsonValue::Null) emitter.of = static_cast<int>(i);  // (its own part's cycle)
      emitter.state = StateNamed(e["state"].StringOr(""), MachinerySpec::State::Building);
      if (!emitter.blueprint.empty() && emitter.of >= 0) spec->parts[i].emitters.push_back(emitter);
    }
  for (const JsonValue& l : root["lamps"].items) {
    MachinerySpec::Lamp lamp;
    lamp.index = static_cast<int>(l["lamp"].NumberOr(-1));
    lamp.period = std::max(0.05f, static_cast<float>(l["period"].NumberOr(2)));
    lamp.phase = static_cast<float>(l["phase"].NumberOr(0));
    lamp.low = std::clamp(static_cast<float>(l["low"].NumberOr(0.2)), 0.0f, 1.0f);
    lamp.beat = l["ease"].StringOr("sine") == "beat";
    lamp.state = StateNamed(l["state"].StringOr(""), MachinerySpec::State::Always);
    if (lamp.index >= 0) spec->lamps.push_back(lamp);
  }
  for (const JsonValue& g : root["glow"].items) {
    MachinerySpec::Glow glow;
    glow.bone = BoneNamed(bones, g["bone"].StringOr(""));
    glow.timing.period = std::max(0.05f, static_cast<float>(g["period"].NumberOr(2)));
    glow.timing.phase = static_cast<float>(g["phase"].NumberOr(0));
    glow.timing.low = std::clamp(static_cast<float>(g["low"].NumberOr(0.2)), 0.0f, 1.0f);
    glow.timing.beat = g["ease"].StringOr("sine") == "beat";
    glow.timing.state = StateNamed(g["state"].StringOr(""), MachinerySpec::State::Always);
    if (glow.bone >= 0) spec->glowParts.push_back(glow);
  }
  spec->glows = !spec->glowParts.empty();
  for (const MachinerySpec::Part& part : spec->parts)
    for (const MachinerySpec::Motion* m : {&part.work, &part.idle})
      for (const MachinerySpec::Key& k : m->keys) spec->scales = spec->scales || k.scale.x != 1 || k.scale.y != 1 || k.scale.z != 1;
  if (spec->parts.empty() && spec->lamps.empty() && spec->glowParts.empty()) {
    *why = "no bones, lamps or glows";
    return nullptr;
  }
  return spec;
}

float Machinery::LampScale(uint32_t unit, size_t lamp) const {
  auto it = units_.find(unit);
  return it == units_.end() || lamp >= it->second.lamps.size() ? 1.0f : it->second.lamps[lamp];
}

void Machinery::Begin(double clock, bool first, Vec3 eye) {
  first_ = first;
  if (!first) return;
  eye_ = eye;
  // (the units not drawn in the frame before are let go, their emitters stopped)
  for (auto it = units_.begin(); it != units_.end();) {
    if (it->second.seen == clock_) {
      ++it;
      continue;
    }
    if (particles_)
      for (uint32_t h : it->second.emitters)
        if (h) particles_->Stop(h);
    it = units_.erase(it);
  }
  lists_.clear();
  scaleLists_.clear();
  glowLists_.clear();
  clock_ = clock;
  posed = 0;
}

float Machinery::PoseUnit(const FrameUnit& unit, const Model& model, Vec3 at, const Quat& turn, const std::string& faction, Pose* pose) {
  const MachinerySpec& spec = *model.machinery;
  // (far off: not worked out; the unit is let go at the next Begin, its emitters stopped, and comes back at rest when near)
  if (Length(at - eye_) > kFar) return -1;
  UnitState& s = units_[unit.id];
  // (MOHO64_DEBUG_MACHINERY: every factory with machinery works, building or not: its parts looked at alone)
  static const bool always = std::getenv("MOHO64_DEBUG_MACHINERY") != nullptr;
  // building: its work going on, its progress moved within the last half second (a paused build, one without the power
  // for it, stands); the parts come to their work and back to rest over the blend
  if (first_) {
    if (unit.workProgress != s.progress) s.progress = unit.workProgress, s.moved = clock_;
    const bool working = always || (unit.workProgress > 0 && unit.workProgress < 1 && !unit.beingBuilt && clock_ - s.moved < 0.5);
    const float dt = s.seen < 0 ? 0.0f : static_cast<float>(std::clamp(clock_ - s.seen, 0.0, 1.0));
    s.work = std::clamp(s.work + (working ? dt : -dt) / spec.blend, 0.0f, 1.0f);
    s.seen = clock_;
    s.belt = std::fmod(s.belt + spec.beltSpeed * s.work * dt, static_cast<double>(spec.beltLength));  // (its belt slows and stops with it)
    if (s.belt < 0) s.belt += spec.beltLength;
  }
  const float w = s.work * s.work * (3 - 2 * s.work);  // (eased)
  const float offset = UnitOffset(unit.id);
  auto cycle = [&](const MachinerySpec::Motion& m) { return Fraction(clock_ / m.period + m.phase + offset); };

  // the simulation's bones as BuildRows reads them: model units, between the two beats
  const float toModel = 1.0f / model.scale;
  auto simPose = [&](size_t bone, Vec3* p, Quat* q) {
    const FrameBone* a = nullptr;
    const FrameBone* b = nullptr;
    for (size_t i = 0; i < pose->nowCount; ++i)
      if (pose->now[i].bone == bone) a = &pose->now[i];
    for (size_t i = 0; i < pose->nextCount; ++i)
      if (pose->next[i].bone == bone) b = &pose->next[i];
    const ModelBone& rest = model.bones[bone];
    auto place = [&](const FrameBone* f) { return f ? Vec3{f->position[0], f->position[1], f->position[2]} * toModel : rest.position; };
    auto rot = [&](const FrameBone* f) { return f ? Quat{f->orientation[0], f->orientation[1], f->orientation[2], f->orientation[3]} : rest.rotation; };
    *p = Lerp(place(a), place(b), pose->blend);
    *q = Nlerp(rot(a), rot(b), pose->blend);
  };

  std::vector<Vec3> placed(spec.parts.size());
  std::vector<Quat> turned(spec.parts.size());
  std::vector<bool> hidden(spec.parts.size(), false);
  std::vector<Vec3> scaled(spec.parts.size(), Vec3{1, 1, 1});
  for (size_t i = 0; i < spec.parts.size(); ++i) {
    const MachinerySpec::Part& part = spec.parts[i];
    // its idle motion (or rest) and its work, blended by how far the factory is at work
    Quat r, workR;
    Vec3 move, workMove, scale{1, 1, 1}, workScale{1, 1, 1};
    bool idleHidden = false, workHidden = false;
    if (w < 1) Evaluate(part.idle, cycle(part.idle), &r, &move, &idleHidden, &scale);
    if (w > 0) Evaluate(part.work, cycle(part.work), &workR, &workMove, &workHidden, &workScale);
    r = Nlerp(r, workR, w);
    move = Lerp(move, workMove, w);
    scaled[i] = Lerp(scale, workScale, w);
    hidden[i] = w >= 0.5f ? workHidden : idleHidden;
    // the parent as posed now (the simulation's bone, or a part before this one), and this part on it as it stands to it
    // at rest, turned and moved about its head in the parent's axes at rest
    Vec3 pp;
    Quat pq;
    const int parent = part.parent;
    size_t of = spec.parts.size();
    for (size_t k = 0; k < i; ++k)
      if (spec.parts[k].bone == parent) of = k;
    if (of < spec.parts.size()) pp = placed[of], pq = turned[of];
    else simPose(static_cast<size_t>(parent), &pp, &pq);
    const ModelBone& restParent = model.bones[parent];
    const ModelBone& rest = model.bones[part.bone];
    const Quat back = Conjugate(restParent.rotation);
    const Quat relQ = Mul(back, Mul(r, rest.rotation));
    const Vec3 relP = Rotate(back, rest.position + move - restParent.position);
    turned[i] = Mul(pq, relQ);
    placed[i] = pp + Rotate(pq, relP);
    // [buildarms] turned toward what is built, by its work: its axis onto the way to the target (the shortest turn)
    if (part.aim.at >= 0 && w > 0) {
      Vec3 tp;
      Quat tq;
      simPose(static_cast<size_t>(part.aim.at), &tp, &tq);
      const Vec3 to = tp + Vec3{0, part.aim.lift * toModel, 0} - placed[i];
      const Vec3 now = Rotate(turned[i], Rotate(Conjugate(rest.rotation), part.aim.axis));
      if (Length(to) > 1e-4f) {
        const Vec3 a = Normalize(now), d = Normalize(to);
        const float c = std::clamp(Dot(a, d), -1.0f, 1.0f);
        Vec3 ax = Cross(a, d);
        if (Length(ax) > 1e-6f) {
          const float angle = std::acos(c) * w * part.aim.strength;
          ax = Normalize(ax);
          const Quat by{ax.x * std::sin(angle * 0.5f), ax.y * std::sin(angle * 0.5f), ax.z * std::sin(angle * 0.5f), std::cos(angle * 0.5f)};
          turned[i] = Mul(by, turned[i]);
        }
      }
    }
    if (parent >= 0 && of < spec.parts.size() && hidden[of]) hidden[i] = true;
  }

  // the lists: the simulation's bones, then the parts' (rising bone order: the parts' bones come after the simulation's)
  for (int which = 0; which < 2; ++which) {
    const FrameBone* src = which == 0 ? pose->now : pose->next;
    const size_t count = which == 0 ? pose->nowCount : pose->nextCount;
    lists_.emplace_back();
    std::vector<FrameBone>& out = lists_.back();
    out.reserve(count + spec.parts.size());
    for (size_t i = 0; i < count; ++i)
      if (src[i].bone < spec.simBones) out.push_back(src[i]);
    std::vector<size_t> order(spec.parts.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spec.parts[a].bone < spec.parts[b].bone; });
    for (size_t i : order) {
      FrameBone f;
      f.bone = static_cast<uint32_t>(spec.parts[i].bone);
      const Vec3 p = placed[i] * model.scale;  // (poses are in world units)
      f.position[0] = p.x, f.position[1] = p.y, f.position[2] = p.z;
      f.orientation[0] = turned[i].x, f.orientation[1] = turned[i].y, f.orientation[2] = turned[i].z, f.orientation[3] = turned[i].w;
      f.hidden = hidden[i];
      out.push_back(f);
    }
    if (out.empty() && spec.glows && !model.bones.empty()) {  // (posed all the same, at rest: its glows ride on its rows)
      FrameBone f;
      const Vec3 p = model.bones[0].position * model.scale;
      f.position[0] = p.x, f.position[1] = p.y, f.position[2] = p.z;
      const Quat& q = model.bones[0].rotation;
      f.orientation[0] = q.x, f.orientation[1] = q.y, f.orientation[2] = q.z, f.orientation[3] = q.w;
      out.push_back(f);
    }
    if (which == 0) pose->now = out.data(), pose->nowCount = out.size();
    else pose->next = out.data(), pose->nextCount = out.size();
  }
  if (spec.scales && !spec.parts.empty()) {  // (the parts' scales, by bone from the first part's: BuildRows)
    uint32_t lo = 0xffffffffu, hi = 0;
    for (const MachinerySpec::Part& part : spec.parts) lo = std::min(lo, static_cast<uint32_t>(part.bone)), hi = std::max(hi, static_cast<uint32_t>(part.bone));
    scaleLists_.emplace_back(static_cast<size_t>(hi - lo + 1) * 3, 1.0f);
    std::vector<float>& sc = scaleLists_.back();
    for (size_t i = 0; i < spec.parts.size(); ++i) {
      float* d = sc.data() + (static_cast<uint32_t>(spec.parts[i].bone) - lo) * 3;
      d[0] = scaled[i].x, d[1] = scaled[i].y, d[2] = scaled[i].z;
    }
    pose->scales = sc.data();
    pose->scaleFrom = lo;
    pose->scaleCount = hi - lo + 1;
  }
  // its lamps' pulse: in their state by how far the unit is at work (outside it as the list has them)
  if (first_) {
    s.lamps.assign(model.lights.size(), 1.0f);
    for (const MachinerySpec::Lamp& l : spec.lamps)
      if (static_cast<size_t>(l.index) < s.lamps.size()) s.lamps[static_cast<size_t>(l.index)] = Pulse(l, clock_, offset, w);
  }
  if (spec.glows) {  // its lit parts' glow, by bone from the first of them (MeshRenderer::BuildRows)
    uint32_t lo = 0xffffffffu, hi = 0;
    for (const MachinerySpec::Glow& g : spec.glowParts) lo = std::min(lo, static_cast<uint32_t>(g.bone)), hi = std::max(hi, static_cast<uint32_t>(g.bone));
    glowLists_.emplace_back(static_cast<size_t>(hi - lo + 1), 1.0f);
    std::vector<float>& gl = glowLists_.back();
    for (const MachinerySpec::Glow& g : spec.glowParts) gl[static_cast<uint32_t>(g.bone) - lo] = Pulse(g.timing, clock_, offset, w);
    pose->glows = gl.data();
    pose->glowFrom = lo;
    pose->glowCount = hi - lo + 1;
  }
  ++posed;

  const float belt = spec.beltBone >= 0 ? static_cast<float>(s.belt) : -1.0f;
  // [buildarms] its parts' build beams while it builds (the first view: the ribbons' beams are the frame's, drawn in each)
  if (first_ && w > 0.5f && ribbons_) {
    auto world = [&](Vec3 p) { return at + Rotate(turn, p * model.scale); };
    for (size_t i = 0; i < spec.parts.size(); ++i) {
      const MachinerySpec::Part& part = spec.parts[i];
      if (part.beams.empty() || hidden[i]) continue;
      const float t = cycle(part.work);
      for (size_t k = 0; k < part.beams.size(); ++k) {
        const MachinerySpec::Beam& beam = part.beams[k];
        bool on = beam.windows.empty();
        for (const auto& win : beam.windows) on = on || (win.first <= win.second ? t >= win.first && t < win.second : t >= win.first || t < win.second);
        if (!on) continue;
        Vec3 tp;
        Quat tq;
        simPose(static_cast<size_t>(beam.at), &tp, &tq);
        const Quat rest = model.bones[part.bone].rotation;
        const Vec3 tip = world(placed[i] + Rotate(turned[i], Rotate(Conjugate(rest), beam.offset * toModel)));
        const Vec3 end = world(tp + Vec3{0, beam.lift * toModel, 0});
        // the game's build beam (the faction's look where the remastered effects have one), its light and sparks as the
        // game's own beams have them (effects.cpp)
        static const std::string kBuildBeam = "/effects/emitters/build_beam_01_emit.bp";
        const std::string& bp = beam.blueprint.empty() ? kBuildBeam : beam.blueprint;
        if (!ribbons_->Beam(bp, tip, end, faction)) ribbons_->PlainBeam(tip, end, Vec3{0.35f, 0.75f, 1.0f}, beam.width);
        if (build_ && build_->On()) {
          std::string unusedRamp;
          Vec3 glow{0, 0, 0};
          if (!ribbons_->Glows(bp, &unusedRamp, &glow, faction)) glow = Vec3{0, 0, 0};
          build_->AddBeam(tip, end, faction, glow, (static_cast<uint64_t>(unit.id) << 32) ^ (static_cast<uint64_t>(i + 1) << 8) ^ k ^ (1ull << 62));
        }
      }
    }
  }
  // its emitters (the first view): playing while their part's cycle is within a window, on the part they are on
  if (!first_ || !particles_) return belt;
  size_t n = 0;
  for (const MachinerySpec::Part& part : spec.parts) n += part.emitters.size();
  s.emitters.resize(n, 0);
  size_t e = 0;
  for (size_t i = 0; i < spec.parts.size(); ++i)
    for (const MachinerySpec::Emitter& em : spec.parts[i].emitters) {
      uint32_t& handle = s.emitters[e++];
      const bool atWork = w >= 0.5f;
      bool on = !hidden[i] && (em.state == MachinerySpec::State::Always || (em.state == MachinerySpec::State::Building) == atWork);
      if (on && !em.windows.empty()) {
        const MachinerySpec::Part& of = spec.parts[em.of];
        const float u = cycle(atWork || of.idle.mode == MachinerySpec::Motion::Still ? of.work : of.idle);
        bool in = false;
        for (const auto& win : em.windows) in = in || (u >= win.first && u <= win.second);
        on = in;
      }
      const Vec3 where = at + Rotate(turn, placed[i] * model.scale);
      const Quat facing = Mul(turn, turned[i]);
      if (on && !handle) handle = particles_->Start(em.blueprint, where, facing, 1, -1, true, faction);
      else if (on) particles_->Move(handle, where, facing);
      else if (handle) particles_->Stop(handle), handle = 0;
    }
  return belt;
}

}  // namespace render
