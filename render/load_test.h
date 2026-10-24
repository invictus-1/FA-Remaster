// [bgload] The viewer's --load-test: one window's picture going from map to map as the interface's will
// (WorldPicture::BeginMap / MapReady / SwapMap), with its frame times, times to ready, the swaps' frames and the card's
// memory printed, and what it showed written as pictures.
#pragma once
#include <string>

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
};

// The test; 0 when every map opened and was shown.
int LoadTest(const moho::Vfs& vfs, const LoadTestSetup& setup);

}  // namespace render
