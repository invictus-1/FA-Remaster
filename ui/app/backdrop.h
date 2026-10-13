// The front end's new background: where the original plays a film behind the main menu
// (/movies/main_menu.sfd), this shows the game's own world: the renderer's picture of a map, the
// camera gliding slowly over it in the evening light. It is ours, not the original's (the film is
// not decoded; the new background was chosen to come first).
//
// The picture is made on a thread of its own, with a graphics device of its own, so the menu
// never waits for it: the menu takes the newest finished picture each frame. Until the first one
// is ready the background is black, as it was. While nobody asks for a picture (another screen is
// up) none is made.
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
  // `map`: the map file in the game's file system ("/maps/SCMP_009/SCMP_009.scmap").
  Backdrop(const Vfs* vfs, std::string map);
  ~Backdrop() override;
  const uint32_t* Latest(int width, int height) override;
  // End it and give the graphics device back (a game is starting: it has its own picture).
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
  std::mutex mutex_;
  int wantW_ = 0, wantH_ = 0;      // the size asked for
  std::chrono::steady_clock::time_point asked_{};  // when it was last asked for
  std::vector<uint32_t> ready_;    // the newest finished picture
  int readyW_ = 0, readyH_ = 0;
  uint64_t readyCount_ = 0;
  std::vector<uint32_t> shown_;    // the caller's copy of it
  int shownW_ = 0, shownH_ = 0;
  uint64_t shownCount_ = 0;
};

}  // namespace moho::ui
