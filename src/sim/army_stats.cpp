// The armies' macro statistics (MOHO64_ARMY_STATS): sim/army_stats.h says what is written and how.
#include "sim/army_stats.h"

#include <cstdlib>
#include <map>
#include <string>

#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/command_stream.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

// The categories looked at, as bits of ArmyStatsLog::cats_
enum : uint32_t {
  kCommand = 1u << 0, kSubCommander = 1u << 1, kEngineer = 1u << 2, kMobile = 1u << 3, kStructure = 1u << 4,
  kMassExtraction = 1u << 5, kMassFabrication = 1u << 6, kEnergyProduction = 1u << 7, kHydrocarbon = 1u << 8,
  kFactory = 1u << 9, kResearch = 1u << 10, kLand = 1u << 11, kAir = 1u << 12, kNaval = 1u << 13,
  kTech1 = 1u << 14, kTech2 = 1u << 15, kTech3 = 1u << 16, kExperimental = 1u << 17,
};
const std::pair<const char*, uint32_t> kCategories[] = {
    {"COMMAND", kCommand}, {"SUBCOMMANDER", kSubCommander}, {"ENGINEER", kEngineer}, {"MOBILE", kMobile},
    {"STRUCTURE", kStructure}, {"MASSEXTRACTION", kMassExtraction}, {"MASSFABRICATION", kMassFabrication},
    {"ENERGYPRODUCTION", kEnergyProduction}, {"HYDROCARBON", kHydrocarbon}, {"FACTORY", kFactory},
    {"RESEARCH", kResearch}, {"LAND", kLand}, {"AIR", kAir}, {"NAVAL", kNaval}, {"TECH1", kTech1},
    {"TECH2", kTech2}, {"TECH3", kTech3}, {"EXPERIMENTAL", kExperimental}};

// The first moments (ArmyStatsLog::seen_ bits), in the order of kMomentNames
enum : uint32_t {
  kT2Factory = 1u << 0, kT3Factory = 1u << 1, kT2Mex = 1u << 2, kT3Mex = 1u << 3, kT2Pgen = 1u << 4,
  kT3Pgen = 1u << 5, kT2Engineer = 1u << 6, kT3Engineer = 1u << 7, kScu = 1u << 8, kExp = 1u << 9,
  kLandFactory = 1u << 10, kAirFactory = 1u << 11, kNavalFactory = 1u << 12, kMex = 1u << 13,
};
const char* const kMomentNames[] = {"t2_factory", "t3_factory", "t2_mex", "t3_mex", "t2_pgen", "t3_pgen",
                                    "t2_engineer", "t3_engineer", "scu", "experimental", "land_factory",
                                    "air_factory", "naval_factory", "mex"};

bool Has(uint32_t c, uint32_t bits) { return (c & bits) == bits; }
bool IsEngineer(uint32_t c) { return Has(c, kMobile | kEngineer) && !(c & (kCommand | kSubCommander)); }
int Tech(uint32_t c) { return c & kExperimental ? 3 : c & kTech3 ? 2 : c & kTech2 ? 1 : 0; }  // 0-based

// The moments a finished unit of these categories is
uint32_t MomentsOf(uint32_t c) {
  uint32_t m = 0;
  const int t = Tech(c);
  if (Has(c, kStructure | kFactory)) {
    m |= t == 1 ? kT2Factory : t == 2 ? kT3Factory : 0;
    m |= c & kLand ? kLandFactory : c & kAir ? kAirFactory : c & kNaval ? kNavalFactory : 0;
  }
  if (Has(c, kStructure | kMassExtraction)) m |= kMex | (t == 1 ? kT2Mex : t == 2 ? kT3Mex : 0);
  if (Has(c, kStructure | kEnergyProduction) && !(c & kHydrocarbon)) m |= t == 1 ? kT2Pgen : t == 2 ? kT3Pgen : 0;
  if (IsEngineer(c)) m |= t == 1 ? kT2Engineer : t == 2 ? kT3Engineer : 0;
  if (c & kSubCommander) m |= kScu;
  if (c & kExperimental) m |= kExp;
  return m;
}

// A string as JSON (the ids and names here are plain, but a personality may be anything)
std::string Json(const std::string& s) {
  std::string o = "\"";
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\', o += static_cast<char>(ch);
    else if (ch < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", ch);
      o += b;
    } else {
      o += static_cast<char>(ch);
    }
  }
  return o + "\"";
}

