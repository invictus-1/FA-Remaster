// The per-beat fingerprint (MOHO64_FINGERPRINT): sim/fingerprint.h says what it is and what the file holds.
#include "sim/fingerprint.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"
#include "script/lua.hpp"
#include "sim/ai_platoon.h"
#include "sim/commands.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

// A 64-bit running hash of 32-bit words (a multiply-xorshift step per word: not for adversaries, only to tell two
// states apart).
struct Hash {
  uint64_t h = 0xcbf29ce484222325ull;
  void U32(uint32_t v) {
    h = (h ^ v) * 0x9e3779b97f4a7c15ull;
    h ^= h >> 29;
  }
  void U64(uint64_t v) {
    U32(static_cast<uint32_t>(v));
    U32(static_cast<uint32_t>(v >> 32));
  }
  void I(int64_t v) { U64(static_cast<uint64_t>(v)); }
  void B(bool v) { U32(v ? 1 : 0); }
  void F(float f) {  // the bits, not the value: -0 and 0 differ, and so do two NaNs
    uint32_t v;
    std::memcpy(&v, &f, 4);
    U32(v);
  }
  void V(const Vec3& v) {
    F(v.x);
    F(v.y);
    F(v.z);
  }
  void Q(const Quat& q) {
    F(q.x);
    F(q.y);
    F(q.z);
    F(q.w);
  }
  void S(std::string_view s) {
    U32(static_cast<uint32_t>(s.size()));
    uint32_t w = 0;
    size_t i = 0;
    for (; i < s.size(); ++i) {
      w = (w << 8) | static_cast<unsigned char>(s[i]);
      if ((i & 3) == 3) {
        U32(w);
        w = 0;
      }
    }
    if (i & 3) U32(w);
  }
};

std::string_view BpName(const Entity* e) { return e->blueprint ? std::string_view(e->blueprint->id) : "-"; }
const char* BpId(const Entity* e) { return e->blueprint ? e->blueprint->id.c_str() : "-"; }
int ArmyIndex(const Entity* e) { return e->army ? e->army->index : 0; }

// Entities of one kind in id order (ties, which live entities do not have, by creation handle). The lists are
// mostly in that order already (creation order, ids counting up): then nothing is sorted.
template <class T>
std::vector<const T*> ById(const std::vector<T*>& v) {
  std::vector<const T*> out(v.begin(), v.end());
  auto less = [](const T* a, const T* b) { return a->id != b->id ? a->id < b->id : a->handle < b->handle; };
  if (!std::is_sorted(out.begin(), out.end(), less)) std::sort(out.begin(), out.end(), less);
  return out;
}

void HashTarget(Hash& h, const AiTarget& t) {
  h.I(t.type);
  h.U32(t.entityId);
  h.V(t.pos);
  h.I(t.aimBone);
  h.B(t.mobile);
  h.B(t.viaBlip);
  h.U32(t.blipId);
  h.B(t.pastDeath);
  h.U32(t.fakeId);
}

void HashCommand(Hash& h, const UnitCommand& c) {
  h.U32(c.id);
  h.I(static_cast<int>(c.type));
  h.V(c.pos);
  h.B(c.hasPos);
  h.U32(c.targetId);
  h.I(c.targetPoint);
  h.B(c.targetViaBlip);
  h.U32(c.targetFakeId);
  h.S(c.blueprintId);
  h.I(c.count);
  h.I(c.maxCount);
  h.U32(static_cast<uint32_t>(c.units.size()));
}

