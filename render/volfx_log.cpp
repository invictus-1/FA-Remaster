#include "volfx_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "frame.h"
#include "particles.h"

namespace render {
namespace {

constexpr uint32_t kNone = 0xffffffffu;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// "/projectiles/TIFArtillery01/TIFArtillery01_proj.bp" -> "TIFArtillery01" (the folder the blueprint is in)
std::string Short(const std::string& path) {
  const size_t end = path.rfind('/');
  if (end == std::string::npos || end == 0) return path;
  const size_t begin = path.rfind('/', end - 1);
  return path.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1));
}

// The largest value and spread a curve gives (y + half its spread).
float Most(const EmitterCurve& c) {
  float most = 0;
  for (const Vec3& k : c.keys) most = std::max(most, k.y + std::fabs(k.z) * 0.5f);
  return most;
}

// About how many particles an emitter throws out over its life (its emit rate summed tick by tick; a looping one's
// first ten seconds).
float Count(const EmitterBlueprint& bp) {
  const int ticks = bp.lifetime > 0 ? std::min(static_cast<int>(std::ceil(bp.lifetime)), 100) : 100;
  float n = 0;
  for (int t = 0; t < ticks; ++t) {
    float x = static_cast<float>(t);
    if (bp.repeatTime > 0) x = std::fmod(x, bp.repeatTime);
    float value = 0, spread = 0;
    bp.emitRate.At(x, &value, &spread);
    n += std::max(0.0f, value);
  }
  return n;
}

// The few most often of a tally of names, "name x3, other x1".
std::string Top(const std::map<std::string, int>& counts, size_t most) {
  std::vector<std::pair<int, std::string>> v;
  for (const auto& [name, n] : counts) v.push_back({-n, name});
  std::sort(v.begin(), v.end());
  std::string out;
  for (size_t i = 0; i < v.size() && i < most; ++i) out += (i ? ", " : "") + v[i].second + " x" + std::to_string(-v[i].first);
  if (v.size() > most) out += ", ... (" + std::to_string(v.size()) + " kinds)";
  return out.empty() ? "-" : out;
}

// What an event's origin is on, without the call ("unit:uel0303:dead").
std::string OnWhat(const std::string& origin) {
  const size_t space = origin.find(' ');
  return space == std::string::npos ? origin : origin.substr(space + 1);
}

}  // namespace

VolfxLog* VolfxLog::Get() {
  static const std::unique_ptr<VolfxLog> log(std::getenv("MOHO64_DEBUG_VOLFX") ? new VolfxLog : nullptr);
  return log.get();
}

VolfxLog::~VolfxLog() {
  for (const Blast& b : blasts_) Print(b);
  blasts_.clear();
  for (Started& s : started_) Finish(s);
  started_.clear();
  Summary("at the end");
}

void VolfxLog::SetOrigin(const FrameEffectEvent& event, const Frame& frame) {
  effect_ = event.id;
  std::string what = "none";
  const uint32_t entity = event.entity;
  const auto* names = frame.blueprints;
  auto name = [&](uint32_t i) { return names && i < names->size() ? (*names)[i] : std::string("?"); };
  if (entity != kNone) {
    auto unit = std::lower_bound(frame.units.begin(), frame.units.end(), entity, [](const FrameUnit& u, uint32_t id) { return u.id < id; });
    if (unit != frame.units.end() && unit->id == entity) {
      what = "unit:" + Lower(name(unit->blueprint)) + (unit->dead ? ":dead" : ":alive");
    } else {
      what.clear();
      for (const FrameProjectile& p : frame.projectiles)
        if (p.id == entity) what = "shot:" + Short(name(p.blueprint));
      if (what.empty())
        for (const FrameEntity& f : frame.entities)
          if (f.id == entity) what = "entity:" + (name(f.mesh).empty() ? std::string("(no mesh)") : name(f.mesh));
      if (what.empty()) what = shots_.count(entity) ? "shot:" + shots_.at(entity) + ":gone" : "gone";
    }
  }
  origin_ = event.name + " " + what;
}

void VolfxLog::SeeShots(const Frame& frame) {
  if (shots_.size() > 200000) shots_.clear();
  const auto* names = frame.blueprints;
  for (const FrameProjectile& p : frame.projectiles)
    if (names && p.blueprint < names->size()) shots_[p.id] = Short((*names)[p.blueprint]);
}

