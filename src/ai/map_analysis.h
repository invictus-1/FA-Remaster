// FA Remastered's own AI, native core, phase 1: the map analysis (docs/ai-architecture.md 3.1).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code: nothing in src/ai/ is taken
// from M28AI or follows its functions (docs/ai-architecture.md 0; the lab's CLAUDE.md, "M28AI's license").
//
// What it makes, once per game, from the terrain, the engine's passability rule (Navigation::MobileCaps, the
// original's OCCUPY_MobileCheck), the game's named footprints and the resource deposits:
// - a grid of segments of 4 x 4 world units; per segment and domain (land, naval, hover, amphibious) whether the
//   domain's 1x1 footprint stands on at least half of its 16 cells;
// - the connected areas of each domain (land: islands, naval: ponds, hover / amphibious: plateaus);
// - zones: land zones grown from groups of mass points, plain land zones where no mass zone reaches, water zones
//   over the ponds; segments that only hover or amphibious units reach join the nearest zone;
// - adjacency between zones with the border's width and midpoint, coast links (land - water);
// - travel distances per domain between neighbours (on the segment grid, through the two zones) and between every
//   pair of zones (over the zone graph); -1 = that domain cannot get there;
// - chokepoints (narrow borders without a short way round) and cut zones (removing one splits its island);
// - build spots: per land segment, the largest square (2, 4, 8, 16 world units) whose cells a land unit stands on,
//   with a height range of at most 1 and no deposit in it (terrain only; structures and props are not counted).
//
// It writes nothing of the simulation's: it reads the map, the navigation's terrain caps and blocking types, the
// footprints, the deposits and the armies' start positions, single-threaded, in fixed orders (no unordered
// container is walked, ties broken by index), so every machine gets the same zones.
//
// When it runs (the lead's choice, 2026-10-28): only in a session with our own AI (an army whose AIPersonality
// starts with "remastered"), at the end of Sim::Start or on the first Lua call; MOHO64_AI_MAPDUMP=<file> forces it
// at the end of Sim::Start for a debug look (and writes <file> and <file>.png). The Remastered.AI* Lua functions
// are made only in a session with our own AI, so every other game's Lua state is exactly as before.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class Sim;

namespace ai {

enum Domain : int { kLand = 0, kNaval = 1, kHover = 2, kAmphibious = 3, kDomains = 4 };
const char* DomainName(int d);  // "land", "naval", "hover", "amphibious"
int DomainFromName(const char* s);  // -1 for none of those

struct ZoneDeposit {
  float x = 0, z = 0;  // the deposit's centre (world)
  int type = 0;        // 1 mass, 2 hydrocarbon
  int zone = 0;        // its zone (1-based), 0 = none
};

struct Zone {
  int id = 0;           // 1-based
  int domain = kLand;   // kLand or kNaval
  int area = 0;         // island (land) or pond (naval) id, 1-based in that domain's numbering
  bool massSeeded = false;
  float x = 0, z = 0;   // centre: the middle of the zone's segment nearest the mean of its segments
  int centreSeg = -1;
  int segments = 0;
  int anchor[kDomains] = {-1, -1, -1, -1};  // per domain: the zone's segment that domain stands on nearest the centre
  std::vector<int> mass, hydro;  // indices into MapAnalysis::deposits
  std::vector<int> starts;       // 1-based army indices whose start position is in the zone
  std::vector<int> links;        // indices into MapAnalysis::links, by neighbour id
  bool cut = false;              // its removal splits its island's zone graph (land zones)
};

// Two neighbouring zones (a < b).
struct ZoneLink {
  int a = 0, b = 0;
  int width = 0;          // 4-neighbour segment pairs on the border (any domain)
  float x = 0, z = 0;     // the border's midpoint
  bool coast = false;     // one land zone and one water zone
  bool choke = false;     // a chokepoint (docs/ai-architecture.md 3.1)
  float dist[kDomains] = {-1, -1, -1, -1};  // travel distance centre to centre through the two zones; -1: not passable
};

struct BuildSpotSeg {
  int seg = 0;      // segment index
  uint8_t size = 0;  // largest square that fits with its corner at the segment's corner: 2, 4, 8 or 16
};

class MapAnalysis {
 public:
  static constexpr int kSeg = 4;  // world units per segment

  int mapW = 0, mapH = 0;  // world units
  int sw = 0, sh = 0;      // segments
  std::vector<uint8_t> pass;      // per segment: bit d = domain d stands there
  std::vector<int> area[kDomains];  // per segment: the domain's connected area, 1-based, 0 = not passable
  std::vector<int> areaSize[kDomains];  // per area id (index 0 unused): segments
  std::vector<int> zoneOf;        // per segment: zone id, 0 = none
  std::vector<Zone> zones;        // zones[id - 1]
  std::vector<ZoneLink> links;
  std::vector<ZoneDeposit> deposits;
  std::vector<std::vector<BuildSpotSeg>> buildSpots;  // per zone (index id - 1), in segment order
  std::vector<float> dist[kDomains];  // zones x zones, row-major; -1 unreachable
  std::string footprintNote;          // which footprints were used (the dump's header)
  double ms = 0;                      // time to make it (the log and the dump only; never handed to Lua)

  int Seg(float x, float z) const;  // segment index at a world position, -1 outside
  int ZoneAt(float x, float z, int domain) const;  // domain -1: any
  float Distance(int a, int b, int domain) const;  // 1-based zones; -1 unreachable or no such zone
  const ZoneLink* Link(int a, int b) const;
};

// Does the session have an army with our own AI (AIPersonality "remastered...")?
bool SessionHasOurAi(const Sim& sim);
// The analysis of the sim's map, made on the first call (logged with its time).
const MapAnalysis& MapOf(Sim& sim);
// The end of Sim::Start: made now in a session with our AI; MOHO64_AI_MAPDUMP forces it and writes the dump.
void AfterSessionStart(Sim& sim);
// Remastered.AI* (docs/ai-architecture.md 4.2): into the Remastered table, when it exists. Only for a session with
// our AI (Sim::CreateArmies calls it then).
void RegisterMapBindings(lua_State* L);
// The dump's text and picture (MOHO64_AI_MAPDUMP).
bool WriteMapDump(const MapAnalysis& m, const std::string& mapName, const std::string& path);

}  // namespace ai
}  // namespace moho