void HashUnit(Hash& h, const Unit* u) {
  h.U32(u->id);
  h.S(BpName(u));
  h.V(u->position);
  h.Q(u->orientation);
  h.F(u->health);
  h.F(u->maxHealth);
  h.F(u->fractionComplete);
  h.F(u->workProgress);
  h.S(u->layer);
  h.B(u->dead);
  h.B(u->destroyQueued);
  h.B(u->beingBuilt);
  h.B(u->busy);
  h.B(u->paused);
  h.B(u->autoMode);
  h.B(u->repeatQueue);
  h.I(u->fireState);
  h.I(u->stunned);
  h.U32(u->scriptBits);
  h.U32(static_cast<uint32_t>(u->unitStates.size()));
  for (const std::string& s : u->unitStates) h.S(s);  // (a std::set of names: sorted)
  h.V(u->motion.vel);
  h.F(u->motion.maxSpeedU594);
  h.U32(u->focusId);
  h.U32(u->guardedId);
  h.U32(u->parentId);
  h.U32(u->transportedBy);
  h.I(u->siloAmmo[0]);
  h.I(u->siloAmmo[1]);
  h.F(u->fuelRatio);
  h.F(u->shieldRatio);
  h.F(u->resourceConsumed);
  for (int i = 0; i < 2; ++i) {
    h.F(u->consumption[i]);
    h.F(u->production[i]);
    h.F(u->producedTick[i]);
    h.F(u->consumedTick[i]);
  }
  h.F(u->buildRate);
  h.V(u->formSlot);
  h.F(u->formRank);
  for (uint8_t r : u->recon) h.U32(r);
  h.U32(static_cast<uint32_t>(u->commands.size()));
  for (const auto& c : u->commands)
    if (c) HashCommand(h, *c);
  h.U32(static_cast<uint32_t>(u->factoryCommands.size()));
  for (const auto& c : u->factoryCommands)
    if (c) HashCommand(h, *c);
  h.U32(static_cast<uint32_t>(u->weapons.size()));
  for (const UnitWeapon* w : u->weapons) {
    h.B(w->enabled);
    HashTarget(h, w->target);
    h.B(w->onTarget);
    h.B(w->canReach);
    h.I(w->fireClock);
    h.I(w->shots);
    h.I(w->missCount);
    h.U32(w->nextAcquire);
    h.B(w->fireSuspended);
  }
}

void HashArmy(Hash& h, const Army& a) {
  const ArmyEconomy& e = a.econ;
  for (int i = 0; i < 2; ++i) {
    h.F(e.income[i]);
    h.F(e.incomeTrend[i]);
    h.F(e.stored[i]);
    h.F(e.incomeStat[i]);
    h.F(e.reclaimed[i]);
    h.F(e.requested[i]);
    h.F(e.usage[i]);
    h.I(e.maxStorage[i]);
    h.F(e.brainStorage[i]);
    h.F(e.peak[i]);
  }
  h.B(e.sharing);
  h.U32(static_cast<uint32_t>(e.requests.size()));
  for (const auto& [name, v] : a.stats) {  // (std::map: by name)
    h.S(name);
    h.F(v);
  }
  for (const auto& [name, per] : a.bpStats) {
    h.S(name);
    for (const auto& [bit, v] : per) {
      h.I(bit);
      h.F(v);
    }
  }
  h.F(a.unitCap);
  h.F(a.unitCost);
  h.B(a.outOfGame);
  h.B(a.ignoreUnitCap);
  h.U32(a.serial);
  h.U32(static_cast<uint32_t>(a.units.size()));
  for (int v : a.alliance) h.I(v);
  h.U32(static_cast<uint32_t>(a.platoons.size()));
  for (const Platoon* p : a.platoons) {  // (creation order, as the army keeps them)
    h.S(p->uniqueName);
    h.S(p->plan);
    for (const auto& s : p->squads) {
      h.I(s->cls);
      h.U32(static_cast<uint32_t>(s->units.size()));
      for (const Unit* u : s->units) h.U32(u->id);  // (an EntitySet: ascending id)
    }
  }
}

void HashEntity(Hash& h, const Entity* e) {
  h.U32(e->id);
  h.I(static_cast<int>(e->kind));
  h.S(BpName(e));
  h.I(ArmyIndex(e));
  h.V(e->position);
  h.Q(e->orientation);
  h.F(e->health);
  h.F(e->maxHealth);
  h.F(e->fractionComplete);
  h.B(e->dead);
  h.B(e->destroyQueued);
  h.U32(e->attachParent);
}

