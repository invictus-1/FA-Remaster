// Entity motors: MotorFallDown 0x695180 and the sink-away motor (engine-ref prop_falldown.md).
#include "sim/prop_motor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "sim/entity_grid.h"
#include "sim/sim.h"
#include "sim/terrain.h"

namespace moho {

namespace {

struct Motor : ScriptObject {
  bool sink = false;
  float heading = 0, theta = 0, omega = 0;
  bool broken = false;
  float vy = 0;
  Entity* owner = nullptr;
  // UpdateFall at a fixed point: the last call changed neither theta, omega nor heading and returned before the
  // transform (the turn left to make was within 1e-4). It is a function of these and the entity's orientation
  // alone, so with all of them bit for bit the same the call would do the same nothing again.
  bool settled = false;
  uint32_t settledBits[4] = {};  // theta, omega, heading, broken
  Quat settledOri;
};

struct MotorSlot {
  uint32_t ref;  // EntityRef (the entity may be deleted)
  Motor* m;
};
}  // namespace

// The sim's prop motors (Sim::motorState). Multicore step 1: these were process-wide (g_motors, g_moved).
struct MotorState {
  std::vector<MotorSlot> motors;  // entities with a motor, in the order they got one
  // MotorsTick's erase pass drops the motors whose entity is gone or destroy-queued: it can find one only after a
  // motor was added or another entity was queued for destruction (Sim::DestroysQueued) since it last ran.
  bool added = false;
  uint64_t destroysSeen = ~uint64_t(0);
  std::vector<Entity*> moved;     // moved this beat (for the grid at step 13)
};

namespace {

MotorState& MS(Sim& sim) {
  if (!sim.motorState) sim.motorState = std::make_shared<MotorState>();
  return *sim.motorState;
}

Sim* S(lua_State* L) { return Sim::From(L); }

float NormAngle(float x) {  // 0x62fb50
  float r = static_cast<float>(std::fmod(static_cast<double>(x), static_cast<double>(6.28318548f)));
  if (r < -3.14159274f) r += 6.28318548f;
  else if (r > 3.14159274f) r -= 6.28318548f;
  return r;
}

void SetMotor(Sim& sim, Entity* e, Motor* m) {
  std::vector<MotorSlot>& motors = MS(sim).motors;
  for (auto& s : motors)
    if (s.ref == EntityRef(e)) {
      if (s.m) s.m->UnbindLua();  // the old motor is deleted
      s.m = m;
      return;
    }
  motors.push_back({EntityRef(e), m});
  MS(sim).added = true;
}

Motor* NewMotor(lua_State* L, Entity* e, bool sink) {
  auto o = std::make_unique<Motor>();
  Motor* m = o.get();
  m->sink = sink;
  m->owner = e;
  CreateObject(L, m, "MotorFallDown");  // pushed
  S(L)->Own(std::move(o));
  return m;
}

// Entity:FallDown() 0x695600
int l_FallDown(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "Entity:FallDown(dx,dy,dz,force) -- start falling down", 1,
                      lua_gettop(L));
  Entity* e = CheckObject<Entity>(L, 1);
  Motor* m = NewMotor(L, e, false);
  SetMotor(*S(L), e, m);
  return 1;
}

float StrictNum(lua_State* L, int i) {
  if (lua_type(L, i) != LUA_TNUMBER) luaL_typerror(L, i, "number");
  return static_cast<float>(lua_tonumber(L, i));
}

// MotorFallDown:Whack(nx, ny, nz, f, dobreak) 0x6957a0
int l_Whack(lua_State* L) {
  if (lua_gettop(L) != 6)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "MotorFallDown:Whack(nx,ny,nz,f,dobreak)", 6, lua_gettop(L));
  Motor* m = CheckObject<Motor>(L, 1);
  float nx = StrictNum(L, 2);
  StrictNum(L, 3);
  float nz = StrictNum(L, 4), f = StrictNum(L, 5);
  bool b = lua_toboolean(L, 6) != 0;
  if (!m->broken) {
    m->heading = static_cast<float>(std::atan2(static_cast<double>(nx), static_cast<double>(nz)));
    m->broken = b;
  }
  m->omega += f;
  lua_settop(L, 1);
  return 1;
}

