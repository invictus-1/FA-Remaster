#include "machinery.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "json.h"
#include "particles.h"

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
      key.sine = k.items.size() > 3 ? k.items[3].StringOr(sine ? "sine" : "linear") != "linear" : sine;
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
void Evaluate(const MachinerySpec::Motion& m, float u, Quat* r, Vec3* move, bool* hidden) {
  *r = Quat{};
  *move = Vec3{};
  *hidden = false;
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
      if (!emitter.blueprint.empty() && emitter.of >= 0) spec->parts[i].emitters.push_back(emitter);
    }
  if (spec->parts.empty()) {
    *why = "no bones";
    return nullptr;
  }
  return spec;
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
  for (size_t i = 0; i < spec.parts.size(); ++i) {
    const MachinerySpec::Part& part = spec.parts[i];
    // its idle motion (or rest) and its work, blended by how far the factory is at work
    Quat r, workR;
    Vec3 move, workMove;
    bool idleHidden = false, workHidden = false;
    if (w < 1) Evaluate(part.idle, cycle(part.idle), &r, &move, &idleHidden);
    if (w > 0) Evaluate(part.work, cycle(part.work), &workR, &workMove, &workHidden);
    r = Nlerp(r, workR, w);
    move = Lerp(move, workMove, w);
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
    if (which == 0) pose->now = out.data(), pose->nowCount = out.size();
    else pose->next = out.data(), pose->nextCount = out.size();
  }
  ++posed;

  const float belt = spec.beltBone >= 0 ? static_cast<float>(s.belt) : -1.0f;
  // its emitters (the first view): playing while their part's cycle is within a window, on the part they are on
  if (!first_ || !particles_) return belt;
  size_t n = 0;
  for (const MachinerySpec::Part& part : spec.parts) n += part.emitters.size();
  s.emitters.resize(n, 0);
  size_t e = 0;
  for (size_t i = 0; i < spec.parts.size(); ++i)
    for (const MachinerySpec::Emitter& em : spec.parts[i].emitters) {
      uint32_t& handle = s.emitters[e++];
      bool on = w >= 0.5f && !hidden[i];
      if (on) {
        const float u = cycle(spec.parts[em.of].work);
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