std::string Hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

// "x y z" with 9 significant digits: a float's exact value
std::string Fs(std::initializer_list<float> fs) {
  std::string s;
  char b[32];
  for (float f : fs) {
    std::snprintf(b, sizeof b, s.empty() ? "%.9g" : " %.9g", f);
    s += b;
  }
  return s;
}

std::string TargetText(const AiTarget& t) {
  char b[160];
  if (t.type == 0) return "-";
  if (t.type == 1)
    std::snprintf(b, sizeof b, "e%#x%s%s", t.entityId, t.viaBlip ? "/blip" : "", t.fakeId ? "/fake" : "");
  else
    std::snprintf(b, sizeof b, "g(%s)", Fs({t.pos.x, t.pos.y, t.pos.z}).c_str());
  return b;
}

std::string CommandText(const UnitCommand& c) {
  char b[200];
  std::snprintf(b, sizeof b, "[%#x t%d %s%s%s%s x%d]", c.id, static_cast<int>(c.type),
                c.targetId ? ("e" + std::to_string(c.targetId) + " ").c_str() : "",
                c.hasPos ? ("(" + Fs({c.pos.x, c.pos.y, c.pos.z}) + ") ").c_str() : "", c.blueprintId.c_str(),
                c.units.size() > 1 ? (" shared" + std::to_string(c.units.size())).c_str() : "", c.count);
  return b;
}

}  // namespace

std::unique_ptr<Fingerprint> Fingerprint::FromEnvironment() {
  const char* path = getenv("MOHO64_FINGERPRINT");
  if (!path || !*path) return nullptr;
  auto f = std::unique_ptr<Fingerprint>(new Fingerprint);
  f->path_ = path;
  f->out_ = std::fopen(path, "wb");
  if (!f->out_) {
    Logf(LogLevel::Warning, "moho64: cannot open the fingerprint file %s", path);
    return nullptr;
  }
  if (const char* p = getenv("MOHO64_FINGERPRINT_PEER"); p && *p) f->peerPath_ = p;
  if (const char* w = getenv("MOHO64_FINGERPRINT_WAIT"); w && *w) f->waitSeconds_ = std::atof(w);
  if (const char* d = getenv("MOHO64_FINGERPRINT_DETAIL"); d && *d) {
    for (const char* p = d; *p;) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(p, &e, 10);
      if (e == p) break;
      f->detailBeats_.insert(static_cast<uint32_t>(v));
      p = *e ? e + 1 : e;
    }
  }
  Logf(LogLevel::Info, "moho64: fingerprint to %s%s%s", path, f->peerPath_.empty() ? "" : ", checked against ",
       f->peerPath_.c_str());
  return f;
}

Fingerprint::~Fingerprint() {
  if (out_) std::fclose(out_);
  if (peer_) std::fclose(peer_);
}

