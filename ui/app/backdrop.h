// The front end's new background: where the original plays a film behind the main menu
// (/movies/main_menu.sfd), this shows the game's own world: the renderer's picture of a map, the
// camera gliding slowly over it in the evening light. It is ours, not the original's (the film is
// not decoded; the new background was chosen to come first).
//
// The picture is made on a thread of its own, with a graphics device of its own, so the menu
// never waits for it: the menu takes the newest finished picture each frame. Reading the map takes
// a few seconds; until the first picture is ready the menu shows the first picture of the run
// before, kept as a file in the system's folder for temporary files (the camera starts at the same
// place every time, so the moving picture takes over from it without a jump); without such a file
// the background is black and the picture comes in from black. While nobody asks for a picture
// (another screen is up) none is made.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "maui/world_source.h"

namespace moho {
class Vfs;
}

namespace moho::ui {

class Backdrop : public BackdropSource {
 public:
  // `map`: the map file in the game's file system ("/maps/SCMP_009/SCMP_009.scmap"). The map is
  // read only after Begin(): its thread builds the renderer's pipelines for render::gSceneSamples,
  // which must not change while a world is alive (render/world.h), so the caller sets it first.
  Backdrop(const Vfs* vfs, std::string map);
  ~Backdrop() override;
  // `vulkan`: drawn with Vulkan in place of Direct3D 12 (the player's Graphics API).
  void Begin(bool vulkan) {
    vulkan_ = vulkan;
    begun_ = true;
  }
  const uint32_t* Latest(int width, int height) override;
  // End it and give the graphics device back (a game is starting: it has its own picture; or the
  // scene's samples change). Again: nothing.
  void Stop();
  // Wait until pictures of the size last asked for are there and have come in fully (for a
  // screenshot); false after `seconds` without that.
  bool WaitUntilReady(double seconds);

 private:
  void Run();
  const Vfs* vfs_;
  std::string map_;
  std::thread thread_;
  std::atomic<bool> running_{true};
  std::atomic<bool> begun_{false};
  std::atomic<bool> vulkan_{false};
  std::mutex mutex_;
  int wantW_ = 0, wantH_ = 0;      // the size asked for
  std::chrono::steady_clock::time_point asked_{};  // when it was last asked for
  std::vector<uint32_t> ready_;    // the newest finished picture
  int readyW_ = 0, readyH_ = 0;
  uint64_t readyCount_ = 0;
  std::vector<uint32_t> shown_;    // the caller's copy of it
  int shownW_ = 0, shownH_ = 0;
  uint64_t shownCount_ = 0;
  // The kept first picture of an earlier run (see above): read once for the size asked, shown
  // only until this run's first picture is there.
  std::vector<uint32_t> still_;
  int stillW_ = 0, stillH_ = 0;
  int stillTriedW_ = 0, stillTriedH_ = 0;
  bool stillShown_ = false, liveSeen_ = false, stillSaved_ = false;
  void LoadStill(int width, int height);
  void SaveStill(const std::vector<uint32_t>& pixels, int width, int height);
};

}  // namespace moho::ui
