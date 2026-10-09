// Intel and recon: each army's intel grids (vision, water vision, radar, sonar, omni and the
// counter-intel grids) painted by its units' intel circles, and the recon blips each army holds of
// enemy units (sim/intel.cpp).
//
// What the original does (FA exe, read 2026-10-08; spec engine-ref/specs/intel.md):
// - Grids of signed byte counts per army: vision 2-unit cells (only with fog of war), water
//   vision and the rest 4-unit cells. A source paints a rasterised circle (CIntelGrid::Raster
//   0x507540: radius in whole cells, one cell short in +x/+z) into its own army's grid; stealth
//   and cloak fields paint into every other army's counter grids. Circles move when the source
//   has moved a third of its radius or 6 ticks have passed (CIntelPosHandle::UpdatePos 0x76f1e0),
//   exactly when it stops, warps or is attached.
// - Recon is round-robin: in beat t only army t % N (N = all armies, civilians included) runs its
//   ReconTick (0x5c0c40): every unit of every other army is looked up in its own and its allies'
//   grids at the unit's position (layer rules, counter-intel, omni beats all); blips are created,
//   updated (LOSEver and KnownFake are sticky) or deleted, with OnDetectedBy / OnIntelChange.
//   So a new unit is noticed 0..N-1 ticks after it appears; weapons only see units with a blip.
// - One ReconBlip entity per unit, shared by the armies that see it, flags per army.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "script/lua.hpp"
#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;

// EIntel: 1 Vision, 2 WaterVision, 3 Radar, 4 Sonar, 5 Omni, 6 RadarStealthField,
// 7 SonarStealthField, 8 CloakField, 9 Jammer, 10 Spoof, 11 Cloak, 12 RadarStealth, 13 SonarStealth.
struct IntelHandle {
  bool exists = false, enabled = false;
  uint32_t radius = 0;
  int army = -1;  // 0-based army whose grids it paints (counter fields: every other army)
  Vec3 pos;       // where it was last rastered
  uint32_t lastTick = 0;
};
struct EntityIntel {
  IntelHandle h[9];          // slots 1..8
  bool has[14] = {}, on[14] = {};  // flags 9..13
};

// Recon flags per army on a unit (Unit::recon): Radar 1, Sonar 2, Omni 4, LOSNow 8, LOSEver 0x10,
// KnownFake 0x20, MaybeDead 0x40; 0x80 = the army holds a blip of the unit.
enum : uint8_t { kReconInUse = 0x80 };

void InitIntelGrids(Sim& sim, lua_State* L);
// A unit was created: its intel handles from blueprint Intel (all disabled until EnableIntel).
void CreateUnitIntel(Sim& sim, lua_State* L, Unit* u);
// An entity leaves: its circles are removed, its blips lose their source.
void ReleaseEntityIntel(Sim& sim, Entity* e);
// End of a beat: army (tick % N) runs its recon tick; moved sources move their circles.
void ReconBeat(Sim& sim);
void AdvanceIntelCoords(Sim& sim);
// Whether army index a holds a blip of the unit.
bool ArmyHasBlip(const Unit* u, int a);
void RegisterIntelBindings(lua_State* L);

}  // namespace moho