// Entity:SinkAway(vy) 0x696a60
int l_SinkAway(lua_State* L) {
  if (lua_gettop(L) != 2)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "Entity:SinkAway(vy) -- sink into the ground", 2, lua_gettop(L));
  Entity* e = CheckObject<Entity>(L, 1);
  float vy = StrictNum(L, 2);
  Motor* m = NewMotor(L, e, true);
  m->vy = vy;
  SetMotor(*S(L), e, m);
  return 1;
}

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}
bool SameOri(const Quat& a, const Quat& b) {
  return FloatBits(a.x) == FloatBits(b.x) && FloatBits(a.y) == FloatBits(b.y) && FloatBits(a.z) == FloatBits(b.z) &&
         FloatBits(a.w) == FloatBits(b.w);
}

void UpdateFall(Sim& sim, Entity* e, Motor& m) {
  const uint32_t in[4] = {FloatBits(m.theta), FloatBits(m.omega), FloatBits(m.heading), m.broken ? 1u : 0u};
  if (m.settled && in[0] == m.settledBits[0] && in[1] == m.settledBits[1] && in[2] == m.settledBits[2] &&
      in[3] == m.settledBits[3] && SameOri(e->orientation, m.settledOri))
    return;
  m.settled = false;
  if (m.broken) {
    m.omega = m.omega + 0.1f * m.theta;
    m.theta = m.theta + m.omega;
  } else {
    m.omega = m.omega - 0.5f * m.theta;
    m.theta = m.theta + m.omega;
    m.omega = 0.5f * m.omega;
  }
  if (m.theta < 0) {
    m.theta = -m.theta;
    m.omega = -m.omega;
    m.heading = NormAngle(m.heading + 3.14159274f);
  }
  if (m.theta > 1.57079637f) {
    m.theta = 1.57079637f;
    m.omega = 0;
  }
  float a = m.theta - 1.57079637f;
  double sh = std::sin(static_cast<double>(m.heading)), ch = std::cos(static_cast<double>(m.heading));
  double sa = std::sin(static_cast<double>(a)), ca = std::cos(static_cast<double>(a));
  float vx = static_cast<float>(sh * ca), vyv = static_cast<float>(-sa), vz = static_cast<float>(ch * ca);
  const Quat& q = e->orientation;
  float ux = 2 * (q.y * q.x - q.z * q.w), uy = 1 - 2 * (q.z * q.z + q.x * q.x), uz = 2 * (q.z * q.y + q.x * q.w);
  float mx = ux + vx, my = uy + vyv, mz = uz + vz;
  float len = std::sqrt(mx * mx + my * my + mz * mz);
  if (len > 1e-6f) {
    float inv = 1.0f / len;
    mx *= inv;
    my *= inv;
    mz *= inv;
  } else {  // 0x452af0 (0x50cb9f): a length of 1e-6 or less gives the zero vector
    mx = my = mz = 0.0f;
  }
  Quat r;
  r.w = uz * mz + ux * mx + uy * my;
  if (r.w != 0) {
    r.x = uy * mz - uz * my;
    r.y = uz * mx - ux * mz;
    r.z = ux * my - uy * mx;
  } else if (std::fabs(ux) >= std::fabs(uy)) {
    float k = 1.0f / std::sqrt(ux * ux + uz * uz);
    r = Quat{-k * uz, 0, k * ux, 0};
  } else {
    float k = 1.0f / std::sqrt(uy * uy + uz * uz);
    r = Quat{0, k * uz, -k * uy, 0};
  }
  float dw = std::fabs(r.w) - 1.0f;
  if (std::fabs(dw) <= 9.99999975e-05f) {
    if (FloatBits(m.theta) == in[0] && FloatBits(m.omega) == in[1] && FloatBits(m.heading) == in[2]) {
      m.settled = true;
      for (int k = 0; k < 4; ++k) m.settledBits[k] = in[k];
      m.settledOri = e->orientation;
    }
    return;
  }
  Vec3 p = e->position;
  if (m.broken && m.theta > 0.785398185f && sim.map()) {
    float yT = sim.map()->TerrainHeight(p.x, p.z);
    float sizeX = 1;
    if (e->blueprint) {
      lua_State* L = sim.L();
      int top = lua_gettop(L);
      sim.blueprints().PushTable(L, *e->blueprint);
      lua_pushstring(L, "SizeX");
      lua_gettable(L, -2);
      if (lua_isnumber(L, -1)) sizeX = static_cast<float>(lua_tonumber(L, -1));
      lua_settop(L, top);
    }
    float target = yT + sizeX * 0.1f;
    p.y = static_cast<float>(static_cast<double>(p.y) + (static_cast<double>(target) - p.y) *
                                                            (static_cast<double>(m.theta) - 0.785398185f) * 1.27323949);
  }
  // n = r * q in the exe's grouping (0x69548e..0x69553c), then 0x4edaa0 (0x69555c): ((w*w + x*x) + y*y) + z*z, above
  // 1e-6 times 1 / len, else all zero. (Ours grouped x, y and z left to right from r.w, and kept n unnormalised
  // at or below 1e-6.)
  Quat n;
  n.w = ((q.w * r.w - r.x * q.x) - r.y * q.y) - r.z * q.z;
  n.x = ((r.y * q.z + r.x * q.w) + q.x * r.w) - r.z * q.y;
  n.y = ((r.z * q.x + r.y * q.w) + q.y * r.w) - r.x * q.z;
  n.z = ((r.x * q.y + r.z * q.w) + q.z * r.w) - r.y * q.x;
  const float ql = std::sqrt(((n.w * n.w + n.x * n.x) + n.y * n.y) + n.z * n.z);
  if (ql > 9.99999997e-07f) {
    const float inv = 1.0f / ql;
    n = Quat{n.x * inv, n.y * inv, n.z * inv, n.w * inv};
  } else {
    n = Quat{0.0f, 0.0f, 0.0f, 0.0f};
  }
  e->orientation = n;
  e->position = p;
  MS(sim).moved.push_back(e);
}

}  // namespace