void VolfxLog::Scaled(uint32_t effect, float scale) { scales_[effect] = scale; }

void VolfxLog::SetArea(const std::string& weapon, const std::string& projectile, const std::string& damageType, float amount) {
  char buffer[64];
  snprintf(buffer, sizeof buffer, " %s %.0f", damageType.c_str(), amount);
  area_ = std::string(buffer) + " by " + (weapon.empty() ? "?" : weapon) + (projectile.empty() ? "" : " shot " + projectile);
}

void VolfxLog::Emitter(const std::string& path, const EmitterBlueprint& bp, Vec3 at, float scale, double born, bool replaced) {
  Started s;
  s.born = born;
  s.at = at;
  s.path = path;
  s.texture = bp.texturePath;
  s.ramp = bp.rampPath;
  s.origin = origin_.empty() ? "direct (the renderer's own, or the stand's)" : origin_;
  s.effect = origin_.empty() ? 0 : effect_;
  s.scale = scale;
  s.size = bp.largestSize;
  s.lifetime = bp.lifetime;
  s.particleLife = bp.longestLife;
  s.count = Count(bp);
  s.speed = Most(bp.velocity);
  s.blend = bp.blendMode;
  s.flat = bp.flat;
  s.replaced = replaced;
  s.sixWay = !bp.sixWayPath.empty();
  s.normals = !bp.normalPath.empty();
  started_.push_back(std::move(s));
}

void VolfxLog::Offered(bool flash, float size, const std::string& ramp, Vec3 at, double born) {
  (void)at;
  (void)born;
  char buffer[96];
  if (flash) snprintf(buffer, sizeof buffer, "flash %.1f %s", size, ramp.empty() ? "-" : Short(ramp + "/x").c_str());
  else snprintf(buffer, sizeof buffer, "area %.1f", size);
  offer_ = std::string(buffer) + (flash ? " on " + (origin_.empty() ? std::string("? (no event: a test)") : OnWhat(origin_)) : area_);
}

void VolfxLog::Into(Vec3 at, double born, float radius, bool nuke, bool fresh) {
  if (fresh) {
    Blast b;
    b.id = nextId_++;
    b.born = born;
    b.at = at;
    b.radius = radius;
    b.nuke = nuke;
    b.made.push_back(offer_);
    blasts_.push_back(std::move(b));
  } else {
    for (Blast& b : blasts_)
      if (b.born == born && b.at.x == at.x && b.at.y == at.y && b.at.z == at.z) {
        b.radius = std::max(b.radius, radius);
        b.nuke = b.nuke || nuke;
        if (b.made.size() < 12) b.made.push_back(offer_);
        else if (b.made.size() == 12) b.made.push_back("...");
        break;
      }
  }
  offer_.clear();
}

void VolfxLog::Declined(bool flash, float size) {
  if (size < (flash ? 5.0f : 3.0f)) return;  // (too small to have been a blast anyway: VolumeBlasts::smallest, smallestArea)
  std::string what = OnWhat(origin_.empty() ? std::string("? ?") : origin_);
  if (!flash) {  // (" Normal 181 by ueb2204-1-AAGun shot /projectiles/x/x_proj.bp": the weapon's unit and the shot)
    const size_t by = area_.find(" by ");
    what = by == std::string::npos ? area_ : area_.substr(by + 4);
    const size_t dash = what.find('-'), shot = what.find(" shot ");
    if (dash != std::string::npos && (shot == std::string::npos || dash < shot))
      what = what.substr(0, dash) + (shot == std::string::npos ? "" : " " + Short(what.substr(shot + 6)));
  } else if (what.compare(0, 5, "unit:") == 0) {
    what = what.substr(0, what.rfind(':')) + (what.find(":alive") != std::string::npos ? " (living)" : " (dead)");
  }
  ++declined_[(flash ? "flash on " : "area by ") + what];
  ++declinedCount_;
}

