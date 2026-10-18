// The cinematic camera, for watching replays and taking pictures: free in the world, anywhere and at any height
// down to the ground, looking any way, toward the horizon and above it; its movement, turning and field of view
// eased (damped), so that whatever it follows comes smoothly. A host (the interface's replay screen) keeps a
// CineCamera, switches it on from where its own (the game's) camera is (Start), says what it follows (a scripted
// path, a place or a unit it circles, or nothing: it holds still or is flown by Fly, Turn and Zoom), moves it on once
// a frame (Update) and draws with the renderer's camera it makes (View). That camera says it is a cinematic one
// (Camera::cinematic): the renderer then places for such a view what it places round what the camera looks at.
// Switched off (Stop), it hands back the game's camera that goes on from where it was, and eases the picture back
// to that camera over a moment while the player already moves it (Shown).
// With it go the lens's depth of field (Lens, World::lens; WorldPicture::SetLens) and pictures larger than the
// window (WorldPicture::TakePhoto).
//
// Use, with the world picture `picture` and the game's camera `game`:
//   render::CineCamera cine;
//   const render::GroundFn ground = picture.CineGround();
//   switched on:   cine.Start(game);  then cine.FollowPath(path) or cine.Follow(place, ...) (or neither)
//   each frame:    if (following a unit) cine.FollowPlace(where picture.UnitTransform puts it now);
//                  cine.Update(seconds, ground);
//                  if (cine.TakeCut()) picture.CameraCut();
//                  with the lens on: lens.strength = cine.LensShare(); picture.SetLens(lens);
//                  picture.DrawToWindow(cine.Shown(game, ground), width, height, tick);
//   any camera input from the player:  game = cine.Stop(ground);  (then that input moves `game`)
// While it is off and not easing back, Shown returns `game` itself, so the host may draw with Shown always.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "camera.h"
#include "linalg.h"

namespace render {

// Where the camera is and which way it looks.
struct CinePose {
  Vec3 eye;           // world units (y: the height in the world, not over the ground)
  float yaw = 0;      // radians round the upright axis, as Camera::heading: 0 looks north (toward -z), pi/2 west
  float pitch = 0;    // radians below the horizon: 0 at the horizon, positive down (pi/2 straight down), negative up
  float fovY = 0.7f;  // the vertical field of view, radians (0.7: 40 degrees)
};

// The height one stands on at a place (the ground, or the water's surface over it).
using GroundFn = std::function<float(float x, float z)>;
// The same anywhere, from that in the map (`onMap`, `width` x `height` world units): beyond the map's edge the
// ground goes on mirrored, as the world beyond it is drawn (World::worldBeyond).
GroundFn MirroredGround(GroundFn onMap, float width, float height);

// A scripted flight: poses at times (seconds), flown through smoothly (a curve through the eyes whose speed is even
// through every key, the angles and the field of view eased the same way), held at the first before it and the
// last after it; it starts and ends gently.
class CinePath {
 public:
  struct Key {
    float time = 0;
    CinePose pose;
  };
  std::vector<Key> keys;  // in order of time
  // The pose at a moment.
  CinePose At(float time) const;
  float Length() const { return keys.empty() ? 0 : keys.back().time - keys.front().time; }
  // From text: keys with ';' between, each "t:x,y,z,yaw,pitch,fov" (the viewer's --cine-path): the time in
  // seconds, the place (y the height over the ground there, by `ground`), the angles and the field of view in
  // degrees. False (and `error` says why) when it cannot be read.
  bool Parse(const std::string& text, const GroundFn& ground, std::string* error);
};

class CineCamera {
 public:
  CinePose pose;    // where it is now (eased toward `wanted`)
  CinePose wanted;  // where it is asked to be
  // How long a change takes to settle (seconds, the time a critically damped spring takes to close most of it:
  // moves feel smooth, not floaty); 0: at once. SetPace sets them together.
  float moveSeconds = 0.35f, turnSeconds = 0.15f, zoomSeconds = 0.25f;
  float clearance = 0.35f;            // the eye stays at least this far above what one stands on
  float lowestFov = 0.087f, highestFov = 1.75f;  // 5 .. 100 degrees
  float lowestPitch = -1.45f, highestPitch = 1.5707963f;  // (straight down: the game's strategic view, Start's)
  // Following a place (Follow): how fast it circles it, radians a second (positive: round to the left).
  float orbitSpeed = 0.06f;
  // The game's camera it hands back (Stop): the least pitch it takes (radians below the horizon; the viewer's 0.1),
  // and its nearest and farthest distance from what it looks at.
  float gamePitch = 0.1f, gameNearest = 3, gameFarthest = 6000;
  float easeBackSeconds = 0.5f;  // how long the picture takes to go back to the game's camera after Stop

  // --- on and off, for a host whose own camera is the game's ---
  // Switched on where `game` is (its eye, heading, pitch and field of view): no jump. It then holds still there until
  // it is told to follow something.
  void Start(const Camera& game);
  // Switched off: the game's camera that goes on from where this one is (the same eye and way of looking, the pitch
  // held to at least `gamePitch` and the distance within the game's; what that changes Shown eases over).
  Camera Stop(const GroundFn& ground);
  bool On() const { return on_; }
  // What to draw with this frame: this camera's view while it is on; after Stop, for `easeBackSeconds`, a view eased
  // from where it was to `game` (the host's camera, which the player may move meanwhile); then `game` itself.
  // (Update moves the easing on: call it every frame, on or off.)
  Camera Shown(const Camera& game, const GroundFn& ground) const;
  bool EasingBack() const { return !on_ && back_ < 1; }
  // How much of the lens's blur (Lens::strength) goes with the picture: 1 while it is on, going to 0 as it eases back,
  // 0 when off.
  float LensShare() const { return on_ ? 1.0f : 1.0f - back_ * back_ * (3 - 2 * back_); }
  // How lively it moves: 0 calm (slow settling, slow circling) .. 1 active (quick).
  void SetPace(float pace);

