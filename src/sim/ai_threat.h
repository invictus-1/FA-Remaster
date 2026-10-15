// The armies' threat maps (CInfluenceMap, sim/ai_threat.cpp): what each army knows of the threat other armies
// pose, per cell of a coarse grid over the map, for the AI brains' threat queries (CAiBrain:GetThreatAtPosition,
// GetThreatBetweenPositions, GetHighestThreatPosition, GetThreatsAroundPosition, AssignThreatAtPosition).
//
// What the original does (FA exe, read 2026-10-15):
// - One map per army (CArmyImpl ctor 0x6feb94 -> CInfluenceMap ctor 0x716140, army+0x218; the army's recon DB holds
//   it too, db+0x3c). Cells of max(32, max(mapW, mapH) / 16) world units (16 x 16 cells on a 20 km map); each cell
//   holds the blips the army has seen there, a threat sum per army, and a threat the scripts assigned.
// - The recon tick keeps it current: a new blip goes in (GenerateNewBlips 0x5c0bd0 -> InsertEntry 0x715d10), every
//   updated blip is refreshed or moved to its new cell (UpdateBlip 0x5c1f53 -> UpdateBlipPosition 0x715f30), a
//   structure confirmed gone comes out (ReconTick 0x5c0da6, 0x5c0f91 -> RemoveEntry 0x715eb0). A blip the army lost
//   stays in the map where it was last seen: full weight for 10 updates, then mobile units fade out by 0.02 per
//   update, structures never.
// - Every 30 beats in the army's own slot (CArmyImpl::OnTick 0x6ffe11: tick % 30 == army index) the map sums its
//   entries' blueprint threat levels (Defense.Air/Surface/Sub/EconomyThreatLevel) into the threat classes
//   (CInfluenceMap::Update 0x716480). The update draws no random numbers and changes nothing outside the map
//   (it also feeds the sim's desync checksum, which we do not keep).
#pragma once
#include <cstdint>

#include "script/lua.hpp"

namespace moho {

class Sim;
class ReconBlip;

// The armies' maps, made with the armies (CArmyImpl ctor): call after the armies exist and the map is loaded.
void InitThreatMaps(Sim& sim);

// The recon tick of army index a (0-based) keeps a's map current (sim/intel.cpp):
// a new blip of a's (GenerateNewBlips 0x5c0bd0, after the blip's first UpdateBlip and before OnDetectedBy),
void ThreatInsertEntry(Sim& sim, int a, const ReconBlip* blip);
// every blip a's recon tick updates (UpdateBlip 0x5c1f53, before its CheckIntelEvents),
void ThreatUpdateBlipPosition(Sim& sim, int a, const ReconBlip* blip);
// a structure's blip whose death a confirmed (ReconTick 0x5c0da6 / 0x5c0f91).
void ThreatRemoveEntry(Sim& sim, int a, uint32_t blipId);

// CArmyImpl::OnTick 0x6ffe11: the army whose slot this beat is (tick % 30 == index) updates its map.
void ThreatMapBeat(Sim& sim);

// For the engine's other users of the map (CAiBrain:FindPlaceToBuild 0x58a4c0 calls it twice):
// CInfluenceMap::GetThreatRect 0x715ff0 of army index a's map: the threat of `type` (EThreatType, below) that
// army index `threatArmy` (-1: all armies) poses in the cells [cx - rings, cx + rings] x [cz - rings, cz + rings],
// only those inside the playable area when playableOnly. 0 if a has no map.
float ThreatRect(const Sim& sim, int a, int cx, int cz, int rings, bool playableOnly, int type, int threatArmy);
// CInfluenceMap::VectorToCoords 0x715c60: the cell index (cz * W + cx) of a world point in army index a's map.
int ThreatCellOf(const Sim& sim, int a, float x, float z);

// EThreatType (EThreatTypeTypeInfo::AddEnums 0x715590): Overall 0, OverallNotAssigned 1, Structures 2,
// StructuresNotMex 3, Naval 4, Land 5, Air 6, Experimental 7, Commander 8, Artillery 9, AntiAir 10, AntiSurface 11,
// AntiSub 12, Economy 13, Unknown 14. (There is no 'Intel' type.)
// A script's threat type string as the engine reads it (REnumType::SetLexical 0x8d9670); raises the original's
// Lua error for an unknown name.
int CheckThreatType(lua_State* L, int idx);

// The five threat functions of CAiBrain, and CAiBrain:CheckBlockingTerrain (0x5910a0; it reads only the terrain,
// not the threat map, and sits here with the brain's other map queries).
void RegisterThreatBindings(lua_State* L);

}  // namespace moho
