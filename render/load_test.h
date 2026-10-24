// [bgload] The viewer's --load-test: one window's picture going from map to map as the interface's will
// (WorldPicture::BeginMap / MapReady / SwapMap), with its frame times, times to ready, the swaps' frames and the card's
// memory printed, and what it showed written as pictures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace moho {
class Vfs;
}

namespace render {

struct LoadTestSetup {
  std::string mapA, mapB;    // --map, --load-test's
  int width = 2560, height = 1440;
  bool vulkan = false, vsync = false;
  std::string pictures;      // --screenshot's name: <name>_<phase>.png
  float timeOfDay = -1;      // --time-of-day (through SetDayAndWeather)
  bool openFirst = false;    // --load-test-open: the first map by Open itself (the way before), not BeginMap
  float budgetMs = 2;        // --load-budget
  int cycles = 2;            // --load-cycles: how many times B then A again after the first A
  std::string settings;      // --settings (the lighting bundles, as --picture has them)
  double hitchLog = -1;      // --hitch-log: each frame longer than this (ms) printed with what the frames' thread made in it
  int backdrop = -1;         // [backdrop] --load-backdrop <fit>: SetBackdrop's test (0 stretch, 1 cover, 2 contain; -1 none)
  std::string gameLooks;     // [ui 39] --load-game-looks: the lighting bundles of <map> (--settings' form); the others --settings'
  bool holdLooks = false;    // [ui 39] --load-hold: each map's looks given before its BeginMap (HoldLooks), else after its swap
  // [session] --load-session <game configuration>: <map> (the session's) opened behind while that game loads beside it (a
  // LiveSim, as the interface's game loads behind its loading screen), shown once both are ready (its first beat held until
  // then), then run at 10 beats a second; with --load-session-prepare, WorldPicture::PrepareSession first. --init, --maps,
  // --mod-uids and --seed as for --session.
  std::string session, initFile, modUids;
  std::vector<std::string> maps;
  uint32_t seed = 0;
  bool prepareSession = false;
  bool loadDay = false;       // [session] --load-day: with --load-day-after, the day given to its warm frames (SetLoadDayAndWeather)
  bool dayAfterSwap = false;  // [session] --load-day-after: --time-of-day given to <map> only after its swap (the interface's way today)
};

// The test; 0 when every map opened and was shown.
int LoadTest(const moho::Vfs& vfs, const LoadTestSetup& setup);

}  // namespace render