float Stat(const Army& a, const char* name) {  // (find: reading must not insert)
  auto it = a.stats.find(name);
  return it == a.stats.end() ? 0.0f : it->second;
}

// Enemies of army i still in the game (civilians not counted)
int EnemiesLeft(const Sim& sim, size_t i) {
  const Army& a = *sim.armies()[i];
  int n = 0;
  for (size_t j = 0; j < sim.armies().size(); ++j) {
    const Army& b = *sim.armies()[j];
    if (j != i && !b.civilian && !b.outOfGame && j < a.alliance.size() && a.alliance[j] == 0) ++n;
  }
  return n;
}

}  // namespace

std::unique_ptr<ArmyStatsLog> ArmyStatsLog::FromEnvironment(const Sim& sim, const std::string& suffix) {
  const char* path = std::getenv("MOHO64_ARMY_STATS");
  if (!path || !*path) return nullptr;
  const std::string file = path + suffix;
  FILE* f = std::fopen(file.c_str(), "w");
  if (!f) {
    Logf(LogLevel::Error, "army stats: cannot write %s", file.c_str());
    return nullptr;
  }
  const char* e = std::getenv("MOHO64_ARMY_STATS_EVERY");
  const int every = e && std::atoi(e) > 0 ? std::atoi(e) : 300;
  Logf(LogLevel::Info, "army stats: every %d ticks to %s", every, file.c_str());
  return std::unique_ptr<ArmyStatsLog>(new ArmyStatsLog(sim, f, every));
}

ArmyStatsLog::ArmyStatsLog(const Sim& sim, FILE* out, int every) : out_(out), every_(every) {
  const int n = sim.blueprints().EntityCount();
  cats_.assign(static_cast<size_t>(n > 0 ? n : 0), 0);
  for (const auto& [name, bit] : kCategories) {
    auto it = sim.blueprints().Categories().find(name);
    if (it == sim.blueprints().Categories().end()) continue;
    for (int i : it->second)
      if (i >= 0 && i < n) cats_[static_cast<size_t>(i)] |= bit;
  }
  const size_t armies = sim.armies().size();
  seen_.assign(armies, 0);
  out_of_.assign(armies, false);
  acuSeen_.assign(armies, false);
  acuDied_.assign(armies, false);
  std::string head = "{\"k\":\"head\",\"format\":1,\"every\":" + std::to_string(every_) + ",\"map\":" +
                     Json(sim.mapFile) + ",\"seed\":" + std::to_string(sim.sessionSeed()) + ",\"order\":\"" +
                     (sim.ParallelOrder() ? "parallel" : "original") + "\",\"armies\":[";
  for (size_t i = 0; i < armies; ++i) {
    const Army& a = *sim.armies()[i];
    out_of_[i] = a.outOfGame;  // (out from the start: simInit's merged team armies; no "out" moment for them)
    size_t team = i;  // the lowest index allied both ways
    for (size_t j = 0; j < i; ++j) {
      const Army& b = *sim.armies()[j];
      if (j < a.alliance.size() && a.alliance[j] == 2 && i < b.alliance.size() && b.alliance[i] == 2) {
        team = j;
        break;
      }
    }
    char b[96];
    std::snprintf(b, sizeof b, "\"team\":%zu,\"start\":[%.1f,%.1f],\"out\":%s}", team + 1, a.startX, a.startZ,
                  a.outOfGame ? "true" : "false");
    head += std::string(i ? "," : "") + "{\"army\":" + std::to_string(a.index) + ",\"name\":" + Json(a.name) +
            ",\"faction\":" + std::to_string(a.faction) + ",\"civilian\":" + (a.civilian ? "true" : "false") +
            ",\"human\":" + (a.human ? "true" : "false") + ",\"ai\":" + Json(a.personality) + "," + b;
  }
  head += "]}\n";
  std::fputs(head.c_str(), out_);
  std::fflush(out_);
}

ArmyStatsLog::~ArmyStatsLog() {
  if (out_) std::fclose(out_);
}

