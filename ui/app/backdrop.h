// The front end's new background: where the original plays a film behind the main menu
// (/movies/main_menu.sfd), this shows the game's own world: the renderer's picture of a map, the
// camera gliding slowly over it in the evening light. It is ours, not the original's (the film is
// not decoded; the new background was chosen to come first).
//
// Two ways to show it:
// - In the live window (the game's way, Begin's `direct`): the window's own picture is opened with
//   the map (GameSession::PresentToWindow) and draws it straight into the window, the interface
//   laid over it; the movie leaves its part of the interface transparent but for a veil (Show), so
//   the interface is painted only where it changes. TakeView gives that picture its camera each
//   frame the movie showed. Nothing is made here then and nothing is read back.
// - Otherwise (a run without a window that asks for it, --gdi): the picture is made on a thread of
//   its own, with a graphics device of its own, so the menu never waits for it: the menu takes the
//   newest finished picture each frame (Latest). Reading the map takes a few seconds; until the
//   first picture is ready the menu shows the first picture of the run before, kept as a file in
//   the system's folder for temporary files (the camera starts at the same place every time, so
//   the moving picture takes over from it without a jump); without such a file the background is
//   black and the picture comes in from black. While nobody asks for a picture (another screen is
//   up) none is made.
#pragma once
#include <atomic>
#include <condition_variable>
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
namespace render {
class WorldPicture;
struct Camera;
}  // namespace render

namespace moho::ui {

class Backdrop : public BackdropSource {
 public:
  // What plays behind the main menu (the Interface tab's "Menu Background", LookSettings::menuBackground; Chris
  // approved the flight on 2026-11-08, docs/ui-menuflight.md): Flight, the live battle over Seton's Clutch Remastered
  // (app/menu_flight.h; the window's picture only); Dusk, this background as it was before (stock Seton's in the
  // evening, the slow round); Still, one picture of Dusk's first view drawn once at the start and shown as it is
  // (GameSession: the lightest, for weak cards). The map follows the mode: SETONS_R for Flight, else stock Seton's.
  enum class Mode { Flight, Dusk, Still };
  static constexpr const char* kDuskMap = "/maps/SCMP_009/SCMP_009.scmap";
  static constexpr float kDuskHour = 17.4f;  // (the evening of the menu's background: Dress, and the flight's day)
  static const char* ModeName(Mode mode) { return mode == Mode::Flight ? "flight" : mode == Mode::Dusk ? "dusk" : "still"; }
  // `map`: the map file in the game's file system ("/maps/SCMP_009/SCMP_009.scmap"). The map is
  // read only after Begin(): its picture builds the renderer's pipelines for render::gSceneSamples,
  // which must not change while a world is alive (render/world.h), so the caller sets it first.
  Backdrop(const Vfs* vfs, std::string map);
  Backdrop(const Vfs* vfs, Mode mode);
  Mode mode() const { return mode_; }
  // The mode changed (the option, live; a fallback): the map with it (the window's picture opens it behind and swaps
  // it in, GameSession::ServeWindowMap). The direct way only (the read-back way's thread has its map).
  void SetMode(Mode mode);
  // A camera given by another (the flight's) fitted to the movie's part of the window, as TakeView fits its own.
  void FitToMovie(render::Camera* camera, int height) const;
  ~Backdrop() override;
  // `vulkan`: drawn with Vulkan in place of Direct3D 12 (the player's Graphics API);
  // `textureDetail`: WorldPicture::SetTextureDetail (the game's Texture Detail); `direct`: shown by
  // the window's own picture (see above), else made here on a thread of its own.
  void Begin(bool vulkan, int textureDetail, bool direct = false);
  const std::string& map() const { return map_; }
  // Shown by the window's picture: begun so and not stopped.
  bool Direct() const { return direct_ && begun_ && running_; }
  // Begun so, stopped since or not (a game stops it: GameSession::ServeWindowMap begins the menus' map behind a game's
  // world that is over, before the next front end's background is begun).
  bool BegunDirect() const { return direct_ && begun_; }
  // BackdropSource's.
  const uint32_t* Latest(int width, int height) override;
  bool DrawsToWindow() const override { return Direct(); }
  int Show(float x0, float y0, float x1, float y1) override;
  // For the window's picture (the direct way), once a frame before it is drawn: false when the
  // movie did not show since the last call (the window's picture then shows black under the
  // interface); else the camera and the moment (`tick`, as WorldPicture takes it) to draw the
  // map from, at the window's `width` x `height`.
  bool TakeView(const render::WorldPicture& picture, int width, int height, render::Camera* camera, double* tick);
  // The movie showed since the last TakeView (the main menu is up), without taking it (the flight's kept first frame is
  // shown there while no world is: GameSession::GiveBackdrop).
  bool ShownThisFrame() const { return shownThisFrame_; }
  void EndFrame() { shownThisFrame_ = false; }  // (the frame drawn: a showing not taken by TakeView is not the next frame's)
  // The background's look on a picture just opened with the map: the evening, a low sun, some clouds.
  static void Dress(render::WorldPicture& picture);
  // End it (a game is starting: it has its own picture; or the scene's samples change): its own
  // picture's thread ends and gives the graphics device back; the direct way is off (the window's
  // picture lets the map go). Again: nothing.
  void Stop();
  // Wait until pictures of the size last asked for are there and have come in fully (for a
  // screenshot); false after `seconds` without that.
  bool WaitUntilReady(double seconds);