std::string Fingerprint::Line(const Sim& sim, uint32_t beat) const {
  std::vector<std::pair<std::string, uint64_t>> parts;
  {
    Hash h;
    h.U32(sim.sessionSeed());
    h.U64(sim.rngDraws_);
    parts.emplace_back("rng", h.h);
  }
  {  // the sim's own: the path searches waiting their turn (in their order), command ids, pause, playable rect (not
     // the focus army: each player's copy of a multiplayer game has its own, tools/twocopies.py --focus-a / -b)
    Hash h;
    h.U32(static_cast<uint32_t>(sim.pathQueue.size()));
    for (const Unit* u : sim.pathQueue) h.U32(u ? u->id : 0xffffffffu);
    h.U32(sim.nextCommandId);
    h.I(sim.commandSource);
    h.I(sim.pauseOwner);
    h.B(sim.endGame);
    for (int v : sim.playableRect) h.I(v);
    h.U32(static_cast<uint32_t>(sim.commandsById.size()));
    h.U32(static_cast<uint32_t>(sim.streamCommands.size()));
    parts.emplace_back("sim", h.h);
  }
  std::string lua = "-";  // (not hashed: the numbers themselves, the heap and threshold in KB, and the serial)
  if (lua_State* L = sim.L())
    lua = std::to_string(lua_getgccount(L)) + ":" + std::to_string(lua_getgcthreshold(L)) + ":" +
          std::to_string(lua_getnextserial(L));
  const auto& armies = sim.armies();
  for (const auto& a : armies) {
    Hash h;
    HashArmy(h, *a);
    parts.emplace_back("a" + std::to_string(a->index), h.h);
  }
  {
    std::vector<Hash> per(armies.size() + 1);
    for (const Unit* u : ById(sim.units())) {
      const int i = ArmyIndex(u);
      HashUnit(per[i >= 0 && i <= static_cast<int>(armies.size()) ? i : 0], u);
    }
    for (size_t i = 1; i < per.size(); ++i) parts.emplace_back("u" + std::to_string(i), per[i].h);
    if (per[0].h != Hash().h) parts.emplace_back("u0", per[0].h);  // (units of no army: none expected)
  }
  {
    Hash h;
    for (const Projectile* p : ById(sim.projectiles)) {
      HashEntity(h, p);
      h.V(p->velocity);
      HashTarget(h, p->target);
      h.F(p->damage);
      h.F(p->damageRadius);
      h.U32(p->expireTick);
      h.S(p->layer);
      h.I(p->bounces);
      h.F(p->hitFraction);
    }
    parts.emplace_back("proj", h.h);
  }
  {
    Hash h;
    for (const Prop* p : ById(sim.props)) HashEntity(h, p);
    parts.emplace_back("props", h.h);
  }
  {
    std::vector<const Entity*> others;
    for (const auto& [handle, e] : sim.entities())
      if (e->kind != Entity::Kind::Unit && e->kind != Entity::Kind::Prop && e->kind != Entity::Kind::Projectile)
        others.push_back(e);
    std::sort(others.begin(), others.end(), [](const Entity* a, const Entity* b) {
      return a->id != b->id ? a->id < b->id : a->handle < b->handle;
    });
    Hash h;
    for (const Entity* e : others) HashEntity(h, e);
    parts.emplace_back("ents", h.h);
  }
  Hash total;
  for (const auto& [name, v] : parts) total.U64(v);
  std::string line = std::to_string(beat) + " " + std::to_string(sim.tick()) + " " + Hex(total.h);
  for (const auto& [name, v] : parts) line += " " + name + "=" + Hex(v);
  return line + " lua=" + lua;
}

bool Fingerprint::Beat(const Sim& sim, uint32_t beat) {
  if (!headerDone_) {
    headerDone_ = true;
    std::fprintf(out_, "# moho64 fingerprint 1 seed %u armies %zu\n", sim.sessionSeed(), sim.armies().size());
  }
  const auto t0 = std::chrono::steady_clock::now();
  const std::string line = Line(sim, beat);
  hashSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ++beats_;
  std::fprintf(out_, "%s\n", line.c_str());
  if (!peerPath_.empty()) std::fflush(out_);  // (the peer reads it now; alone, the end flushes it)
  if (detailBeats_.count(beat)) WriteDetail(sim, beat, path_ + ".detail." + std::to_string(beat), "asked for");
  if (peerPath_.empty() || peerGone_) return true;
  std::string theirs;
  if (!PeerLine(beat, &theirs)) return true;
  // the beat, the tick and the total (lua's part is not in it)
  auto head = [](const std::string& l) {
    size_t p = 0;
    for (int i = 0; i < 3 && p != std::string::npos; ++i) p = l.find(' ', p + 1);
    return l.substr(0, p);
  };
  if (head(theirs) == head(line)) return true;
  std::fprintf(out_, "# differs %u\n", beat);
  std::fflush(out_);
  Logf(LogLevel::Warning, "fingerprint: beat %u (tick %u) differs from the peer's:\n  ours   %s\n  theirs %s", beat,
       sim.tick(), line.c_str(), theirs.c_str());
  WriteDetail(sim, beat, path_ + ".detail", "the first beat that differs from the peer's");
  return false;
}