std::string VolfxLog::Hint(const Started& s, float radius) const {
  const std::string name = Lower(s.path) + " " + Lower(s.texture);
  auto has = [&](const char* w) { return name.find(w) != std::string::npos; };
  std::string kind = "-";
  if (s.blend == 5) kind = "refraction";
  else if (has("spark")) kind = "sparks";
  else if (has("debris") || has("fragment") || has("shrapnel") || has("chunk") || has("rock")) kind = "debris";
  else if (has("trail") || has("streak") || has("line")) kind = "streaks";
  else if (has("ring") || has("wave") || has("shock")) kind = "ring";
  else if (has("flash") || has("flare") || has("glow") || has("light") || has("shine")) kind = "flash";
  else if (has("smoke") || has("cloud") || has("dust")) kind = "smoke";
  else if (has("fire") || has("flame") || has("explo") || has("fireball") || has("plasma")) kind = "fire";
  const float size = s.size * s.scale;
  const bool big = size >= std::max(1.5f, 0.3f * radius);
  std::string body = "small";
  if (s.flat) body = "flat";
  else if (big && (s.blend == 2 || s.blend == 3 || s.blend == 4)) body = "BIG-ADD";
  else if (big && (s.blend == 0 || s.blend == 1) && s.particleLife >= 10) body = "BIG-ALPHA";
  else if (big) body = "big-brief";
  return kind + "/" + body;
}

void VolfxLog::Print(const Blast& b) {
  printf("volfx\tblast\t#%d\tt=%.1f\tat=%.1f,%.1f,%.1f\tr=%.2f\t%s\tmade:", b.id, b.born, b.at.x, b.at.y, b.at.z, b.radius, b.nuke ? "NUKE" : "blast");
  for (const std::string& m : b.made) printf(" [%s]", m.c_str());
  printf("\n");
  // the emitters started with it, one line per blueprint: how many, when (seconds from the blast's birth), how far from
  // its middle (horizontally), at what scales, on what
  struct Group {
    int n = 0, replaced = 0;
    double dtLow = 1e9, dtHigh = -1e9;
    float dLow = 1e9f, dHigh = 0, scaleLow = 1e9f, scaleHigh = 0, upLow = 1e9f, upHigh = -1e9f;
    const Started* largest = nullptr;
    std::map<std::string, int> on;
  };
  std::map<std::string, Group> groups;
  int listed = 0;
  for (Started& s : started_) {
    const double dt = s.born - b.born;
    if (dt < -(b.nuke ? 1.0 : window) - 1e-6 || dt > (b.nuke ? nukeWindow : window) + 1e-6) continue;
    const float d = std::hypot(s.at.x - b.at.x, s.at.z - b.at.z), up = s.at.y - b.at.y;
    if (d > b.radius + 0.5f || up < -b.radius - 1 || up > b.radius * 2.5f + 1) continue;
    s.matched = true;
    auto scaled = scales_.find(s.effect);
    if (s.effect && scaled != scales_.end()) s.scale = scaled->second;
    Group& g = groups[s.path];
    ++g.n;
    if (s.replaced) ++g.replaced;
    g.dtLow = std::min(g.dtLow, dt), g.dtHigh = std::max(g.dtHigh, dt);
    g.dLow = std::min(g.dLow, d), g.dHigh = std::max(g.dHigh, d);
    g.upLow = std::min(g.upLow, up), g.upHigh = std::max(g.upHigh, up);
    g.scaleLow = std::min(g.scaleLow, s.scale), g.scaleHigh = std::max(g.scaleHigh, s.scale);
    if (!g.largest || s.scale > g.largest->scale) g.largest = &s;
    ++g.on[OnWhat(s.origin)];
    ++listed;
    Tally& t = tally_[s.path];
    ++t.startsIn;
    t.largest = std::max(t.largest, s.size * s.scale);
    ++t.inOn[OnWhat(s.origin)];
  }
  for (const auto& [path, g] : groups) {
    const Started& s = *g.largest;
    const std::string hint = Hint(s, b.radius);
    Tally& t = tally_[path];
    ++t.blasts;
    t.texture = s.texture;
    t.ramp = s.ramp;
    t.blend = s.blend;
    t.hint = hint;
    const std::string call = s.origin.substr(0, s.origin.find(' '));
    printf("volfx\temitter\t#%d\tx%d\tdt=%+.1f..%+.1f\td=%.1f..%.1f\td/r<=%.2f\tup=%.1f..%.1f\t%s\ttex=%s\tramp=%s\tblend=%d\tsize=%.2f\tscale=%.2f..%.2f\tlife=%g\tplife=%g\tn=%.0f\tspeed=%.2f\tflat=%d%s%s\t%s\t%s %s\thint=%s\n",
           b.id, g.n, g.dtLow, g.dtHigh, g.dLow, g.dHigh, g.dHigh / std::max(b.radius, 0.01f), g.upLow, g.upHigh, path.c_str(), s.texture.c_str(),
           s.ramp.empty() ? "-" : s.ramp.c_str(), s.blend, s.size, g.scaleLow, g.scaleHigh, s.lifetime, s.particleLife, s.count, s.speed, s.flat ? 1 : 0,
           s.sixWay ? "\tsixway" : "", s.normals ? "\tnormals" : "", g.replaced == g.n ? "REPLACED" : g.replaced ? "partly-replaced" : "drawn", call.c_str(),
           Top(g.on, 3).c_str(), hint.c_str());
  }
  printf("volfx\tend\t#%d\t%d emitters, %zu blueprints\n", b.id, listed, groups.size());
  ++printed_;
}

