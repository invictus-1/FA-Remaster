// [devicelost] The viewer's --recover-test: one window's picture as the interface's game has it (WorldPicture on a
// window: opened without a map, the map opened behind and shown, a game's beats where a session is given), its graphics
// device lost on purpose at a frame (--lose-device-at) and made again (WorldPicture::RecoverDevice), with what was
// noticed when, how long the recovery took and what it carried over printed, and pictures written at chosen frames, to
// compare with the same run without the loss.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace moho {
class Vfs;
}

namespace render {

struct RecoverTestSetup {
  std::string map;           // --map (with a session: the session's own unless given)
  int width = 1600, height = 900;
  bool vulkan = false, vsync = false, debug = false;
  std::string pictures;      // --screenshot's name: <name>_f<frame>.png for each of `shots`, <name>.png for the last frame
  float timeOfDay = -1, dayLength = 0;  // --time-of-day, --day-length (through SetDayAndWeather)
  bool weatherAuto = false;  // --weather auto
  std::string settings;      // --settings (the lighting bundles, battle-effects, photo-look: as --picture has them)
  // a game: --session <game configuration> with --init, --maps, --mod-uids, --seed as for --session; run to --tick first
  std::string session, initFile, modUids;
  std::vector<std::string> maps;
  uint32_t seed = 0;
  int focus = 0;             // --focus
  double tick = 0;           // --tick: where the counted frames begin (a game is run there first, frames drawn meanwhile)
  int frames = 240;          // --frames: the counted frames (each a third of a beat on: 30 a second of the game)
  std::vector<int> loseAt;   // --lose-device-at <frame>[,<frame>...]: the device lost on purpose after each of those counted frames
  int ignore = 0;            // --lose-device-ignore <n>: that many frames more asked of the lost picture before it is made
                             // again (what a host that does not look gets: nothing drawn, and no error line a frame)
  std::vector<int> shots;    // --recover-shots a,b,...: counted frames written as pictures
  std::string shaderCache;   // --shader-cache <folder> ("-": --no-shader-cache): WorldPicture::SetShaderCache
  bool hasCam = false;       // --cam x,z,distance,pitch,heading (degrees); else over the map's middle
  float cam[5] = {};
};

// The test; 0 when the picture was shown to the end (after a loss: made again), 1 otherwise.
int RecoverTest(const moho::Vfs& vfs, const RecoverTestSetup& setup);

}  // namespace render
