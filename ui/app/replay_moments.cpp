// The stand-in for the engine's ScanReplayMoments (see replay_moments.h): a fixed list of moments spread over the
// replay's length and a made-up amount of fighting, handed out over about twelve seconds, as a scan would find them.
// Nothing here is the replay's own: the markers it gives are placeholders until the engine's scan is on main.
#include "app/replay_moments.h"

#include <chrono>
#include <cmath>
#include <thread>

namespace moho::ui {

namespace {
struct Planned {
  float at;  // where in the replay, 0..1 of its length
  Kind kind;
  int player;  // which player (by the order of the armies with a command source), modulo their number
  const char* unit;
};
// Every kind at least once, the two halves of a nuke one beat after the other.
const Planned kPlan[] = {
    {0.16f, Kind::ExperimentalFinished, 0, "url0402"},  // (a Monkeylord)
    {0.30f, Kind::NukeLaunched, 1, "ueb2305"},
    {0.31f, Kind::NukeLanded, 1, "ueb2305"},
    {0.43f, Kind::ExperimentalDied, 0, "url0402"},
    {0.52f, Kind::ExperimentalFinished, 1, "uel0401"},  // (a Fatboy)
    {0.64f, Kind::NukeLaunched, 0, "urb2305"},
    {0.65f, Kind::NukeLanded, 0, "urb2305"},
    {0.77f, Kind::ExperimentalDied, 1, "uel0401"},
    {0.95f, Kind::CommanderDied, 1, "uel0001"},
    {0.952f, Kind::ArmyDefeated, 1, ""},
};
}  // namespace

bool ScanReplayMoments(const ScanArgs& args, Moments* out, std::atomic<int>* tick, const std::atomic<bool>* cancel) {
  std::vector<int> players;  // the armies a player commands (0-based)
  for (size_t i = 0; i < args.header.armies.size(); ++i)
    if (!args.header.armies[i].sources.empty()) players.push_back(static_cast<int>(i));
  if (players.empty()) players.push_back(0);
  const int armies = static_cast<int>(args.header.armies.size());
  const int ticks = static_cast<int>(args.ticks);
  {
    std::lock_guard<std::mutex> hold(out->lock);
    out->moments.clear();
    out->fight.assign(static_cast<size_t>(armies > 0 ? armies : 1), {});
  }
  // A made-up amount of fighting: a little all along, more near the moments (a small random walk from the seed).
  uint32_t random = args.header.seed | 1u;
  auto next = [&random]() {
    random = random * 1664525u + 1013904223u;
    return static_cast<float>(random >> 8) / static_cast<float>(1u << 24);
  };
  auto fightAt = [&](int tick0, int army) {
    FightBucket b;
    b.tick0 = tick0;
    float near = 0;
    for (const Planned& p : kPlan) {
      const float d = std::fabs(static_cast<float>(tick0) - p.at * static_cast<float>(ticks)) / 600.0f;  // (a minute)
      near += std::exp(-d * d);
    }
    const float ramp = std::fmin(1.0f, static_cast<float>(tick0) / (0.15f * static_cast<float>(ticks > 0 ? ticks : 1)));
    b.damage = (40.0f * ramp + 900.0f * near) * (0.5f + next()) * (army % 2 == 0 ? 1.0f : 0.8f);
    b.hits = static_cast<int>(b.damage / 25.0f);
    b.deaths = static_cast<int>(b.damage / 400.0f);
    return b;
  };
  const int steps = 120;  // (about twelve seconds: longer than a replay takes to load, so the markers are seen to come)
  size_t planned = 0;
  int bucketsDone = 0;
  for (int s = 1; s <= steps; ++s) {
    if (cancel && cancel->load()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int reached = static_cast<int>(static_cast<int64_t>(ticks) * s / steps);
    std::lock_guard<std::mutex> hold(out->lock);
    while (planned < sizeof kPlan / sizeof kPlan[0] && static_cast<int>(kPlan[planned].at * static_cast<float>(ticks)) <= reached) {
      const Planned& p = kPlan[planned++];
      Moment m;
      m.tick = static_cast<int>(p.at * static_cast<float>(ticks));
      if (m.tick < 1) m.tick = 1;
      m.kind = p.kind;
      m.army = players[static_cast<size_t>(p.player) % players.size()];
      m.pos = Vec3{256.0f + 40.0f * static_cast<float>(planned), 20.0f, 256.0f};
      m.unit = p.unit;
      out->moments.push_back(m);
    }
    for (; (bucketsDone + 1) * 50 <= reached; ++bucketsDone)
      for (int a = 0; a < static_cast<int>(out->fight.size()); ++a) {
        bool player = false;
        for (int p : players) player = player || p == a;
        out->fight[static_cast<size_t>(a)].push_back(player ? fightAt(bucketsDone * 50, a) : FightBucket{bucketsDone * 50, 0, 0, 0});
      }
    if (tick) tick->store(reached);
  }
  return true;
}

}  // namespace moho::ui