  // --- what it follows ---
  // A scripted flight from its start (eased into from where the camera is: `easeIn` false, a cut to its start).
  void FollowPath(const CinePath& path, bool easeIn = true);
  // A place (a unit's, from WorldPicture::UnitTransform, given anew every frame by FollowPlace): circled at
  // `distance` world units (along the ground) and `height` over the place, looking at it, from the side the camera
  // is on now.
  void Follow(Vec3 place, float distance, float height);
  void FollowPlace(Vec3 place) { followPlace_ = place; }
  // Nothing: it stays where it is (and Fly, Turn, Zoom move it).
  void Hold() { mode_ = Mode::Hold; }
  enum class Mode { Hold, Path, Follow };
  Mode Following() const { return mode_; }
  const CinePath& Path() const { return path_; }   // (the path it flies, Mode::Path)
  Vec3 FollowedPlace() const { return followPlace_; }  // (the place it circles, Mode::Follow)
  float PathTime() const { return pathTime_; }
  // How far what it follows is from the eye (for the lens's focus: World::lens.focus); 0 when it follows no place.
  float FollowedDistance() const { return mode_ == Mode::Follow ? Length(followPlace_ - pose.eye) : 0; }

  // --- moved directly (the viewer's flying; a pose set by the host) ---
  // A cut: there at once, nothing eased (the renderer is told: World::cameraCut, for what keeps frames before).
  void Jump(const CinePose& to);
  // Asked to go there, eased from where it is (turning the short way round).
  void GoTo(const CinePose& to);
  // Moves what is asked by `along` world units in the camera's own directions: x to the right, y straight up,
  // z forward (level: along the ground, whatever the pitch, as one walks; `level` false: along the line of sight).
  void Fly(Vec3 along, bool level = true);
  // Turns what is asked by these radians: yaw to the left positive (as Camera::heading grows), pitch down positive.
  void Turn(float yaw, float pitch);
  // Multiplies the field of view asked for (below 1 zooms in).
  void Zoom(float factor);

  // Moves it on by `seconds` (the frame's time): what it follows, then the pose eased toward what is asked, the eye
  // kept over the ground; after Stop, the easing back.
  void Update(float seconds, const GroundFn& ground);
  // True once after a cut (Jump, a path begun with one): the host tells the renderer (WorldPicture::CameraCut,
  // World::cameraCut).
  bool TakeCut() {
    const bool c = cut_;
    cut_ = false;
    return c;
  }

  // The renderer's camera for the pose now (Camera::cinematic set): its target the place at the picture's middle
  // where the line of sight meets the ground, held between 2 and 120 world units ahead (120 looking at the sky).
  Camera View(const GroundFn& ground) const { return MakeView(pose, ground); }
  static Camera MakeView(const CinePose& pose, const GroundFn& ground);
  // Any camera's pose (a game's camera: its eye, heading, pitch, field of view).
  static CinePose PoseOf(const Camera& camera);
  // The game's camera with that eye and way of looking: its target where the line of sight meets the ground (or
  // the level ground under the eye, looking over it), its pitch at least `leastPitch`, its distance within
  // `nearest` .. `farthest`.
  static Camera GameView(const CinePose& pose, const GroundFn& ground, float leastPitch, float nearest, float farthest);

 private:
  Vec3 eyeSpeed_;  // (the springs' speeds)
  float yawSpeed_ = 0, pitchSpeed_ = 0, fovSpeed_ = 0;
  bool cut_ = false, on_ = false;
  Mode mode_ = Mode::Hold;
  CinePath path_;
  float pathTime_ = 0;
  Vec3 followPlace_;
  float followDistance_ = 20, followHeight_ = 6, orbit_ = 0;
  CinePose left_;     // (where it was when it was switched off)
  float back_ = 1;    // (how far the easing back has come, 0..1)
};

// The lens (World::lens): depth of field for the cinematic camera, as a real lens makes it (thin-lens sums on a
// full-frame sensor, its focal length from the field of view). Off at first; everything else is unchanged then.
struct Lens {
  bool on = false;
  float fNumber = 2.8f;        // the aperture: 1.4 very shallow .. 22 nearly everything sharp
  float focus = 0;             // world units from the eye to what is sharp; 0: auto-focus on what is at the middle
  float focusSeconds = 0.4f;   // auto-focus: how long it takes to follow a change (eased); 0: at once
  // How large the world is taken to be for the lens's sums: metres a world unit stands for. A lens at the game's
  // own scale (some 10 to 20 m a unit) blurs next to nothing but at a few units' distance; smaller, the lens sees
  // the battlefield as a model of it and blurs as a film's close shots do.
  float unitMetres = 0.125f;
  float largestBlur = 0.02f;   // the widest a blurred point's disc may be, as a share of the picture's height (radius)
  // How much of the blur there is, 0..1 (the aperture closed down toward none): CineCamera::LensShare, so that the
  // blur goes as the picture eases back to the game's camera and does not stop at once.
  float strength = 1;
};

}  // namespace render