void Fingerprint::End(uint32_t beats) {
  std::fprintf(out_, "# end %u\n", beats);
  std::fflush(out_);
  Logf(LogLevel::Info, "fingerprint: %u beats, %.2f s spent hashing (%.3f ms a beat)", beats_, hashSeconds_,
       beats_ ? hashSeconds_ * 1000.0 / beats_ : 0.0);
}

// The peer's line of `beat`: lines are read as the peer writes them; earlier beats' lines are passed over.
bool Fingerprint::PeerLine(uint32_t beat, std::string* line) {
  auto lastNews = std::chrono::steady_clock::now();
  int spins = 0;
  for (;;) {
    if (!peer_) peer_ = std::fopen(peerPath_.c_str(), "rb");
    if (peer_) {
      char buf[4096];
      size_t n;
      std::clearerr(peer_);
      while ((n = std::fread(buf, 1, sizeof buf, peer_)) > 0) {
        peerBuf_.append(buf, n);
        lastNews = std::chrono::steady_clock::now();
      }
      size_t nl;
      while ((nl = peerBuf_.find('\n')) != std::string::npos) {
        std::string l = peerBuf_.substr(0, nl);
        peerBuf_.erase(0, nl + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind("# end", 0) == 0 || l.rfind("# differs", 0) == 0) {
          peerGone_ = true;
          return false;
        }
        if (l.empty() || l[0] == '#') continue;
        const unsigned long b = std::strtoul(l.c_str(), nullptr, 10);
        if (b < beat) continue;
        if (b > beat) {  // (the peer skipped this beat: the two runs are not of one game)
          *line = l;
          return true;
        }
        *line = std::move(l);
        return true;
      }
    }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - lastNews).count() > waitSeconds_) {
      Logf(LogLevel::Warning, "fingerprint: the peer's file %s has said nothing for %.0f s; not compared from beat %u",
           peerPath_.c_str(), waitSeconds_, beat);
      peerGone_ = true;
      return false;
    }
    // (the two copies run at about the same pace: yield a while before sleeping, whose step on Windows is coarse)
    if (++spins < 2000) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void Fingerprint::WriteDetail(const Sim& sim, uint32_t beat, const std::string& path, const char* why) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    Logf(LogLevel::Warning, "fingerprint: cannot write %s", path.c_str());
    return;
  }
  std::fprintf(f, "# beat %u tick %u (%s)\n", beat, sim.tick(), why);
  std::fprintf(f, "rng seed %u draws %llu\n", sim.sessionSeed(), static_cast<unsigned long long>(sim.rngDraws_));
  if (lua_State* L = sim.L())
    std::fprintf(f, "lua kb %d threshold %d serial %u\n", lua_getgccount(L), lua_getgcthreshold(L), lua_getnextserial(L));
  for (const auto& a : sim.armies()) {
    const ArmyEconomy& e = a->econ;
    std::fprintf(f, "army %d stored %s income %s requested %s usage %s max %lld %lld units %zu cap %.9g cost %.9g%s\n",
                 a->index, Fs({e.stored[0], e.stored[1]}).c_str(), Fs({e.incomeStat[0], e.incomeStat[1]}).c_str(),
                 Fs({e.requested[0], e.requested[1]}).c_str(), Fs({e.usage[0], e.usage[1]}).c_str(),
                 static_cast<long long>(e.maxStorage[0]), static_cast<long long>(e.maxStorage[1]), a->units.size(),
                 a->unitCap, a->unitCost, a->outOfGame ? " out" : "");
    for (const auto& [name, v] : a->stats) std::fprintf(f, "armystat %d %s %.9g\n", a->index, name.c_str(), v);
    for (const Platoon* p : a->platoons) {
      std::string ids;
      for (const auto& s : p->squads)
        for (const Unit* u : s->units) ids += " " + std::to_string(s->cls) + ":" + std::to_string(u->id);
      std::fprintf(f, "platoon %d '%s' '%s'%s\n", a->index, p->uniqueName.c_str(), p->plan.c_str(), ids.c_str());
    }
  }
  for (const Unit* u : ById(sim.units())) {
    std::string states;
    for (const std::string& s : u->unitStates) states += (states.empty() ? "" : ",") + s;
    std::string cmds;
    for (const auto& c : u->commands)
      if (c) cmds += CommandText(*c);
    std::string rally;
    for (const auto& c : u->factoryCommands)
      if (c) rally += CommandText(*c);
    std::string weapons;
    for (const UnitWeapon* w : u->weapons) {
      char b[200];
      std::snprintf(b, sizeof b, " [%d%s %s clock %d shots %d next %u]", w->index, w->enabled ? "" : " off",
                    TargetText(w->target).c_str(), w->fireClock, w->shots, w->nextAcquire);
      weapons += b;
    }
    std::fprintf(f,
                 "unit %#x %s a%d p %s q %s hp %.9g/%.9g frac %.9g work %.9g %s states %s v %s focus %u guard %u "
                 "parent %u by %u%s%s%s cmds %zu%s rally %zu%s weapons%s\n",
                 u->id, BpId(u), ArmyIndex(u), Fs({u->position.x, u->position.y, u->position.z}).c_str(),
                 Fs({u->orientation.x, u->orientation.y, u->orientation.z, u->orientation.w}).c_str(), u->health,
                 u->maxHealth, u->fractionComplete, u->workProgress, u->layer.c_str(),
                 states.empty() ? "-" : states.c_str(),
                 Fs({u->motion.vel.x, u->motion.vel.y, u->motion.vel.z}).c_str(), u->focusId, u->guardedId,
                 u->parentId, u->transportedBy, u->dead ? " dead" : "", u->destroyQueued ? " destroying" : "",
                 u->beingBuilt ? " beingbuilt" : "", u->commands.size(), cmds.c_str(), u->factoryCommands.size(),
                 rally.c_str(), weapons.c_str());
  }
  for (const Projectile* p : ById(sim.projectiles))
    std::fprintf(f, "proj %#x %s a%d p %s q %s v %s target %s damage %.9g expire %u %s\n", p->id, BpId(p),
                 ArmyIndex(p), Fs({p->position.x, p->position.y, p->position.z}).c_str(),
                 Fs({p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w}).c_str(),
                 Fs({p->velocity.x, p->velocity.y, p->velocity.z}).c_str(), TargetText(p->target).c_str(), p->damage,
                 p->expireTick, p->layer.c_str());
  for (const Prop* p : ById(sim.props))
    std::fprintf(f, "prop %#x %s p %s q %s hp %.9g frac %.9g%s\n", p->id, BpId(p),
                 Fs({p->position.x, p->position.y, p->position.z}).c_str(),
                 Fs({p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w}).c_str(), p->health,
                 p->fractionComplete, p->destroyQueued ? " destroying" : "");
  std::vector<const Entity*> others;
  for (const auto& [handle, e] : sim.entities())
    if (e->kind != Entity::Kind::Unit && e->kind != Entity::Kind::Prop && e->kind != Entity::Kind::Projectile)
      others.push_back(e);
  std::sort(others.begin(), others.end(), [](const Entity* a, const Entity* b) {
    return a->id != b->id ? a->id < b->id : a->handle < b->handle;
  });
  for (const Entity* e : others)
    std::fprintf(f, "ent %#x kind %d %s a%d p %s q %s hp %.9g parent %u%s\n", e->id, static_cast<int>(e->kind), BpId(e),
                 ArmyIndex(e), Fs({e->position.x, e->position.y, e->position.z}).c_str(),
                 Fs({e->orientation.x, e->orientation.y, e->orientation.z, e->orientation.w}).c_str(), e->health,
                 e->attachParent, e->destroyQueued ? " destroying" : "");
  std::fclose(f);
  Logf(LogLevel::Info, "fingerprint: the state of beat %u is in %s", beat, path.c_str());
}

}  // namespace moho