void MotorsEntityPurged(Sim& sim, const Entity* e) {
  if (!sim.motorState) return;
  std::vector<MotorSlot>& motors = MS(sim).motors;
  for (size_t i = 0; i < motors.size(); ++i)
    if (motors[i].ref == EntityRef(e)) {
      if (motors[i].m) motors[i].m->UnbindLua();
      motors.erase(motors.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
}

void MotorsTick(Sim& sim) {
  MotorState& ms = MS(sim);
  if (ms.added || ms.destroysSeen != sim.DestroysQueued()) {
    ms.added = false;
    ms.destroysSeen = sim.DestroysQueued();
    ms.motors.erase(std::remove_if(ms.motors.begin(), ms.motors.end(),
                                  [&](const MotorSlot& s) {
                                    Entity* e = sim.FindEntity(s.ref);
                                    return !e || e->destroyQueued;
                                  }),
                    ms.motors.end());
  }
  for (auto& s : ms.motors) {
    Entity* e = sim.FindEntity(s.ref);
    if (!s.m || e->attachParent) continue;
    if (e->kind == Entity::Kind::Unit || e->kind == Entity::Kind::Projectile || e->kind == Entity::Kind::Beam) continue;
    if (s.m->sink) {
      e->position.y += s.m->vy * 0.1f;
      ms.moved.push_back(e);
    } else {
      UpdateFall(sim, e, *s.m);
    }
  }
}

void MotorsAdvanceCoords(Sim& sim) {
  MotorState& ms = MS(sim);
  for (Entity* e : ms.moved)
    if (!e->destroyQueued) GridUpdate(sim, e, false);
  ms.moved.clear();
}

void MotorsForget(Sim& sim) { sim.motorState.reset(); }

void RegisterMotorBindings(lua_State* L) {
  SetMethod(L, "Entity", "FallDown", l_FallDown);
  SetMethod(L, "Entity", "SinkAway", l_SinkAway);
  SetMethod(L, "MotorFallDown", "Whack", l_Whack);
}

}  // namespace moho