void ArmyStatsLog::Beat(const Sim& sim, const CommandStream* stream) {
  // the first moments: every finished unit, its army's bits not seen yet
  const size_t armies = sim.armies().size();
  std::vector<bool> acuNow(armies, false);
  for (const Unit* u : sim.units()) {
    if (u->dead || u->beingBuilt || !u->army || !u->blueprint) continue;
    const int bi = u->blueprint->entityIndex;
    if (bi < 0 || static_cast<size_t>(bi) >= cats_.size()) continue;
    const size_t ai = static_cast<size_t>(u->army->index - 1);
    if (ai >= armies) continue;
    const uint32_t c = cats_[static_cast<size_t>(bi)];
    if (c & kCommand) acuNow[ai] = true;
    const uint32_t fresh = MomentsOf(c) & ~seen_[ai];
    if (!fresh) continue;
    seen_[ai] |= fresh;
    for (size_t k = 0; k < sizeof kMomentNames / sizeof kMomentNames[0]; ++k)
      if (fresh & (1u << k))
        std::fprintf(out_, "{\"k\":\"event\",\"tick\":%u,\"army\":%d,\"what\":\"%s\",\"bp\":%s}\n", sim.tick(),
                     u->army->index, kMomentNames[k], Json(u->blueprint->id).c_str());
  }
  for (size_t i = 0; i < armies; ++i) {
    const Army& a = *sim.armies()[i];
    if (acuNow[i]) acuSeen_[i] = true;
    else if (acuSeen_[i] && !acuDied_[i]) {
      acuDied_[i] = true;
      std::fprintf(out_, "{\"k\":\"event\",\"tick\":%u,\"army\":%d,\"what\":\"acu_died\"}\n", sim.tick(), a.index);
    }
    if (a.outOfGame && !out_of_[i]) {
      out_of_[i] = true;
      std::fprintf(out_, "{\"k\":\"event\",\"tick\":%u,\"army\":%d,\"what\":\"out\",\"enemies_left\":%d}\n", sim.tick(),
                   a.index, EnemiesLeft(sim, i));
    }
  }
  if (sim.tick() % static_cast<uint32_t>(every_) == 0) Sample(sim, stream);
}

