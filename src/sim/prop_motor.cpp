// Entity motors: MotorFallDown 0x695180 and the sink-away motor (engine-ref prop_falldown.md).
#include "sim/prop_motor.h"

#include <algorithm>
#include <cmath>
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
};

struct MotorSlot {
  uint32_t ref;  // EntityRef (the entity may be deleted)
  Motor* m;
};
std::vector<MotorSlot> g_motors;  // entities with a motor, in the order they got one
std::vector<Entity*> g_moved;     // moved this beat (for the grid at step 13)

Sim* S(lua_State* L) { return Sim::From(L); }

float NormAngle(float x) {  // 0x62fb50
  float r = static_cast<float>(std::fmod(static_cast<double>(x), static_cast<double>(6.28318548f)));
  if (r < -3.14159274f) r += 6.28318548f;
  else if (r > 3.14159274f) r -= 6.28318548f;
  return r;
}

void SetMotor(Sim& sim, Entity* e, Motor* m) {
  for (auto& s : g_motors)
    if (s.ref == EntityRef(e)) {
      if (s.m) s.m->UnbindLua();  // the old motor is deleted
      s.m = m;
      return;
    }
  g_motors.push_back({EntityRef(e), m});
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

void UpdateFall(Sim& sim, Entity* e, Motor& m) {
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
  if (std::fabs(dw) <= 9.99999975e-05f) return;
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
  Quat n;
  n.w = r.w * q.w - r.x * q.x - r.y * q.y - r.z * q.z;
  n.x = r.w * q.x + r.x * q.w + r.y * q.z - r.z * q.y;
  n.y = r.w * q.y - r.x * q.z + r.y * q.w + r.z * q.x;
  n.z = r.w * q.z + r.x * q.y - r.y * q.x + r.z * q.w;
  float ql = std::sqrt(n.w * n.w + n.x * n.x + n.y * n.y + n.z * n.z);
  if (ql > 1e-6f) {
    float inv = 1.0f / ql;
    n = Quat{n.x * inv, n.y * inv, n.z * inv, n.w * inv};
  }
  e->orientation = n;
  e->position = p;
  g_moved.push_back(e);
}

}  // namespace

void MotorsTick(Sim& sim) {
  g_motors.erase(std::remove_if(g_motors.begin(), g_motors.end(),
                                [&](const MotorSlot& s) {
                                  Entity* e = sim.FindEntity(s.ref);
                                  return !e || e->destroyQueued;
                                }),
                 g_motors.end());
  for (auto& s : g_motors) {
    Entity* e = sim.FindEntity(s.ref);
    if (!s.m || e->attachParent) continue;
    if (e->kind == Entity::Kind::Unit || e->kind == Entity::Kind::Projectile || e->kind == Entity::Kind::Beam) continue;
    if (s.m->sink) {
      e->position.y += s.m->vy * 0.1f;
      g_moved.push_back(e);
    } else {
      UpdateFall(sim, e, *s.m);
    }
  }
}

void MotorsAdvanceCoords(Sim& sim) {
  for (Entity* e : g_moved)
    if (!e->destroyQueued) GridUpdate(sim, e, false);
  g_moved.clear();
}

void RegisterMotorBindings(lua_State* L) {
  SetMethod(L, "Entity", "FallDown", l_FallDown);
  SetMethod(L, "Entity", "SinkAway", l_SinkAway);
  SetMethod(L, "MotorFallDown", "Whack", l_Whack);
}

}  // namespace moho
