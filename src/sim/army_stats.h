// The armies' macro statistics (MOHO64_ARMY_STATS=<file>): every N game ticks, per army, its economy, what it has
// built (engineers, extractors, power, factories by kind and tech) and its mobile army's mass value, plus the tick of
// first moments (the first tech 2 factory, extractor, the first experimental, ...). tools/procurve.py makes minute
// curves and a scoreboard of them, for replays and for new games (--session) alike.
//
// It only READS the simulation, after the beat, as C++ values: no Lua, nothing written into the sim, no lookup that
// inserts (std::map read with find; the categories as the blueprints were loaded, SimBlueprints::Categories, not
// BpInCategory's cache; Sim::CheatsEnabled is not called: it notes cheaters). The game plays the same with it as
// without: compare MOHO64_FINGERPRINT of a run with and one without (the lua part too). Off (the default) it costs
// one test of a null pointer per beat. On: one pass over the units every beat (the first moments) and a fuller one
// every N ticks.
//
// MOHO64_ARMY_STATS_EVERY=<ticks> (default 300 = 30 s of game time): how often. The file (JSON lines, one object a
// line, flushed after every sample):
//   {"k":"head", "format":1, "every":N, "map":..., "seed":..., "order":"original"|"parallel",
//    "armies":[{"army":1, "name":"ARMY_1", "faction":0, "civilian":false, "human":true, "ai":"Human",
//               "team":1, "start":[x, z]}, ...]}
//     faction 0-based (0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim, then a mod's); ai: Army::personality ("Human" or the
//     lobby's AI personality); team: the lowest army number of the armies it is allied with both ways (itself
//     included), as the session started. No player names (the file may be shared).
//   {"k":"army", "tick":T, "army":i, ...}  per army every N ticks (and at the end), civilians too:
//     out                    out of the game (Army::outOfGame)
//     mass_income ... _spent the economy's own figures, per second (the engine keeps them per tick: x 10):
//                            income = last beat's production (GetEconomyIncome), spent = what was granted
//                            (GetEconomyUsage), requested (GetEconomyRequested); stored, max (storage)
//     mass_reclaimed         reclaimed since the game began (also energy_)
//     mass_produced ...      the army statistics' totals since the game began: Economy_TotalProduced_*,
//                            Economy_TotalConsumed_*; mass_built Units_MassValue_Built, mass_lost
//                            Units_MassValue_Lost, mass_killed Enemies_MassValue_Destroyed, kills Enemies_Killed
//     units                  finished live units; building: units under construction
//     acu, scu               finished units in COMMAND / SUBCOMMANDER
//     eng [t1,t2,t3,t4]      finished MOBILE ENGINEERs that are neither (by TECH1 / 2 / 3 / EXPERIMENTAL)
//     mex [t1,t2,t3]         finished STRUCTURE MASSEXTRACTION; fab [t1,t2,t3] MASSFABRICATION
//     pgen [t1,t2,t3]        finished STRUCTURE ENERGYPRODUCTION but HYDROCARBON; hydro: those in HYDROCARBON
//     fac {land,air,naval,other: [t1,t2,t3]}  finished STRUCTURE FACTORY by layer; hq: those in RESEARCH (FAF's
//                            headquarters; its support factories are the rest)
//     force {land,air,naval} finished MOBILE units but engineers, commanders and sub-commanders: [count, mass]
//                            (mass: the blueprint's Economy.BuildCostMass); AIR first, then NAVAL, else land
//     exp [count, mass]      finished EXPERIMENTAL units (mobile or not)
//     mobile {bp: count}     the finished mobile units of the army part, per blueprint
//     cmd [asked, missing, refused]  (a replay's or --stream's orders, since the start) the unit ids the stream's
//                            IssueCommand / IssueFactoryCommand named that are of this army (the id's army part):
//                            all, those not in the world, those the issuing source may not command. A game that plays
//                            as the original did misses few (a unit that died while the order was on its way); a
//                            replay that does not fit misses more and more (tools/procurve.py fit).
//   {"k":"event", "tick":T, "army":i, "what":..., "bp":...}  the first finished unit of the army in: t2_factory,
//     t3_factory (STRUCTURE FACTORY TECH2 / 3), t2_mex, t3_mex, t2_pgen, t3_pgen, t2_engineer, t3_engineer, scu,
//     experimental, land_factory, air_factory, naval_factory (of any tech), mex (the first extractor); and once each: "acu_died" (its last COMMAND unit was killed), "out" (out of the game, with
//     "enemies_left": its enemies still in, civilians not counted: 0 = the game ended with it, FAF's victory puts the
//     winners out too).
//   {"k":"end", "tick":T, "beats":B}
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace moho {

class Sim;
class CommandStream;

class ArmyStatsLog {
 public:
  // From the environment (see above), for a sim that has started; nullptr when MOHO64_ARMY_STATS is not set. The
  // file is MOHO64_ARMY_STATS + suffix (moho64 --together: ".2" for its second copy).
  static std::unique_ptr<ArmyStatsLog> FromEnvironment(const Sim& sim, const std::string& suffix = std::string());
  ~ArmyStatsLog();
  // After every beat that ran (Sim::Tick). stream: the orders being played, or null.
  void Beat(const Sim& sim, const CommandStream* stream);
  // The run ended after `beats` beats: a last sample, then the end line.
  void End(const Sim& sim, const CommandStream* stream, uint32_t beats);

 private:
  ArmyStatsLog(const Sim& sim, FILE* out, int every);
  void Sample(const Sim& sim, const CommandStream* stream);
  FILE* out_ = nullptr;
  int every_ = 300;
  uint32_t lastSample_ = 0xffffffffu;
  // per blueprint entity index: the categories it is in (kBit... in army_stats.cpp)
  std::vector<uint32_t> cats_;
  // per army (0-based): the first moments seen (bits), out of the game, commander lost
  std::vector<uint32_t> seen_;
  std::vector<bool> out_of_, acuSeen_, acuDied_;
};

}  // namespace moho