void VolfxLog::Finish(Started& s) {
  Tally& t = tally_[s.path];
  ++t.starts;
  if (s.replaced) ++t.replaced;
  if (!s.matched) {
    ++t.out;
    ++t.outOn[OnWhat(s.origin)];
  }
  if (s.effect) scales_.erase(s.effect);
}

void VolfxLog::Tick(double clock) {
  if (clock < clock_ - 2) {  // (the clock went back: a stand that starts over; what is kept is decided now)
    for (const Blast& b : blasts_) Print(b);
    blasts_.clear();
    for (Started& s : started_) Finish(s);
    started_.clear();
    lastSummary_ = clock;
  }
  clock_ = clock;
  // a blast once whatever is told of it has been (Start merges what comes within 0.8 s), its emitters once no blast
  // born near them is still to come
  for (size_t i = 0; i < blasts_.size();) {
    if (blasts_[i].born + (blasts_[i].nuke ? nukeWindow : window) + 0.8 <= clock) {
      Print(blasts_[i]);
      blasts_.erase(blasts_.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
  double keepFrom = 1e300;  // (what a nuke still to be printed may claim: from a second before it)
  for (const Blast& b : blasts_)
    if (b.nuke) keepFrom = std::min(keepFrom, b.born - 1.0);
  while (!started_.empty() && started_.front().born + 2 * window + 1.0 <= clock && started_.front().born < keepFrom) {
    Finish(started_.front());
    started_.pop_front();
  }
  if (started_.size() > 100000) {  // (a clock that stands still while emitters pile up)
    Finish(started_.front());
    started_.pop_front();
  }
  if (clock - lastSummary_ >= 300) {
    lastSummary_ = clock;
    Summary("so far");
  }
}

void VolfxLog::Summary(const char* why) {
  std::vector<std::pair<int, std::string>> order;
  for (const auto& [path, t] : tally_)
    if (t.blasts > 0 || t.replaced > 0) order.push_back({-(t.blasts * 100000 + t.replaced), path});
  std::sort(order.begin(), order.end());
  printf("volfx\tsummary\t%s\tt=%.1f\t%d blasts printed\t%zu emitters doubled or replaced\t%d declined\n", why, clock_, printed_,
         order.size(), declinedCount_);
  {  // what made no blast that would have been one before (Declined), the most first
    std::vector<std::pair<int, std::string>> v;
    for (const auto& [what, n] : declined_) v.push_back({-n, what});
    std::sort(v.begin(), v.end());
    for (const auto& [n, what] : v) printf("volfx\tdeclined\t%d\t%s\n", -n, what.c_str());
  }
  for (const auto& [unused, path] : order) {
    const Tally& t = tally_.at(path);
    printf("volfx\tsum\t%s\tblasts=%d\tstartsIn=%d\tstarts=%d\tout=%d\treplaced=%d\tlargest=%.2f\ttex=%s\tramp=%s\tblend=%d\thint=%s\tin_on=%s\tout_on=%s\n",
           path.c_str(), t.blasts, t.startsIn, t.starts, t.out, t.replaced, t.largest, t.texture.c_str(), t.ramp.empty() ? "-" : t.ramp.c_str(), t.blend,
           t.hint.c_str(), Top(t.inOn, 4).c_str(), Top(t.outOn, 6).c_str());
  }
  fflush(stdout);
}

}  // namespace render