void ArmyStatsLog::Sample(const Sim& sim, const CommandStream* stream) {
  if (lastSample_ == sim.tick()) return;
  lastSample_ = sim.tick();
  const size_t armies = sim.armies().size();
  struct Counts {
    int units = 0, building = 0, acu = 0, scu = 0, eng[4] = {}, mex[3] = {}, fab[3] = {}, pgen[3] = {}, hydro = 0;
    int fac[4][3] = {}, hq[4][3] = {};  // land, air, naval, other
    int armyN[3] = {};
    double armyMass[3] = {};
    int exp = 0;
    double expMass = 0;
    std::map<std::string, int> mobile;
  };
  std::vector<Counts> n(armies);
  for (const Unit* u : sim.units()) {
    if (u->dead || !u->army || !u->blueprint) continue;
    const size_t ai = static_cast<size_t>(u->army->index - 1);
    if (ai >= armies) continue;
    Counts& k = n[ai];
    if (u->beingBuilt) {
      ++k.building;
      continue;
    }
    ++k.units;
    const int bi = u->blueprint->entityIndex;
    const uint32_t c = bi >= 0 && static_cast<size_t>(bi) < cats_.size() ? cats_[static_cast<size_t>(bi)] : 0;
    const int t = Tech(c);
    const float mass = u->bpData ? u->bpData->buildCostMass : 0.0f;
    if (c & kExperimental) ++k.exp, k.expMass += mass;
    if (c & kCommand) {
      ++k.acu;
      continue;
    }
    if (c & kSubCommander) {
      ++k.scu;
      continue;
    }
    if (IsEngineer(c)) {
      ++k.eng[t];
      continue;
    }
    if (c & kStructure) {
      if (c & kMassExtraction) ++k.mex[t < 3 ? t : 2];
      if (c & kMassFabrication) ++k.fab[t < 3 ? t : 2];
      if (c & kEnergyProduction) {
        if (c & kHydrocarbon) ++k.hydro;
        else ++k.pgen[t < 3 ? t : 2];
      }
      if (c & kFactory) {
        const int layer = c & kLand ? 0 : c & kAir ? 1 : c & kNaval ? 2 : 3;
        ++k.fac[layer][t < 3 ? t : 2];
        if (c & kResearch) ++k.hq[layer][t < 3 ? t : 2];
      }
      continue;
    }
    if (c & kMobile) {
      const int layer = c & kAir ? 1 : c & kNaval ? 2 : 0;
      ++k.armyN[layer];
      k.armyMass[layer] += mass;
      ++k.mobile[u->blueprint->id];
    }
  }
  for (size_t i = 0; i < armies; ++i) {
    const Army& a = *sim.armies()[i];
    const ArmyEconomy& e = a.econ;
    const Counts& k = n[i];
    std::string s;
    char b[1024];
    std::snprintf(b, sizeof b,
                  "{\"k\":\"army\",\"tick\":%u,\"army\":%d,\"out\":%s,"
                  "\"mass_income\":%.3f,\"energy_income\":%.3f,\"mass_spent\":%.3f,\"energy_spent\":%.3f,"
                  "\"mass_requested\":%.3f,\"energy_requested\":%.3f,\"mass_stored\":%.1f,\"energy_stored\":%.1f,"
                  "\"mass_max\":%lld,\"energy_max\":%lld,\"mass_reclaimed\":%.1f,\"energy_reclaimed\":%.1f,"
                  "\"mass_produced\":%.1f,\"energy_produced\":%.1f,\"mass_consumed\":%.1f,\"energy_consumed\":%.1f,"
                  "\"mass_built\":%.1f,\"mass_lost\":%.1f,\"mass_killed\":%.1f,\"kills\":%.0f,"
                  "\"units\":%d,\"building\":%d,\"acu\":%d,\"scu\":%d,\"eng\":[%d,%d,%d,%d],\"mex\":[%d,%d,%d],"
                  "\"fab\":[%d,%d,%d],\"pgen\":[%d,%d,%d],\"hydro\":%d,",
                  sim.tick(), a.index, a.outOfGame ? "true" : "false", e.incomeStat[kMass] * 10.0, e.incomeStat[kEnergy] * 10.0,
                  e.usage[kMass] * 10.0, e.usage[kEnergy] * 10.0, e.requested[kMass] * 10.0, e.requested[kEnergy] * 10.0,
                  e.stored[kMass], e.stored[kEnergy], static_cast<long long>(e.maxStorage[kMass]),
                  static_cast<long long>(e.maxStorage[kEnergy]), e.reclaimed[kMass], e.reclaimed[kEnergy],
                  Stat(a, "Economy_TotalProduced_Mass"), Stat(a, "Economy_TotalProduced_Energy"),
                  Stat(a, "Economy_TotalConsumed_Mass"), Stat(a, "Economy_TotalConsumed_Energy"),
                  Stat(a, "Units_MassValue_Built"), Stat(a, "Units_MassValue_Lost"),
                  Stat(a, "Enemies_MassValue_Destroyed"), Stat(a, "Enemies_Killed"), k.units, k.building, k.acu, k.scu,
                  k.eng[0], k.eng[1], k.eng[2], k.eng[3], k.mex[0], k.mex[1], k.mex[2], k.fab[0], k.fab[1], k.fab[2],
                  k.pgen[0], k.pgen[1], k.pgen[2], k.hydro);
    s += b;
    static const char* const kLayers[4] = {"land", "air", "naval", "other"};
    for (const char* part : {"fac", "hq"}) {
      const int(*f)[3] = part[0] == 'f' ? k.fac : k.hq;
      s += std::string("\"") + part + "\":{";
      for (int l = 0; l < 4; ++l) {
        std::snprintf(b, sizeof b, "%s\"%s\":[%d,%d,%d]", l ? "," : "", kLayers[l], f[l][0], f[l][1], f[l][2]);
        s += b;
      }
      s += "},";
    }
    std::snprintf(b, sizeof b, "\"force\":{\"land\":[%d,%.0f],\"air\":[%d,%.0f],\"naval\":[%d,%.0f]},\"exp\":[%d,%.0f],",
                  k.armyN[0], k.armyMass[0], k.armyN[1], k.armyMass[1], k.armyN[2], k.armyMass[2], k.exp, k.expMass);
    s += b;
    s += "\"mobile\":{";
    bool first = true;
    for (const auto& [bp, count] : k.mobile) {
      s += (first ? "" : ",") + Json(bp) + ":" + std::to_string(count);
      first = false;
    }
    s += "}";
    if (stream) {
      CommandStream::IdCount c;
      if (i < stream->idCounts().size()) c = stream->idCounts()[i];
      std::snprintf(b, sizeof b, ",\"cmd\":[%u,%u,%u]", c.asked, c.missing, c.refused);
      s += b;
    }
    s += "}\n";
    std::fputs(s.c_str(), out_);
  }
  std::fflush(out_);
}

void ArmyStatsLog::End(const Sim& sim, const CommandStream* stream, uint32_t beats) {
  Sample(sim, stream);
  std::fprintf(out_, "{\"k\":\"end\",\"tick\":%u,\"beats\":%u}\n", sim.tick(), beats);
  std::fflush(out_);
}

}  // namespace moho