  // --- the graphics device lost, and made again (the renderer's request 71 B; render/world_picture.h [devicelost]) ---
  // The read-back way's own picture is made and drawn on this background's thread, so its part of a recovery is done
  // there, asked for from the interface's thread and waited for (GameSession::ServeDevice, which does the same with
  // the program's other pictures: none can make a new device while another still holds the lost one, so every
  // picture lets go first and only then each makes its own anew). The direct way has no picture here (the window's
  // own shows the map): these then answer false, and nothing is done.
  // Its picture noticed that the device is gone (after a frame it drew: it draws no more until it is made anew).
  bool DeviceLost() const { return deviceLost_.load(std::memory_order_relaxed); }
  std::string DeviceLostReason();
  // The first half (WorldPicture::LetGoOfLostDevice) on its thread; a picture still opening is waited for first.
  // False: it has no picture of its own (not begun, the direct way, ended).
  bool LetGoOfLostDevice();
  // The second half (WorldPicture::RecoverDevice) there, and the evening laid on the new world (Dress).
  struct Recovered {
    bool own = false;   // it has a picture of its own (false: nothing was to be done)
    bool ok = false;    // made anew
    bool lost = false;  // not ok: no device could be made (ask again later); else its map could not be opened again
    std::string error;
    double seconds = 0, deviceSeconds = 0, mapSeconds = 0;  // (WorldPicture::LastRecoverNumbers)
    int deviceTries = 0;
    float timeOfDay = -1, cloudCover = -1;  // (tests: the new world's, as Dress left them)
  };
  // (`takeMapAsGone`, checks: the device is made anew, then its map is taken to be one that could not be opened)
  Recovered RecoverDevice(bool takeMapAsGone = false);
  // (checks: moho64ui_RecoverDevice("lose")) WorldPicture::LoseDeviceForTest on its picture, on its thread.
  bool LoseDeviceForTest();
  // (checks, before Begin: --shader-cache) Where its own picture's device keeps its compiled shaders and pipelines
  // (WorldPicture::SetShaderCache); empty: the player's own folder, as ever.
  void SetShaderCache(const std::string& folder) { shaderCache_ = folder; }
  // (tests) How many pictures its thread has finished so far.
  uint64_t Pictures();
  // It has a picture of its own, there or being opened (the read-back way, begun and not ended).
  bool OwnPicture();

 private:
  void Run();
  // (the device's asks: Run's thread serves them between its frames; true while its picture has no device to draw with)
  enum class DeviceAsk { None, LetGo, Recover, RecoverNoMap, Lose };
  enum class PictureState { None, Opening, Up, Ended };  // (its own picture: none asked for, being opened, there, gone)
  bool ServeDevice(render::WorldPicture& picture);
  bool AskDevice(DeviceAsk ask, Recovered* result);
  void SetPictureState(PictureState state);
  std::mutex deviceMutex_;  // (the asks and their answers, the picture's state, the reason)
  std::condition_variable deviceCv_;
  PictureState pictureState_ = PictureState::None;
  DeviceAsk deviceAsk_ = DeviceAsk::None;
  bool deviceAnswered_ = false, deviceAnswer_ = false;
  Recovered deviceResult_;
  std::string deviceReason_;
  std::atomic<bool> deviceLost_{false};
  bool deviceLetGo_ = false;  // (the thread's: its picture has let go of its device and waits for RecoverDevice)
  std::string shaderCache_;
  const Vfs* vfs_;
  std::string map_;
  Mode mode_ = Mode::Dusk;
  std::thread thread_;
  std::atomic<bool> running_{true};
  std::atomic<bool> begun_{false};
  std::atomic<bool> vulkan_{false};
  std::atomic<int> textureDetail_{0};
  bool direct_ = false;
  std::mutex mutex_;
  int wantW_ = 0, wantH_ = 0;      // the size asked for
  std::chrono::steady_clock::time_point asked_{};  // when it was last asked for
  // (The pictures change hands by swapping, never copied under the lock: a 4K picture is 32 MB, and the menu's
  // frame waits for the lock.)
  std::vector<uint32_t> ready_;    // the newest finished picture
  int readyW_ = 0, readyH_ = 0;
  uint64_t readyCount_ = 0;
  std::vector<uint32_t> shown_;    // the caller's: the last ready picture taken
  int shownW_ = 0, shownH_ = 0;
  uint64_t shownCount_ = 0;
  // The kept first picture of an earlier run (see above): read once for the size asked, shown
  // only until this run's first picture is there.
  std::vector<uint32_t> still_;
  int stillW_ = 0, stillH_ = 0;
  int stillTriedW_ = 0, stillTriedH_ = 0;
  bool stillShown_ = false, liveSeen_ = false, stillSaved_ = false;
  // The kept picture is read on a thread of its own (32 MB at 4K: some 50 ms), the background black meanwhile.
  std::thread stillLoader_;
  bool stillLoading_ = false;  // (under mutex_)
  void LoadStill(int width, int height);
  void SaveStill(const std::vector<uint32_t>& pixels, int width, int height);
  // The direct way's showing (Show, TakeView; the interface's thread alone).
  bool everShown_ = false, shownThisFrame_ = false;
  std::chrono::steady_clock::time_point firstShown_{}, lastShown_{}, shownSince_{};
  float movieHeight_ = 0;
  // (For pictures of it: MOHO64UI_BACKDROP_AT=<seconds> starts that far along the camera's way;
  // MOHO64UI_BACKDROP_HOLD=1 keeps it there, the world's moment too, to compare pictures. Still: held at its start.)
  double along_ = 0;
  bool hold_ = false;
};

}  // namespace moho::ui
