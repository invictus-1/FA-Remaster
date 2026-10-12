#include "sim/sim_view.h"

#include "sim/commands.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {

std::vector<ArmyView> ArmyViews(const Sim& sim) {
  std::vector<ArmyView> out;
  for (const auto& a : sim.armies()) {
    ArmyView v;
    v.index = a->index;
    v.name = a->name;
    v.nickname = a->nickname;
    v.faction = a->faction;
    v.human = a->human;
    v.civilian = a->civilian;
    v.outOfGame = a->outOfGame;
    v.showScore = a->showScore;
    v.armyColor = a->armyColor;
    v.playerColor = a->playerColor;
    for (int i = 0; i < 3; ++i) v.rgb[i] = a->rgb[i];
    v.alliance = a->alliance;
    out.push_back(std::move(v));
  }
  return out;
}

EconomyTotals EconomyTotalsOf(const Sim& sim, int army) {
  EconomyTotals t;
  if (army < 1 || army > static_cast<int>(sim.armies().size())) return t;
  const ArmyEconomy& e = sim.armies()[army - 1]->econ;
  for (int i = 0; i < 2; ++i) {
    t.income[i] = e.incomeStat[i];
    t.lastUseRequested[i] = e.requested[i];
    t.lastUseActual[i] = e.usage[i];
    t.stored[i] = e.stored[i];
    t.maxStorage[i] = static_cast<float>(e.maxStorage[i]);
    t.reclaimed[i] = e.reclaimed[i];
  }
  return t;
}

void UnitViews(const Sim& sim, std::vector<UnitView>* out) {
  out->clear();
  out->reserve(sim.units().size());
  for (const Unit* u : sim.units()) {
    if (u->destroyQueued) continue;
    UnitView v;
    v.id = u->id;
    if (u->blueprint) v.blueprint = u->blueprint->id;
    v.army = u->army ? u->army->index : 0;
    v.position[0] = u->position.x;
    v.position[1] = u->position.y;
    v.position[2] = u->position.z;
    v.orientation[0] = u->orientation.x;
    v.orientation[1] = u->orientation.y;
    v.orientation[2] = u->orientation.z;
    v.orientation[3] = u->orientation.w;
    v.health = u->health;
    v.maxHealth = u->maxHealth;
    v.fractionComplete = u->fractionComplete;
    v.workProgress = u->workProgress;
    v.dead = u->dead;
    v.beingBuilt = u->beingBuilt;
    v.selectable = u->unitStates.count("UnSelectable") == 0;
    v.queueLength = static_cast<int>(u->commands.size());
    v.headCommand = u->commands.empty() ? -1 : static_cast<int>(u->commands.front()->type);
    out->push_back(std::move(v));
  }
}

}  // namespace moho
