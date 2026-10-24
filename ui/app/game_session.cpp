#include "app/game_session.h"

#include "app/backdrop.h"
#include "app/window.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "core/crash.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/command_stream.h"
#include "sim/command_writer.h"
#include "sim/fingerprint.h"
#include "sim/moments.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/view.h"
#include "render/world.h"
#include "render/world_picture.h"
#endif

#include <algorithm>
#include <cstdlib>

#include "sim/sim_view.h"
#include "user/bindings.h"
#include "user/keymap.h"
#include "user/replay_timeline.h"
#include "user/user_state.h"
#include "sound/sound_system.h"

namespace moho::ui {

// The commit this program was built from (build_ui.py writes it into the build folder's build_commit.cpp).
extern const char kBuildCommit[];

#ifdef MOHO64UI_WORLD_PICTURE
namespace {
// The player's looks that are the world's own switches (the Options menu's Video tab), on `w`: each frame on the
// world shown (Picture::SetScene), and on a game's world before it opens behind the window's picture (BeginWindowMap's
// `before`: what its opening and its unseen first frames make is then made for them). `photoLook`: the Photographic
// Look's grade set too (on a change only: Named sets the grade, the eye and lens keep theirs).
void WorldLooks(render::World& w, const LookSettings& looks, bool photoLook) {
  w.look = looks.originalLook ? render::Look::Original : render::Look::Modern;  // (the player's "Look")
  w.ribbons.tracers = looks.tracers;
  w.terrain.richGround = looks.richGround && looks.fidelity >= 1;  // (Fidelity Low: plain ground)
  w.ao.strength = looks.fidelity >= 1 ? 1.0f : 0.0f;
  w.shadowsOn = looks.shadows >= 1;
  w.lampShadows = looks.shadows >= 3;
  w.shadowSize = looks.shadows <= 1 ? 1024 : looks.shadows == 2 ? 2048 : 4096;  // (the renderer's offer)
  w.meshes.detail = looks.meshDetail;
  w.tracks.on = looks.snowTracks;
  w.icons.playerScale = looks.iconScale;
  w.icons.always = looks.iconsAlways;
  w.bloom = looks.bloom;
  w.bloomIntensity = looks.bloomIntensity;
  w.waterRipples = looks.waterRipples;  // (Water Ripples: the renderer's World::waterRipples)
  // (Battle Effects: the renderer's bundle as agreed on 2026-10-18; Energy Shields: its own)
  w.blastWaves = w.blastDust = w.blastDebris = looks.battleEffects;
  w.litSmoke = w.smokeShadows = w.heatShimmer = looks.battleEffects;
  w.unitScars = looks.battleEffects;
  // (render e8a4d56: wrecks and craters that glow and cool; dust, spray and contrails; explosions as lit volumes
  // and the nuke's set piece, which without the volume art still throws the blasts' light and the nuke's flash)
  w.hotMetal = w.movementDust = w.volumeExplosions = looks.battleEffects;
  w.realExhausts = looks.battleEffects;  // (engines' exhausts as lit plumes, render 3e6ce2e: Chris's item 8)
  w.remasteredBuild = looks.battleEffects;  // (render 6b8109e: the build effects remade; with the remastered effects and the modern look)
  w.energyShields = looks.energyShields;
  // (ren_DecalFadeFraction: the decals fade by distance as the original's CWldTerrainDecal::GetLODAlpha 0x89d400,
  // render 33; 0.5 stock, 0.25 once FAF's gamemain.lua has set it)
  w.decalFadeFraction = looks.decalFadeFraction;
  // (the Photographic Look, render ef7e1bb: on change only; Named sets the grade, the eye and lens keep theirs)
  if (photoLook) {
    w.photoLook.on = looks.photoLook != 0;
    static const char* const kNames[5] = {"", "plain", "film", "warm", "coldwar"};
    if (w.photoLook.on) w.photoLook.Named(kNames[looks.photoLook]);
  }
}
// The player's looks that are the picture's own switches, all of them (Picture::SetScene gives each when it changes).
void PictureLooks(render::WorldPicture& p, const LookSettings& looks) {
  p.SetRemasteredEffects(looks.remasteredEffects);
  p.SetEffectsQuality(looks.effectsQuality);
  p.SetUpscale(looks.upscale);
  p.SetTemporalUpscale(looks.temporalUpscale);
  p.SetAtmosphere(looks.atmosphere);
  p.SetSoftShadows(looks.softShadows);
  p.SetRayTracing(looks.rayTracing);
  p.SetWeatherEffects(looks.weatherEffects);
  p.SetGroundDetail(looks.groundDetail);
  p.SetFrameGeneration(looks.frameGeneration);
  p.SetHiddenOutlines(looks.hiddenOutlines);
  p.SetMinimapMeshes(looks.minimapLod != 0);
}
// A game's sky on its world (GameSession::Sky): the map's edge, and a kind of weather that stays set with the values
// the renderer's own kinds have (render/weather.cpp, kKinds) and with what that weather has done to the ground by now
// (weather that changes by itself is the renderer's, by the game's seed and clock: WorldPicture::SetDayAndWeather).
void SkyOnWorld(render::World& w, const std::string& edge, float startHour, const std::string& kind) {
  // The map's edge, one of the two looks (the renderer's answer to ui request 20: all three
  // switches set, as their defaults are neither look).
  const bool frame = edge == "frame";
  w.worldBeyond = !frame;
  w.frame = frame;
  w.beyondHaze = false;
  if (startHour < 0) return;
  struct Kind {
    const char* name;
    float clouds, rain, snow, fog;
  };
  static const Kind kKinds[] = {{"none", 0.35f, 0, 0, 0},     {"clear", 0.10f, 0, 0, 0},      {"cloudy", 0.65f, 0, 0, 0},
                                {"overcast", 0.92f, 0, 0, 0.1f}, {"rain", 0.85f, 0.5f, 0, 0},  {"storm", 1.0f, 1.0f, 0, 0.25f},
                                {"fog", 0.5f, 0, 0, 1.0f},    {"snow", 0.9f, 0, 0.8f, 0.1f}};
  w.climate = kind == "cold" ? render::Climate::Cold : kind == "dry" ? render::Climate::Dry : render::Climate::Temperate;
  for (const Kind& k : kKinds) {
    if (kind != k.name) continue;
    w.cloudCover = k.clouds, w.rain = k.rain, w.snow = k.snow, w.fog = k.fog;
    w.wet = k.rain > 0 ? 1.0f : 0.0f;
    w.snowLying = k.snow > 0 ? 1.0f : 0.0f;
  }
}
bool WeatherByItself(const std::string& kind) { return kind == "changing" || kind == "cold" || kind == "dry"; }
}  // namespace
#endif

// The renderer's world picture behind the interface's WorldSource.
struct GameSession::Picture : WorldSource {
#ifdef MOHO64UI_WORLD_PICTURE
  // The renderer's picture: the window's (GameSession::WindowPicture, which shows the menus' map and each game's in
  // turn: render request 37), or one of its own (a run without the window's picture: read back each frame).
  std::unique_ptr<render::WorldPicture> own;
  render::WorldPicture& picture;
  explicit Picture(render::WorldPicture* shared) : own(shared ? nullptr : std::make_unique<render::WorldPicture>()), picture(shared ? *shared : *own) {}
  GameSession* session = nullptr;
  LookSettings lookNow;      // the player's looks as last given to the picture
  bool lookApplied = false;
  std::vector<uint32_t> iconColorsNow;  // the icons' team colours as last given (empty: the armies' own)
  std::vector<uint32_t> armyColorsNow;  // the units' own colours as last given (Colour Vision; empty: the game's)
  float playableNow[4] = {0, 0, -1, -1};  // the playable part as last given
  // The order lines and their settings as last given (SetOrderLines, SetOrderLineSettings).
  std::vector<std::string> orderTexturesNow;
  std::vector<WorldOrderLeg> orderLegsNow;
  std::vector<WorldOrderMark> orderMarksNow;
  WorldOrderSettings orderSettingsNow;
  bool orderSettingsApplied = false;
  // On the game's window (UseWindow): the world view's Draw only notes its camera and size, and
  // the frame is drawn and shown by PresentToWindow with the interface over it.
  bool direct = false;
  render::Camera camera;
  int cameraW = 0, cameraH = 0;
  // The moment the frame shows, once fixed this frame by LifeBarAnchors (PresentToWindow takes
  // the same, so the bars it draws are those the anchors were of).
  double moment = 0;
  bool momentFixed = false;
  bool DrawsToWindow() const override { return direct; }
  // The cinematic camera for replays (ours: the renderer's CineCamera, render/cinecam.h, and his answer of
  // 2026-10-18 in docs/render-answers.md, "To ui: the cinematic camera for replays"): the world view's input and
  // console command switch it (maui/worldview.cpp); here what it needs of the picture. `cineGround`: the picture's
  // ground for it (CineGround, taken when it first starts); `cineUnit`: the unit it follows (0xffffffff: none).
  render::CineCamera cine;
  render::GroundFn cineGround;
  uint32_t cineUnit = 0xffffffffu;
  // While it shows (on, or easing back) the picture has no life bars, selection marks or work bars, and none of
  // the interface's shapes in the world (lines, range rings, order lines, ghosts); after, again what the player has
  // (`lifeBarsWanted`: the scene's, his switch). What was last given, so that each is set when it changes.
  bool lifeBarsWanted = false;
  bool marksApplied = false, lifeBarsGiven = false, marksGiven = true, workBarsGiven = true, lensGiven = false;
  // The camera the frame was last drawn with, and whether that was the cinematic camera's (ShownCamera).
  render::Camera shownNow;
  bool shownCine = false;
  bool CineShows() const { return cine.On() || cine.EasingBack(); }
  // The photo mode (ours: the renderer's answer of 2026-10-18 in docs/render-answers.md, "To ui: the photo mode's calls
  // are in"): the same CineCamera, flown freely (PhotoFly) while `photoOn`; `photoEasing` while the picture eases back
  // from it (its lens goes with it, not the replay camera's). `photoLens`: the photo mode's own lens; `momentSet`: a
  // moment of its own is set (SetPhotoMoment).
  bool photoOn = false, photoEasing = false, momentSet = false;
  PhotoLens photoLens;
  static render::Camera ToRender(const WorldCamera& c) {
    render::Camera cam;
    cam.target = {c.target[0], c.target[1], c.target[2]};
    cam.distance = c.distance;
    cam.heading = c.heading;
    cam.pitch = c.pitch;
    cam.fovY = c.fovY;
    cam.lodScale = c.lodScale;  // (cam_SetLOD's: render 33 / 34, ui REQUESTS 34)
    return cam;
  }
  static WorldCamera FromRender(const render::Camera& c) {
    WorldCamera w;
    w.target[0] = c.target.x, w.target[1] = c.target.y, w.target[2] = c.target.z;
    w.distance = c.distance;
    w.heading = c.heading;
    w.pitch = c.pitch;
    w.fovY = c.fovY;
    w.lodScale = c.lodScale;
    return w;
  }
  // The bars and marks as the cinematic camera wants them (none while it shows), each set when it changes.
  void ApplyCineMarks() {
    const bool shows = CineShows();
    const bool lifeBars = lifeBarsWanted && !shows, marks = Looks().selectionMarks && !shows, work = !shows;
    if (!marksApplied || lifeBars != lifeBarsGiven) picture.SetLifeBars(lifeBars), lifeBarsGiven = lifeBars;
    if (!marksApplied || marks != marksGiven) picture.SetSelectionMarks(marks), marksGiven = marks;
    if (!marksApplied || work != workBarsGiven) picture.SetWorkBars(work), workBarsGiven = work;
    marksApplied = true;
  }
  // The lens's depth of field (the player's Cinematic Lens Blur, LookSettings::lens): only while the cinematic
  // camera shows, its blur by CineCamera::LensShare (going as the picture eases back), focused on the unit it follows
  // (FollowedDistance; 0: on what is at the picture's middle); the renderer's lens otherwise (f/2.8, its scale).
  // From the photo mode (and while the picture eases back from it): the photo mode's own lens, its blur by LensShare too.
  void ApplyLens() {
    const bool photo = photoOn || photoEasing;
    const bool on = (photo ? photoLens.on : Looks().lens) && CineShows();
    if (!on && !lensGiven) return;
    render::Lens lens;
    lens.on = on;
    lens.strength = cine.LensShare() * (photo ? photoLens.strength : 1.0f);
    if (photo) lens.fNumber = photoLens.fNumber, lens.focus = photoLens.focus;
    else lens.focus = cine.FollowedDistance();
    picture.SetLens(lens);
    lensGiven = on;
  }
  bool CineStart(const WorldCamera& game, uint32_t follow) override {
    if (photoOn) return false;  // (the photo mode has the camera)
    if (!cineGround) cineGround = picture.CineGround();
    const render::Camera from = ToRender(game);
    cine.Start(CineShows() ? cine.Shown(from, cineGround) : from);  // (switched on again while easing back: from what shows)
    photoEasing = false;
    cineUnit = follow;
    float p[3], q[4];
    if (follow != 0xffffffffu && picture.UnitTransform(follow, FrameMoment(), p, q)) cine.Follow({p[0], p[1], p[2]}, 14, 3);
    ApplyCineMarks();
    ApplyLens();
    return true;
  }
  bool CineStop(WorldCamera* game) override {
    if (!cine.On() || !cineGround || photoOn) return false;
    *game = FromRender(cine.Stop(cineGround));
    cineUnit = 0xffffffffu;
    return true;
  }
  void CineFrame(float seconds) override {
    if (!cineGround) return;
    if (cine.On() && cine.Following() == render::CineCamera::Mode::Follow) {  // (the unit where the frame shows it)
      float p[3], q[4];
      if (picture.UnitTransform(cineUnit, FrameMoment(), p, q)) cine.FollowPlace({p[0], p[1], p[2]});
      else cine.Hold();  // (gone: it holds where it is)
    }
    cine.Update(seconds, cineGround);
    if (cine.TakeCut()) picture.CameraCut();
    if (photoEasing && !CineShows()) photoEasing = false;  // (eased back from the photo mode)
    ApplyCineMarks();
    ApplyLens();
  }
  // The photo mode (WorldSource: PhotoStart and its kin).
  bool PhotoStart(const WorldCamera& game) override {
    if (!picture.world()) return false;
    if (!cineGround) cineGround = picture.CineGround();
    const render::Camera from = ToRender(game);
    cine.Start(CineShows() ? cine.Shown(from, cineGround) : from);  // (from what shows: no jump)
    float mapW = 0, mapH = 0;
    picture.MapSize(&mapW, &mapH);
    cine.SetMap(mapW, mapH);  // (Fly's reach round the map)
    cineUnit = 0xffffffffu;
    photoOn = true, photoEasing = false;
    ApplyCineMarks();
    ApplyLens();
    return true;
  }
  void PhotoFly(float seconds, const float move[3], const float turn[2], float roll) override {
    if (!photoOn || !cine.On()) return;
    cine.Fly(seconds, {move[0], move[1], move[2]}, {turn[0], turn[1]}, roll);
  }
  void PhotoStop() override {
    if (!photoOn) return;
    photoOn = false;
    // (the game's camera Stop hands back is not taken: the player's camera stays where it was, and Shown eases the
    // picture back to it over the cinematic camera's half second, the roll levelling out)
    if (cine.On() && cineGround) cine.Stop(cineGround);
    photoEasing = CineShows();
    picture.ClearPhotoMoment();
    momentSet = false;
    ApplyCineMarks();
    ApplyLens();
  }
  PhotoPose PhotoCamera() const override {
    PhotoPose p;
    const render::CinePose& at = cine.pose;
    p.eye[0] = at.eye.x, p.eye[1] = at.eye.y, p.eye[2] = at.eye.z;
    p.yaw = at.yaw, p.pitch = at.pitch, p.roll = at.roll, p.fovY = at.fovY;
    p.free = photoOn && cine.Following() == render::CineCamera::Mode::Free;
    return p;
  }
  void SetPhotoMoment(float hour, int weather, float strength) override {
    picture.SetPhotoMoment(hour, weather, strength);
    momentSet = true;
  }
  void ClearPhotoMoment() override {
    picture.ClearPhotoMoment();
    momentSet = false;
  }
  PhotoMoment GetPhotoMoment() const override {
    PhotoMoment m;
    picture.GetPhotoMoment(&m.hour, &m.weather, &m.strength);
    m.set = momentSet;
    return m;
  }
  std::vector<std::string> WeatherKinds() const override { return render::WorldPicture::WeatherKinds(); }
  void SetPhotoLens(const PhotoLens& lens) override {
    photoLens = lens;
    ApplyLens();
  }
  float FocusAt(float x, float y) override { return picture.FocusAt(x, y); }
  bool StartPhoto(int width, int height) override {
    if (!photoOn || !cineGround || width <= 0 || height <= 0) return false;
    render::Camera view = cine.View(cineGround);
    view.lodScale = shownNow.lodScale;  // (the cinematic camera's views start at 1: the game camera's LOD scale)
    return picture.StartPhoto(view, width, height, FrameMoment(), 0);  // (0: the renderer's supersampling)
  }
  float PhotoProgress() const override { return picture.PhotoProgress(); }
  bool TakeDonePhoto(std::vector<uint32_t>* pixels, int* width, int* height) override { return picture.TakeDonePhoto(pixels, width, height); }
  CineState Cine() const override {
    CineState s;
    s.on = cine.On(), s.easingBack = cine.EasingBack();
    s.following = cine.On() && cine.Following() == render::CineCamera::Mode::Follow;
    const render::Vec3 f = cine.FollowedPlace();
    s.followed[0] = f.x, s.followed[1] = f.y, s.followed[2] = f.z;
    s.lifeBars = lifeBarsGiven, s.selectionMarks = marksGiven, s.workBars = workBarsGiven, s.lens = lensGiven;
    return s;
  }
  bool ShownCamera(WorldCamera* out) const override {
    if (!shownCine || !CineShows()) return false;
    *out = FromRender(shownNow);
    return true;
  }
  bool TakePhoto(const WorldCamera& game, int width, int height, std::vector<uint32_t>* pixels, int* madeWidth, int* madeHeight) override {
    if (!cineGround) cineGround = picture.CineGround();
    render::Camera camera = cine.On() ? cine.View(cineGround) : ToRender(game);
    camera.lodScale = game.lodScale;  // (the cinematic camera's views start at 1)
    return picture.TakePhoto(camera, width, height, FrameMoment(), pixels, madeWidth, madeHeight);
  }
  // The frame's moment, fixed by the first that asks this frame (the camera following a unit asks
  // before the anchors and the drawing); PresentToWindow lets it go after.
  double FrameMoment() {
    if (!direct) return session ? session->PictureTick() : 0.0;
    if (!momentFixed) moment = session ? session->PictureTick() : 0.0, momentFixed = true;
    return moment;
  }
  bool UnitTransform(uint32_t id, float position[3], float orientation[4]) override {
    return session && picture.UnitTransform(id, FrameMoment(), position, orientation);
  }
  void LifeBarAnchors(std::vector<BarAnchor>* out) override {
    out->clear();
    if (!direct || cameraW <= 0 || !session) return;
    FrameMoment();
    std::vector<render::WorldPicture::LifeBarAnchor> anchors;
    picture.LifeBarAnchors(camera, cameraW, cameraH, moment, &anchors);
    out->reserve(anchors.size());
    for (const auto& a : anchors) out->push_back({a.unit, a.x, a.y});
  }
  // [views] (WorldSource::SetViews: the renderer's WorldPicture::SetViews, render-answers "request 35") The views as
  // last given, each to the renderer when they change; one view (an empty list): the picture as it always was. With
  // views the world view's Draw comes with the window's size (cameraW x cameraH: what LifeBarAnchors and the frame are
  // prepared with), and view 0 is drawn at its own place's size.
  std::vector<WorldViewSpec> viewsNow;
  bool SetViews(const std::vector<WorldViewSpec>& views, int windowW, int windowH) override {
    (void)windowW, (void)windowH;
    if (views == viewsNow) return false;
    viewsNow = views;
    std::vector<render::WorldPicture::PictureView> list;
    list.reserve(views.size());
    for (const WorldViewSpec& v : views) {
      render::WorldPicture::PictureView p;
      p.rect = {v.x, v.y, v.width, v.height};
      p.camera = ToRender(v.camera);
      p.kind = v.kind == WorldViewSpec::Kind::Split   ? render::WorldPicture::ViewKind::Split
               : v.kind == WorldViewSpec::Kind::Inset ? render::WorldPicture::ViewKind::Inset
                                                      : render::WorldPicture::ViewKind::Main;
      p.every = std::max(v.every, 1);
      list.push_back(p);
    }
    picture.SetViews(list);
    return true;
  }
  int ViewAt(float x, float y) override { return picture.ViewAt(x, y); }
  bool ViewProject(int view, const float world[3], float* x, float* y) override {
    return picture.ViewProject(view, {world[0], world[1], world[2]}, x, y);
  }
  bool ViewGroundAt(int view, float x, float y, float world[3]) override {
    render::Vec3 hit;
    if (!picture.ViewGroundAt(view, x, y, &hit)) return false;
    world[0] = hit.x, world[1] = hit.y, world[2] = hit.z;
    return true;
  }
  uint32_t ViewUnitAt(int view, float x, float y) override { return picture.ViewUnitAt(view, x, y); }
  void ViewUnitsIn(int view, float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) override {
    picture.ViewUnitsIn(view, x0, y0, x1, y1, out);
  }
  void ViewLifeBarAnchors(int view, std::vector<BarAnchor>* out) override {
    if (view == 0) return LifeBarAnchors(out);
    out->clear();
    if (!direct || !session) return;  // (as LifeBarAnchors: the window's picture only)
    std::vector<render::WorldPicture::LifeBarAnchor> anchors;
    picture.ViewLifeBarAnchors(view, &anchors);
    out->reserve(anchors.size());
    for (const auto& a : anchors) out->push_back({a.unit, a.x, a.y});
  }

  const uint32_t* Draw(const WorldCamera& c, int width, int height) override {
    render::Camera cam = ToRender(c);
    // (the cinematic camera's view while it shows; Shown is the game's camera once it has eased back)
    shownCine = CineShows() && cineGround;
    if (shownCine) cam = cine.Shown(cam, cineGround);
    cam.lodScale = c.lodScale;  // (the cinematic camera's views start at 1: the game camera's LOD scale, cam_SetLOD)
    shownNow = cam;
    if (direct) {  // (drawn and shown with the interface by PresentToWindow)
      camera = cam, cameraW = width, cameraH = height;
      static const uint32_t token = 0;
      return &token;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint32_t>& pixels = picture.Draw(cam, width, height, session->PictureTick());
    FrameTimes& times = Times();
    times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ++times.worldDraws;
    return pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) ? pixels.data() : nullptr;
  }
  bool Project(const float world[3], float* x, float* y) override {
    return picture.Project({world[0], world[1], world[2]}, x, y);
  }
  bool GroundAt(float x, float y, float world[3]) override {
    render::Vec3 hit;
    if (!picture.GroundAt(x, y, &hit)) return false;
    world[0] = hit.x, world[1] = hit.y, world[2] = hit.z;
    return true;
  }
  uint32_t UnitAt(float x, float y) override { return picture.UnitAt(x, y); }
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) override {
    picture.UnitsIn(x0, y0, x1, y1, out);
  }
  void SetScene(const WorldScene& scene) override {
    picture.SetSelection(scene.selection);
    // (While the cinematic camera shows, none of the interface's shapes in the world: the renderer's advice.)
    const bool cineShows = CineShows();
    static const std::vector<WorldGhost> kNoGhosts;
    static const std::vector<WorldLine> kNoLines;
    static const std::vector<WorldOrderLeg> kNoLegs;
    static const std::vector<WorldOrderMark> kNoMarks;
    static const std::vector<WorldRangeGroup> kNoRanges;
    std::vector<render::WorldPicture::BuildPreview> previews;
    for (const WorldGhost& g : cineShows ? kNoGhosts : scene.ghosts) {
      render::WorldPicture::BuildPreview p;
      p.blueprint = g.blueprint;
      p.position[0] = g.position[0], p.position[1] = g.position[1], p.position[2] = g.position[2];
      p.heading = g.heading;
      p.free = g.free;
      previews.push_back(p);
    }
    picture.SetBuildPreviews(previews);
    std::vector<render::WorldPicture::Line> lines;
    for (const WorldLine& l : cineShows ? kNoLines : scene.lines) {
      render::WorldPicture::Line o;
      for (int i = 0; i < 3; ++i) o.a[i] = l.a[i], o.b[i] = l.b[i];
      o.color = l.color;
      o.width = l.width;
      lines.push_back(o);
    }
    picture.SetShapes(lines, {});
    // The order lines while Shift is held (the original's command graph), when they change; empty
    // lists when Shift is let go (renderer request 30). Then the console variables they go by.
    const std::vector<WorldOrderLeg>& legsWanted = cineShows ? kNoLegs : scene.orderLegs;
    const std::vector<WorldOrderMark>& marksWanted = cineShows ? kNoMarks : scene.orderMarks;
    if (scene.orderTextures != orderTexturesNow || legsWanted != orderLegsNow || marksWanted != orderMarksNow) {
      orderTexturesNow = scene.orderTextures, orderLegsNow = legsWanted, orderMarksNow = marksWanted;
      std::vector<render::WorldPicture::OrderLeg> legs;
      legs.reserve(orderLegsNow.size());
      for (const WorldOrderLeg& l : orderLegsNow) {
        render::WorldPicture::OrderLeg o;
        for (int i = 0; i < 3; ++i) o.a[i] = l.a[i], o.b[i] = l.b[i], o.ta[i] = l.ta[i], o.tb[i] = l.tb[i];
        o.baseWidth = l.baseWidth, o.color = l.color, o.texture = l.texture, o.uvAspect = l.uvAspect, o.animRate = l.animRate;
        legs.push_back(o);
      }
      std::vector<render::WorldPicture::OrderMark> marks;
      marks.reserve(orderMarksNow.size());
      for (const WorldOrderMark& m : orderMarksNow) {
        render::WorldPicture::OrderMark o;
        for (int i = 0; i < 3; ++i) o.position[i] = m.position[i];
        o.texture = m.texture, o.color = m.color, o.size = m.size;
        marks.push_back(o);
      }
      picture.SetOrderLines(orderTexturesNow, legs, marks);
    }
    if (!orderSettingsApplied || !(scene.orderSettings == orderSettingsNow)) {
      orderSettingsNow = scene.orderSettings, orderSettingsApplied = true;
      const WorldOrderSettings& s = orderSettingsNow;
      picture.SetOrderLineSettings(s.lineScale, s.curveSmoothness, s.curveSegments, s.markMinPixels, s.markMaxPixels);
    }
    // The range rings in groups: the renderer draws a group as one outline.
    std::vector<render::WorldPicture::RangeGroup> ranges;
    for (const WorldRangeGroup& g : cineShows ? kNoRanges : scene.ranges) {
      render::WorldPicture::RangeGroup o;
      o.color = g.color, o.glow = g.glow;
      o.outerNear = g.outer[0], o.outerFar = g.outer[1];
      o.innerNear = g.inner[0], o.innerFar = g.inner[1];
      o.fill = g.fill;
      for (const WorldRangeRing& r : g.rings) {
        render::WorldPicture::RangeRing ring;
        for (int i = 0; i < 3; ++i) ring.centre[i] = r.centre[i];
        ring.radius = r.radius, ring.innerRadius = r.innerRadius;
        o.rings.push_back(ring);
      }
      ranges.push_back(std::move(o));
    }
    picture.SetRanges(ranges, scene.zoomNear, scene.zoomFar);
    // The meshes the game's script has placed in the world, and the marks where orders landed.
    std::vector<render::WorldPicture::WorldMesh> meshes;
    for (const WorldMeshView& m : scene.meshes) {
      render::WorldPicture::WorldMesh o;
      o.mesh = m.mesh, o.texture = m.texture, o.shader = m.shader;
      o.uniformScale = m.uniformScale;
      for (int i = 0; i < 3; ++i) o.position[i] = m.position[i], o.scale[i] = m.scale[i];
      for (int i = 0; i < 4; ++i) o.orientation[i] = m.orientation[i];
      o.color = m.color, o.hidden = m.hidden;
      o.lifetime = m.lifetime, o.age = m.age, o.fractionComplete = m.fractionComplete;
      o.lodCutoff = m.lodCutoff;
      meshes.push_back(std::move(o));
    }
    picture.SetWorldMeshes(meshes);
    // The script's decals on the ground (UserDecal: the rings under the cursor while aiming).
    std::vector<render::WorldPicture::UserDecal> decals;
    for (const WorldDecal& d : scene.decals) decals.push_back({d.texture, d.position[0], d.position[2], d.size[0], d.size[1]});
    picture.SetUserDecals(decals);
    // The playable part of the map, when it changes.
    if (!std::equal(scene.playableRect, scene.playableRect + 4, playableNow)) {
      std::copy(scene.playableRect, scene.playableRect + 4, playableNow);
      picture.SetPlayableRect(playableNow[0], playableNow[1], playableNow[2], playableNow[3]);
    }
    // The strategic icons' team colours (TeamColorMode), when they change.
    if (scene.armyColors != armyColorsNow) {  // (render request 36)
      armyColorsNow = scene.armyColors;
      picture.SetArmyColors(armyColorsNow);
    }
    if (scene.iconColors != iconColorsNow) {
      iconColorsNow = scene.iconColors;
      picture.SetIconColors(iconColorsNow);
    }
    picture.SetBorders(scene.borders);  // the machinery the map lies in (MapBorderAdd)
    // the brackets on the unit under the mouse (none while the cinematic camera or the photo mode's shows: a mark)
    picture.SetHover(cineShows ? 0xffffffffu : scene.hover, cineShows ? 0 : scene.hoverKind);
    lifeBarsWanted = scene.lifeBars;  // the player's switch (Alt-L in the game's key map): ApplyCineMarks below
    // Life bars over the player's and his allies' units only, as the original (render request
    // 21), unless the player asks for the enemies' too.
    picture.SetLifeBarsOnEnemies(scene.lifeBarsOnEnemies);
    // The camera jumped (a snap, the minimap): FSR 3 starts afresh rather than mix the frames before in.
    if (scene.cameraCut)
      if (render::World* w = picture.world()) w->cameraCut = true;
    // The icons on the resource deposits from far up (the renderer's World::depositIcons, its "resource icons"): the
    // main view's EnableResourceRendering (off, for one, while an operation's cinematic plays: FAF's gamemain.NISMode).
    if (render::World* w = picture.world()) w->depositIcons = scene.resourceIcons;
    // The player's own looks (the Options menu's Video tab), when they change.
    const LookSettings& looks = Looks();
    if (!lookApplied || looks.remasteredEffects != lookNow.remasteredEffects) picture.SetRemasteredEffects(looks.remasteredEffects);
    if (!lookApplied || looks.effectsQuality != lookNow.effectsQuality) picture.SetEffectsQuality(looks.effectsQuality);
    if (!lookApplied || looks.upscale != lookNow.upscale) picture.SetUpscale(looks.upscale);
    if (!lookApplied || looks.temporalUpscale != lookNow.temporalUpscale) picture.SetTemporalUpscale(looks.temporalUpscale);
    // (the lighting upgrade's switches, the renderer's request 31)
    if (!lookApplied || looks.atmosphere != lookNow.atmosphere) picture.SetAtmosphere(looks.atmosphere);
    if (!lookApplied || looks.softShadows != lookNow.softShadows) picture.SetSoftShadows(looks.softShadows);
    if (!lookApplied || looks.rayTracing != lookNow.rayTracing) picture.SetRayTracing(looks.rayTracing);
    if (!lookApplied || looks.weatherEffects != lookNow.weatherEffects) picture.SetWeatherEffects(looks.weatherEffects);
    if (!lookApplied || looks.groundDetail != lookNow.groundDetail) picture.SetGroundDetail(looks.groundDetail);  // (request 33)
    if (!lookApplied || looks.frameGeneration != lookNow.frameGeneration) picture.SetFrameGeneration(looks.frameGeneration);  // (34)
    ApplyLens();  // (Cinematic Lens Blur: only while the cinematic camera shows)
    if (render::World* w = picture.world()) WorldLooks(*w, looks, !lookApplied || looks.photoLook != lookNow.photoLook);
    if (!lookApplied || looks.hiddenOutlines != lookNow.hiddenOutlines) picture.SetHiddenOutlines(looks.hiddenOutlines);
    // (the minimap stamps the script's meshes while cam_DefaultMiniLOD is not 0: render 4a061d2)
    if (!lookApplied || (looks.minimapLod != 0) != (lookNow.minimapLod != 0)) picture.SetMinimapMeshes(looks.minimapLod != 0);
    // The cinematics' switches (UI_NisRenderIcons, ren_SelectBoxes, ren_Oblivion).
    if (!lookApplied || looks.strategicIcons != lookNow.strategicIcons) picture.SetStrategicIcons(looks.strategicIcons);
    ApplyCineMarks();  // (the selection's marks, ren_SelectBoxes, and the life bars and work bars: none while the cinematic camera shows)
    if (!lookApplied || looks.worldShown != lookNow.worldShown) picture.SetWorldShown(looks.worldShown);
    lookNow = looks;
    lookApplied = true;
    if (session && session->sim_) picture.SetAllies(session->allies_);  // (as of the last beat: SetScene may come while one runs)
  }
  // The minimap's picture with the deposits' icons when the minimap view's switch (EnableResourceRendering) says so:
  // the renderer lays them per call under the units' icons, as the original's RenderResources 0x862a80 (render
  // a8ac1f0, ui REQUESTS 32); off is the original's default (FAF's minimap.lua: off unless the profile's
  // MiniMap_resource_icons).
  const uint32_t* DrawMinimap(int width, int height, bool resourceIcons) override {
    const std::vector<uint32_t>& pixels = picture.DrawMinimap(width, height, resourceIcons);
    return pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) ? pixels.data() : nullptr;
  }
  bool MapSize(float* width, float* height) override {
    picture.MapSize(width, height);
    return *width > 0 && *height > 0;
  }
  bool MinimapProject(const float world[3], float* x, float* y) override {
    return picture.MinimapProject({world[0], world[1], world[2]}, x, y);
  }
  bool MinimapGroundAt(float x, float y, float world[3]) override {
    render::Vec3 hit;
    if (!picture.MinimapGroundAt(x, y, &hit)) return false;
    world[0] = hit.x, world[1] = hit.y, world[2] = hit.z;
    return true;
  }
#else
  void SetScene(const WorldScene&) override {}
  const uint32_t* DrawMinimap(int, int, bool) override { return nullptr; }
  bool MapSize(float*, float*) override { return false; }
  bool MinimapProject(const float*, float*, float*) override { return false; }
  bool MinimapGroundAt(float, float, float*) override { return false; }
  const uint32_t* Draw(const WorldCamera&, int, int) override { return nullptr; }
  bool Project(const float*, float*, float*) override { return false; }
  bool GroundAt(float, float, float*) override { return false; }
  uint32_t UnitAt(float, float) override { return 0xffffffffu; }
  void UnitsIn(float, float, float, float, std::vector<uint32_t>*) override {}
#endif
};

#ifdef MOHO64UI_WORLD_PICTURE
namespace {
// The interface layer to the renderer: only its changed part, but all of it the first time and
// after a change of size (as SetInterfaceLayer asks).
void GiveLayer(render::WorldPicture& picture, const SoftwareTarget& target, const IntRect& changed, int* layerW, int* layerH) {
  const int w = target.Width(), h = target.Height();
  IntRect part = changed.Intersect(IntRect{0, 0, w, h});
  if (*layerW != w || *layerH != h) part = IntRect{0, 0, w, h}, *layerW = w, *layerH = h;
  if (part.empty()) part = IntRect{};
  picture.SetInterfaceLayer(target.pixels().data(), w, h, w, part.x0, part.y0, part.x1 - part.x0, part.y1 - part.y0);
}
// What a game or the menus before left set on the window's picture that is the picture's own, not its world's (the
// world's own lists begin empty with each swap: render-answers.md, request 37), put back as a picture starts:
// the army it is for, what that army knows, the colours, the day, the lens, the cinematics' switches. `looks`: the
// player's looks too, as a picture starts (the menus' background keeps the look it always had; a game's own
// looks are given again by its first frame, Picture::SetScene).
void StartAfresh(render::WorldPicture& p, bool looks) {
  p.SetFocusArmy(0);
  p.SetUnexplored(false);
  p.SetAllies({});
  p.SetLifeBarsOnEnemies(false);
  p.SetIconColors({});
  p.SetArmyColors({});
  p.SetDayAndWeather(-1, 0, false);
  p.SetLens(render::Lens{});
  p.ClearPhotoMoment();
  p.SetHover(0xffffffffu, 0);
  p.SetLifeBars(false);
  p.SetWorkBars(true);
  p.SetStrategicIcons(true);
  p.SetSelectionMarks(true);
  if (!looks) return;
  // (each as a picture starts: world_picture.h; the lighting bundles off are the world's own switches as at first;
  // while the picture holds looks for a map opening behind, the bundles' are only kept: WorldPicture::HoldLooks)
  p.SetRemasteredEffects(true);
  p.SetEffectsQuality(1);
  p.SetUpscale(1);
  p.SetTemporalUpscale(false);
  p.SetAtmosphere(false);
  p.SetSoftShadows(false);
  p.SetRayTracing(false);
  p.SetWeatherEffects(false);
  p.SetGroundDetail(false);
  p.SetFrameGeneration(false);
  p.SetHiddenOutlines(true);
  p.SetMinimapMeshes(false);
}
// A game's map opening behind let go of (dropped, failed, not ready): the looks held for it give way to the ones a
// picture starts with, laid on the world shown (the menus', which has them; or a game's being left).
void LetGoOfGameMap(render::WorldPicture& p) {
  p.CancelMap();
  StartAfresh(p, true);
  p.HoldLooks(false);
}
}  // namespace

// The window's one picture of the renderer's (the renderer's answer to ui request 37, docs/render-answers.md "To the
// interface, request 37"): made once on the window without a map (Open with none: the interface shows at once, on a
// cold shader cache too) and given every map behind the one it shows (BeginMap): the menus' background's, each
// game's, the menus' again; the window goes on showing frames while the next map opens, which is swapped in when
// ready (SwapMap; the renderer draws it unseen first, so the first frame after is an ordinary one). Before, each of
// those opened a picture of its own on the window on the main thread: a game's 1.2-2.4 s inside FinishLoad, the
// menus' about 0.9 s after a game (the last frame standing), the window hidden at the start until the menus' map
// was open.
struct GameSession::WindowPicture {
  render::WorldPicture picture;
  bool vulkan = false;  // what it was made with: the player's Graphics API, the scene's samples (gSceneSamples)
  int samples = 0;
  // The world shown, and the map opening behind (Menu or Game; None: none).
  enum class Kind { None, Menu, Game, Left };  // (Left: the world of a game that is over, not shown)
  Kind shows = Kind::None;
  std::string shownMap;
  Kind pending = Kind::None;
  std::string pendingMap;
  std::chrono::steady_clock::time_point begun{};
  std::string failedGameMap;  // (a game's map the picture could not open: not tried again for that launch)
  bool vsync = true;
  bool lowLatency = false;     // (as the renderer's picture starts: SetLowLatency)
  int layerW = 0, layerH = 0;  // the interface layer's size as last given (0: none yet)
  // The picture over the window drawn by the card (SetBackdrop): none, the interface's (CardSink: the loading
  // screen's film, the "IN TRANSIT" cover), or black fading out over the menus' map just swapped in (it comes in
  // from black, as the read-back way's background does, rather than at once after the seconds of black).
  enum class Over { None, Card, Fade };
  Over over = Over::None;
  float overAlpha = 1;
  std::chrono::steady_clock::time_point fadeFrom{};
  bool fading = false;
  uint32_t black = 0xff000000u;
};
#else
struct GameSession::WindowPicture {};
#endif

// The beats' own thread: one job at a time (a beat, or a launch's load), asked for and waited on
// from the main thread.
class GameSession::BeatWorker {
 public:
  BeatWorker() { StartThread(); }
  ~BeatWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    wake_.notify_all();
    JoinThread();
  }
  void Start(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = std::move(job);
      running_.store(true, std::memory_order_release);
    }
    wake_.notify_all();
  }
  bool Running() const { return running_.load(std::memory_order_acquire); }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return !running_.load(std::memory_order_acquire); });
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return quit_ || job_; });
        if (quit_) return;
        job = std::move(job_);
        job_ = nullptr;
      }
      job();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false, std::memory_order_release);
      }
      done_.notify_all();
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  std::function<void()> job_;
  std::atomic<bool> running_{false};
  bool quit_ = false;
#if defined(__APPLE__)
  // (macOS gives a new thread 512 KB of stack; the simulation's start and its beats run deep Lua:
  // the main thread's 8 MB here too)
  pthread_t thread_{};
  void StartThread() {
    void* (*run)(void*) = [](void* self) -> void* {
      static_cast<BeatWorker*>(self)->Run();
      return nullptr;
    };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    int made = pthread_create(&thread_, &attr, run, this);
    pthread_attr_destroy(&attr);
    if (made != 0) made = pthread_create(&thread_, nullptr, run, this);  // (the system's own stack size)
    if (made != 0) {
      Logf(LogLevel::Error, "moho64ui: the beats' thread could not be made (%d)", made);
      std::abort();
    }
  }
  void JoinThread() { pthread_join(thread_, nullptr); }
#else
  std::thread thread_;
  void StartThread() { thread_ = std::thread([this] { Run(); }); }
  void JoinThread() { thread_.join(); }
#endif
};

// --- the replay timeline (ours, not the original's: Chris's approval of 2026-10-23; user/replay_timeline.h) ---

namespace {
// A replay's length in beats: its stream's Advances (sim/command_stream.cpp: messages {u8 type, u16 length with the
// 3 bytes of the header, payload}; type 0 Advance, its payload the u32 count of beats, 1 in every replay seen), read
// as CommandStream::ApplyUntilAdvance reads them.
uint32_t ReplayBeats(const std::string& data, size_t at) {
  uint64_t beats = 0;
  while (at + 3 <= data.size()) {
    const int type = static_cast<unsigned char>(data[at]);
    size_t length = static_cast<unsigned char>(data[at + 1]) | (static_cast<size_t>(static_cast<unsigned char>(data[at + 2])) << 8);
    if (length < 3) length = 3;
    if (at + length > data.size()) break;
    if (type == 0 && length >= 7) {
      uint32_t n = 0;
      std::memcpy(&n, data.data() + at + 3, 4);
      beats += n;
    }
    at += length;
  }
  return beats > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(beats);
}
}  // namespace

// The timeline's host side for one replay: its length, the moments' scan on its own thread (the engine's
// ScanReplayMoments, sim/moments.h: kept across a jump back, which starts the same replay over), and the jumps.
struct GameSession::Timeline : ReplayTimelineSource {
  GameSession* session = nullptr;
  mutable uint32_t ticks = 0;  // (the stream's beats; once the scan has played it to its end, the tick it ended at)
  moho::Moments found;  // (the scan's: filled on its thread, under its lock)
  std::atomic<int> scanned{0};
  std::atomic<bool> cancel{false}, scanning{false};
  std::thread scan;
  std::string kept;  // (what the interface keeps across a jump back: TakeKept)
  // This thread's copy of what the scan found (Moments::CopySince, the new part at each look): the moments, every
  // list of fight buckets (fights[army], the last the hits without an army), and whether the last copy was the
  // final one (the scan done: its last bucket summed to the end).
  mutable std::vector<moho::Moment> moments;
  mutable std::vector<std::vector<moho::FightBucket>> buckets;
  mutable bool finalCopy = false;
  int limit = 0;  // (the scan's ScanArgs::ticks: 0 the whole replay)

  ~Timeline() { Stop(); }
  void Stop() {
    cancel = true;
    if (scan.joinable()) scan.join();
  }
  uint32_t TickNow() const {
    const GameSession& g = *session;
    if (!g.sim_) return 0;
    return g.BeatRunning() ? g.beatFrom_ : g.sim_->tick();  // (a beat on its thread: the one it started from)
  }
  // What is new since the last look. While the scan runs its last bucket is still being summed: that one is asked
  // for again each time (CopySince from its index), until a copy made after the scan's end.
  void Refresh() const {
    if (finalCopy) return;
    size_t from = buckets.empty() ? 0 : buckets[0].size();
    if (from > 0) --from;
    for (auto& list : buckets) list.resize(std::min(list.size(), from));
    const bool ended = !scanning;  // (read before the copy: a scan that has ended has appended all it will)
    bool finished = false;
    int last = 0;
    found.CopySince(moments.size(), from, &moments, &buckets, &last, &finished);
    finalCopy = finished || ended;
    // The replay played to its end: its length is the game tick it ended at, the stream's beats less the paused ones
    // (a paused beat leaves Sim::tick() as it is), which the seeks go by.
    if (finished && last > 0 && (limit == 0 || last < limit) && static_cast<uint32_t>(last) != ticks) {
      Logf(LogLevel::Info, "moho64ui: the replay ends at game tick %d (%u beats in its stream)", last, ticks);
      ticks = static_cast<uint32_t>(last);
    }
  }
  size_t DoneBuckets() const {  // (the buckets summed to their end: the last one only once the scan is done)
    size_t n = buckets.empty() ? 0 : buckets[0].size();
    for (const auto& list : buckets) n = std::min(n, list.size());
    return finalCopy || n == 0 ? n : n - 1;
  }
  State Now() const override {
    Refresh();
    State s;
    s.ticks = ticks;
    s.tick = TickNow();
    s.seekTo = session->seekTo_;
    s.scanning = scanning;
    s.scanned = static_cast<uint32_t>(scanned.load());
    s.moments = moments.size();
    s.buckets = DoneBuckets();
    s.kept = !kept.empty();
    return s;
  }
  void MomentsFrom(size_t first, std::vector<ReplayTimelineSource::Moment>* out) const override {
    out->clear();
    for (size_t i = first; i < moments.size(); ++i) {
      const moho::Moment& f = moments[i];
      ReplayTimelineSource::Moment m;
      m.tick = f.tick;
      m.kind = MomentKindName(f.kind);
      m.army = f.army;
      m.pos[0] = f.pos.x, m.pos[1] = f.pos.y, m.pos[2] = f.pos.z;
      m.unit = f.unit;
      m.entity = f.entity;
      out->push_back(std::move(m));
    }
  }
  // Every army's fighting in each 5 s summed, with the hits that have no army (fights[armies]).
  void FightFrom(size_t first, std::vector<Fight>* out) const override {
    out->clear();
    const size_t n = DoneBuckets();
    for (size_t i = first; i < n; ++i) {
      Fight f;
      f.tick0 = buckets[0][i].tick0;
      for (const auto& list : buckets) {
        f.damage += list[i].damage;
        f.hits += list[i].hits;
        f.deaths += list[i].deaths;
      }
      out->push_back(f);
    }
  }
  bool Seek(uint32_t tick, const std::string& keep, std::string* why) override {
    GameSession& g = *session;
    if (!g.sim_ || !g.play_ || g.Loading()) {
      *why = "no replay plays";
      return false;
    }
    if (ticks == 0) {
      *why = "the replay has no beats";
      return false;
    }
    tick = std::max(1u, std::min(tick, ticks));
    const uint32_t now = TickNow();
    if (tick == now) return true;
    if (tick > now) {  // forward: the beats run ahead (Advance, CatchingUp)
      g.seekTo_ = tick;
      Logf(LogLevel::Info, "moho64ui: the replay timeline: forward from beat %u to beat %u", now, tick);
      return true;
    }
    // back: the replay starts over (the host's restart between two frames, RestartSession's: Relaunch) and runs
    // ahead to the beat
    g.seekTo_ = tick;
    g.seekBack_ = true;  // (not reached by this run of the replay: the next one runs ahead to it)
    kept = keep;
    RequestSessionRestart();
    Logf(LogLevel::Info, "moho64ui: the replay timeline: back from beat %u to beat %u (the replay starts over)", now, tick);
    return true;
  }
  std::string TakeKept() override { return std::move(kept); }
};

ReplayTimelineSource* GameSession::timeline() { return play_ && timeline_ ? timeline_.get() : nullptr; }

// The replay's length, and its moments looked for from now on: the engine's ScanReplayMoments (sim/moments.h) on a
// thread of its own, playing the replay a second time in a Sim of its own, mounted as this program mounted the game
// (scanInit, scanFolders, replayMaps), with the replay's mods and one worker. It goes at some 20 times the game's
// speed for a 1v1 and some 7 for a 4v4 (engine, 2026-10-23): the bar fills over minutes. scanTicks: it stops there
// (tests).
void GameSession::StartMomentScan(const ReplayHeader& header, const std::string& file, const std::vector<std::string>& uids) {
  StopMomentScan();
  timeline_ = std::make_unique<Timeline>();
  Timeline& t = *timeline_;
  t.session = this;
  t.ticks = ReplayBeats(playData_, header.streamOffset);
  Logf(LogLevel::Info, "moho64ui: the replay is %u beats long (%u:%02u); its moments are looked for", t.ticks, t.ticks / 600,
       (t.ticks / 10) % 60);
  moho::ScanArgs args;
  args.init = scanInit;
  args.folders = scanFolders;
  args.maps = replayMaps;
  args.modUids = uids;
  args.replay = file;
  args.threads = 1;  // (the cores stay the watched game's)
  args.ticks = scanTicks;
  t.limit = scanTicks;
  t.scanning = true;
  t.scan = std::thread([&t, args] {
    const auto t0 = std::chrono::steady_clock::now();
    const bool through = moho::ScanReplayMoments(args, &t.found, &t.scanned, &t.cancel);
    size_t moments = 0;
    int last = 0;
    std::string error;
    {
      std::lock_guard<std::mutex> hold(t.found.mutex);
      moments = t.found.moments.size();
      last = t.found.lastTick;
      error = t.found.error;
    }
    t.scanning = false;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!error.empty())
      Logf(LogLevel::Warning, "moho64ui: the replay's moments could not be looked for: %s", error.c_str());
    else
      Logf(LogLevel::Info, "moho64ui: the replay's moments: %s at beat %d, %zu found in %.1f s (%.1f times the game's speed)",
           through ? "scanned" : "the scan stopped", last, moments, secs, secs > 0 ? last / 10.0 / secs : 0.0);
  });
}

void GameSession::StopMomentScan() {
  if (timeline_) timeline_->Stop();
  timeline_.reset();
}

void GameSession::SeekReached() {
  if (!seekTo_ || seekBack_ || !sim_ || BeatRunning()) return;
  if (sim_->tick() < seekTo_ && !(play_ && play_->AtEnd())) return;
  Logf(LogLevel::Info, "moho64ui: the replay timeline: at beat %u", sim_->tick());
  seekTo_ = 0;
}

GameSession::GameSession(Vfs* vfs, std::vector<std::string> hookDirs) : vfs_(vfs), hookDirs_(std::move(hookDirs)) {}
GameSession::~GameSession() {
  if (worker_) worker_->Wait();
  StopMomentScan();
}

WorldSource* GameSession::world() const { return picture_.get(); }

bool GameSession::BeatRunning() const { return worker_ && worker_->Running(); }

// The moment the picture shows: the simulation's tick and the part of the next beat that has passed
// (while a beat runs on its thread: the tick it will reach).
double GameSession::PictureTick() const {
  if (!sim_) return 0.0;
  const double tick = BeatRunning() ? static_cast<double>(beatFrom_) + 1.0 : static_cast<double>(sim_->tick());
  return tick + due_ / 0.1;
}

namespace {
// The player's profile name (the game's own prefs.lua: GetCurrentProfile().Name), "" for none.
std::string ProfileName(lua_State* L) {
  const int top = lua_gettop(L);
  std::string name;
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/user/prefs.lua");
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_getfield(L, -1, "GetCurrentProfile");
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_getfield(L, -1, "Name");
      if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return name;
}
// (UTF-8 text and paths both ways; std::filesystem's own u8 forms are char8_t under C++20)
// The session's mods from a list of mod tables (GameMods, scenarioMods) at `list`: their uids, in order.
std::vector<std::string> ModUids(lua_State* L, int list) {
  std::vector<std::string> uids;
  for (int i = 1; lua_istable(L, list); ++i) {
    lua_rawgeti(L, list, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_getfield(L, -1, "uid");
    if (lua_isstring(L, -1)) uids.push_back(lua_tostring(L, -1));
    lua_pop(L, 2);
  }
  return uids;
}
// Whether the player has the debug facilities on (the preference debug.enable_debug_facilities:
// the original's USER_DebugFacilitiesEnabled 0x8cae20, which gives a single-player session its cheats).
bool DebugFacilities(lua_State* L) {
  const int top = lua_gettop(L);
  bool on = false;
  lua_getglobal(L, "GetPreference");
  lua_pushstring(L, "debug.enable_debug_facilities");
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0) on = lua_toboolean(L, -1) != 0;
  lua_settop(L, top);
  return on;
}
std::filesystem::path PathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string TextOf(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::FILE* OpenForWriting(const std::filesystem::path& file) {
#ifdef _WIN32
  return _wfopen(file.wstring().c_str(), L"wb");
#else
  return std::fopen(TextOf(file).c_str(), "wb");
#endif
}
}  // namespace

bool GameSession::Launch(lua_State* L, int config, std::string* why) {
  if (config < 0) config = lua_gettop(L) + config + 1;
  if (sim_ || Loading()) {
    *why = "a session is running already";
    return false;
  }
  auto t0 = std::chrono::steady_clock::now();
  SessionDescription session;
  LaunchParams params;
  params.seed = seedGiven ? seed : static_cast<uint32_t>(t0.time_since_epoch().count());
  if (!SessionFromGameConfig(L, config, params, &session, why)) return false;

  // The session's mods are the configuration's GameMods, by uid, in order.
  lua_getfield(L, config, "GameMods");
  const std::vector<std::string> uids = ModUids(L, lua_gettop(L));
  lua_pop(L, 1);

  // The day and the weather: the game's options where it has them (the lobby's Remaster...
  // options), the host's own otherwise.
  skyNow_ = sky;
  seed_ = session.header.seed;
  lua_getfield(L, config, "GameOptions");
  if (lua_istable(L, -1)) ReadLookOptions(L, lua_gettop(L));
  lua_pop(L, 1);
  profile_ = ProfileName(L);
  playData_.clear();
  play_.reset();
  PrepareSession(session, uids);
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = false;
  return true;
}


bool GameSession::LaunchSinglePlayer(lua_State* L, int info, std::string* why) {
  if (info < 0) info = lua_gettop(L) + info + 1;
  if (sim_ || Loading()) {
    *why = "Can't launch a session while one is already launching or running.";  // (the original's words, 0xe4a3c8)
    return false;
  }
  SessionDescription session;
  SinglePlayerParams params;
  params.seed = seedGiven ? seed : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  params.cheats = DebugFacilities(L);
  if (!SessionFromSinglePlayerInfo(L, info, params, &session, why)) return false;
  lua_getfield(L, info, "scenarioMods");
  const std::vector<std::string> uids = ModUids(L, lua_gettop(L));
  lua_pop(L, 1);
  skyNow_ = sky;
  seed_ = session.header.seed;
  lua_getfield(L, info, "scenarioInfo");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "Options");
    if (lua_istable(L, -1)) ReadLookOptions(L, lua_gettop(L));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  profile_ = ProfileName(L);
  playData_.clear();
  play_.reset();
  PrepareSession(session, uids);
  Logf(LogLevel::Info, "moho64ui: a single-player session: %s, %zu armies%s", session.header.map.c_str(), session.header.armies.size(),
       params.cheats ? ", cheats on (the debug facilities)" : "");
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = false;
  return true;
}

void GameSession::ReadLookOptions(lua_State* L, int options) {
  auto option = [&](const char* key) -> std::string {
    lua_getfield(L, options, key);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
  };
  const std::string start = option("RemasterDayStart"), length = option("RemasterDayLength"), weather = option("RemasterWeather");
  if (start == "map") skyNow_.startHour = -1;
  else if (start == "dawn") skyNow_.startHour = 6.5f;   // (the renderer: at 6.0 the land is nearly as dark as at night)
  else if (start == "morning") skyNow_.startHour = 9.0f;
  else if (start == "noon") skyNow_.startHour = 12.0f;
  else if (start == "afternoon") skyNow_.startHour = 15.0f;
  else if (start == "dusk") skyNow_.startHour = 17.5f;  // (likewise 18.0)
  else if (start == "night") skyNow_.startHour = 0.0f;
  else if (start == "random") skyNow_.startHour = static_cast<float>(seed_ % 96u) * 0.25f;  // every player draws the same
  if (length == "still") skyNow_.dayLengthSeconds = 0;
  else if (!length.empty() && std::atof(length.c_str()) > 0) skyNow_.dayLengthSeconds = static_cast<float>(std::atof(length.c_str()) * 60.0);  // minutes
  if (!weather.empty()) skyNow_.weather = weather;
  const std::string edge = option("RemasterMapEdge");
  if (edge == "frame" || edge == "beyond") skyNow_.edge = edge;
}

bool GameSession::LaunchReplay(lua_State* L, const std::string& file, std::string* why) {
  if (sim_ || Loading()) {
    *why = "a session is running already";
    return false;
  }
  auto data = hostfs::ReadFile(file);
  if (!data) {
    *why = "cannot read " + file;
    return false;
  }
  auto h = ReadReplayHeader(*data, why);
  if (!h) return false;
  SessionDescription session;
  session.header = *h;
  session.replay = true;     // (the simulation's SessionIsReplay)
  session.focusArmy = -1;    // the player watches: no army of his (INFERRED: the original's start)
  session.adjustableSpeed = true;
  // The mods are the header's (GameMods as the launch had them: a list of tables with a uid).
  std::vector<std::string> uids;
  const int top = lua_gettop(L);
  if (PushSerializedLua(L, h->mods) && lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, top + 1, i);
      if (!lua_istable(L, -1)) break;
      lua_getfield(L, -1, "uid");
      if (lua_isstring(L, -1)) uids.push_back(lua_tostring(L, -1));
      lua_pop(L, 2);
    }
  }
  lua_settop(L, top);
  // The look of the game: the scenario's Options, as the lobby gave them.
  skyNow_ = sky;
  seed_ = h->seed;
  if (PushSerializedLua(L, h->scenario) && lua_istable(L, -1)) {
    lua_getfield(L, -1, "Options");
    if (lua_istable(L, -1)) {
      ReadLookOptions(L, lua_gettop(L));
      // A game played here with the engine's script fixes is played back with them (the mark
      // StartReplayFile writes; ui request engine 23). Without the mark (the original's
      // recordings) the engine's own rule stands: no fixes for a replay.
      lua_getfield(L, -1, "RemasterFixes");
      if (lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "on") session.fixes = 1;
    }
  }
  lua_settop(L, top);
  profile_ = ProfileName(L);
  playData_ = std::move(*data);
  play_.reset();
  PrepareSession(session, uids);
  play_ = std::make_unique<CommandStream>(playData_, h->streamOffset);
  ReplayHeld() = false;  // (a new replay plays: no hold of a replay watched before)
  seekTo_ = 0;
  seekBack_ = false;
  StartMomentScan(*h, file, uids);
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = true;
  Logf(LogLevel::Info, "moho64ui: playing the replay %s (%zu bytes, %zu armies)", file.c_str(), playData_.size(), h->armies.size());
  return true;
}

// The replay file of a game being played (LastGame), the original's format (the header as the
// engine's ReadReplayHeader reads it, sim/replay.cpp, then the command stream): so the game's
// Replay dialog lists it and the original game could open it too.
void GameSession::StartReplayFile(const SessionDescription& session) {
  if (replay_) std::fclose(replay_), replay_ = nullptr;
  if (!session.createReplay && !session.replay) currentReplay.clear();  // (a campaign's operation: none, as the original)
  if (session.replay || !session.createReplay || userDir.empty() || profile_.empty()) return;
  std::error_code ec;
  const std::filesystem::path folder = PathOf(userDir) / "replays" / PathOf(profile_);
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path file = folder / "LastGame.SCFAReplay";
  replay_ = OpenForWriting(file);
  if (!replay_) {
    Logf(LogLevel::Warning, "moho64ui: cannot write the replay %s", TextOf(file).c_str());
    return;
  }
  currentReplay = TextOf(file);
  const ReplayHeader& h = session.header;
  std::string out;
  auto cstr = [&](const std::string& s) { out += s, out.push_back('\0'); };
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  };
  auto blob = [&](const std::string& s) { u32(static_cast<uint32_t>(s.size())), out += s; };
  cstr(h.version.empty() ? "Supreme Commander v1.50.3839" : h.version);
  cstr("\r\n");
  cstr((h.replayVersion.empty() ? std::string("Replay v1.9") : h.replayVersion) + "\r\n" + h.map);
  cstr("\r\n\x1a");
  blob(h.mods);
  // (A game played with the engine's script fixes says so in its scenario's Options,
  // RemasterFixes = "on", so that its replay is played with them too; the original ignores it.
  // The table is made in a scratch Lua state, never the sim's: an object made there outside the
  // game's own flow would shift the sim's object serials, and with them its pairs() order.)
  std::string scenario = h.scenario;
  if (sim_ && sim_->fixes) {
    lua_State* S = lua_open();
    const int top = lua_gettop(S);
    if (PushSerializedLua(S, h.scenario) && lua_istable(S, -1)) {
      lua_pushstring(S, "Options");
      lua_rawget(S, top + 1);
      if (lua_istable(S, -1)) {
        lua_pushstring(S, "RemasterFixes");
        lua_pushstring(S, "on");
        lua_rawset(S, -3);
        scenario.clear();  // (SerializeLua appends)
        SerializeLua(S, top + 1, &scenario);
      }
    }
    lua_close(S);
  }
  blob(scenario);
  out.push_back(static_cast<char>(h.sources.size()));
  for (const ReplayHeader::Source& s : h.sources) cstr(s.name), u32(static_cast<uint32_t>(s.pausesAllowed));
  out.push_back(h.cheats ? 1 : 0);
  out.push_back(static_cast<char>(h.armies.size()));
  for (const ReplayHeader::Army& a : h.armies) {
    blob(a.options);
    for (int s : a.sources) out.push_back(static_cast<char>(s));
    out.push_back(static_cast<char>(0xff));
  }
  u32(h.seed);
  std::fwrite(out.data(), 1, out.size(), replay_);
  std::fflush(replay_);
  Logf(LogLevel::Info, "moho64ui: this game's replay is %s", currentReplay.c_str());
}

void GameSession::Record(const std::string& bytes) {
  if (bytes.empty()) return;
  if (replay_) {
    std::fwrite(bytes.data(), 1, bytes.size(), replay_);
    std::fflush(replay_);
  }
  if (!stream_) return;
  std::fwrite(bytes.data(), 1, bytes.size(), stream_);
  std::fflush(stream_);  // (a game that ends badly keeps what it had)
}

void GameSession::End() {
  if (worker_) worker_->Wait();  // (a beat on its thread ends first; its sounds and sync are dropped)
  beatToFinish_ = false;
  load_ = Load::None;  // (a launch still loading is dropped)
  loadSim_.reset();
  loadSession_.reset();
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  if (fp_) fp_->End(fpBeats_), fp_.reset();
  if (replay_) std::fclose(replay_), replay_ = nullptr;
  play_.reset();
  if (!relaunching_) {  // (the replay closes: its moments' scan stops, a jump under way with it)
    StopMomentScan();
    seekTo_ = 0;
    seekBack_ = false;
  }
  if (sounds) sounds->StopWorld();
  onSync = nullptr;
  takeOrders = nullptr;
  picture_.reset();
#ifdef MOHO64UI_WORLD_PICTURE
  // The window's picture stays: the game's world in it is not shown any more (the interface over black) until the
  // next map is swapped in (the menus' background's, or the game started over's); one still opening for a launch
  // that is dropped is let go of.
  if (windowPicture_) {
    using Kind = WindowPicture::Kind;
    WindowPicture& wp = *windowPicture_;
    if (wp.pending == Kind::Game) LetGoOfGameMap(wp.picture), wp.pending = Kind::None;  // (the menus' map, if it shows, keeps its looks)
    if (wp.shows == Kind::Game) wp.shows = Kind::Left;
  }
#endif
  sim_.reset();
  interfaceBlueprints_.reset();
  rules_.reset();
  pendingSync_.clear();
  setupSync_.clear();
  foreignLoops_.clear();
  heardLoops_.clear();
  due_ = 0;
  pace = GamePace();
}

bool GameSession::Relaunch(std::string* why) {
  if (!last_) {
    *why = "no game was launched";
    return false;
  }
  relaunching_ = lastWasReplay_;  // (a replay starting over keeps its timeline: its moments, a jump back under way)
  End();
  relaunching_ = false;
  SessionDescription again = *last_;
  if (lastWasReplay_) {  // a replay starts over from its first beat, with its own seed
    PrepareSession(again, lastUids_);
    play_ = std::make_unique<CommandStream>(playData_, again.header.streamOffset);
    seekBack_ = false;
    if (seekTo_) Logf(LogLevel::Info, "moho64ui: the replay starts over and runs ahead to beat %u", seekTo_);
    return true;
  }
  if (!seedGiven) again.header.seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  PrepareSession(again, lastUids_);
  return true;
}

// The simulation's print lines of a beat, to the game's console (the original: Sim::Printf 0x7462a0 keeps them,
// the beat's sync carries them, the interface's side prints each with CON_Printf 0x895260; engine: Sim::TakePrints,
// ui REQUESTS 41). Posted: they reach the console's receivers on the main thread's next frame.
static void PostPrints(Sim& sim) {
  for (const std::string& line : sim.TakePrints()) ConsolePost(line);
}

// The launch call's part (the original's WLD_SetupSessionInfo + WLD_BeginSession, 0x88cbc0 /
// 0x88d060): the session is described and its simulation made, nothing loaded yet.
void GameSession::PrepareSession(const SessionDescription& session, const std::vector<std::string>& uids) {
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session %s: %zu armies, %zu command sources, focus army %d, seed %u, %zu mods",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, uids.size());
  loadSession_ = std::make_unique<SessionDescription>(session);
  loadSim_ = std::make_unique<Sim>(vfs_, hookDirs_, uids);
  loadSim_->sessionMods = h.mods;
  loadUids_ = uids;
  loadOk_ = false;
  loadPrepared_ = false;
  loadWhy_.clear();
  load_ = Load::Due;
}

// The rules and the simulation's start on the worker thread (the original: the "Map loader"
// background task, func_WorldSessionUserLoad 0x885de0, and the Sim thread, ThreadCreateSim
// 0x73d260). Meanwhile this thread draws only the front end's frames: no Lua state, rules or
// blueprints are made here until FinishLoad (the engine's rule for work beside a running sim).
void GameSession::BeginLoad() {
  if (load_ != Load::Due) return;
  load_ = Load::Running;
  g_crashTick = 0;  // (a crash while it loads names no beat of the game before)
  if (!worker_) worker_ = std::make_unique<BeatWorker>();
  worker_->Start([this] {
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    Sim* sim = loadSim_.get();
    const auto t0 = std::chrono::steady_clock::now();
    if (!sim->LoadRules()) {
      loadWhy_ = "the game's rules (blueprints) did not load";
      return;
    }
    const auto t1 = std::chrono::steady_clock::now();
    sim->collectSync = true;
    sim->collectPrints = true;  // (the scripts' print, for the game's console: ui REQUESTS 41)
#ifdef MOHO64UI_WORLD_PICTURE
    sim->collectEffects = wantPicture;  // explosions, shots, smoke: the picture's, asked for before the first beat
    sim->collectProps = wantPicture;    // wreckage, and trees reclaimed, burnt or knocked over (the picture takes the list)
    // hits, impacts and health changes, for the looks that answer them (marks on units, water ripples, blast waves;
    // about 1% of a beat's time; the renderer's request of 2026-10-18: render/world_picture.h)
    sim->collectHits = sim->collectImpacts = sim->collectHealth = wantPicture;
    sim->collectSounds = sounds != nullptr;  // what the scripts play in the world, as a list per beat
    sim->collectShakes = true;               // and the camera shakes they ask for (PostCameraShake)
#endif
    sim->keepRules = true;  // (the rules state for the interface's copy: LoadInterfaceRules, ui REQUESTS 46)
    if (!sim->Start(*loadSession_)) {
      loadWhy_ = "the simulation did not start";
      return;
    }
    loadRulesSeconds_ = secs(t0, t1);
    loadStartSeconds_ = secs(t1, std::chrono::steady_clock::now());
    loadOk_ = true;
  });
  // Meanwhile the game's map opens behind the window's picture (render request 37), the loading screen drawn over
  // what it shows; FinishLoad swaps it in.
#ifdef MOHO64UI_WORLD_PICTURE
  if (windowPicture_) windowPicture_->failedGameMap.clear();
#endif
  if (wantPicture && window) BeginWindowMap(true, loadSession_->header.map);
}

std::string GameSession::LoadingMapFolder() const {
  return loadSession_ ? SessionMapFolder(loadSession_->header) : std::string();  // (the engine's: sim/session.h)
}

// The renderer's request-39 follow-up (render/world_picture.h, PrepareSession): once the loading thread is through and
// before the session's first beat, the map opening behind gets the simulation, so that the models of the units there
// at the start and of what they may build, and the props the simulation made, are made on its thread while the
// loading screen still shows (MapReady, and so LoadDone, waits for them), not in the game's first frames (78 ms in a
// skirmish's second frame at 4K). Its rule: a picture handed a session is beaten only once swapped in (a map let go of
// is never beaten: LetGoOfGameMap).
void GameSession::ServeLoad() {
#ifdef MOHO64UI_WORLD_PICTURE
  if (load_ != Load::Running || loadPrepared_ || BeatRunning() || !loadOk_ || !loadSim_) return;
  if (!windowPicture_ || windowPicture_->pending != WindowPicture::Kind::Game) return;
  loadPrepared_ = true;
  if (windowPicture_->picture.PrepareSession(*loadSim_))
    Logf(LogLevel::Info, "moho64ui: the game's map opening behind makes the simulation's models ahead");
#endif
}

bool GameSession::LoadDone() const {
  if (load_ != Load::Running || BeatRunning()) return false;
#ifdef MOHO64UI_WORLD_PICTURE
  // (and the game's map opened behind the window's picture: the loading screen shows until both are there)
  if (windowPicture_ && windowPicture_->pending == WindowPicture::Kind::Game && !windowPicture_->picture.MapReady() &&
      !windowPicture_->picture.MapFailed())
    return false;
#endif
  return true;
}

// The load is through (the original's last DoLoading frame, 0x88c000): the session runs.
bool GameSession::FinishLoad(std::string* why) {
  if (load_ == Load::Due) BeginLoad();
  if (load_ != Load::Running) {
    *why = "no launch is loading";
    return false;
  }
  WaitForWorker();
  load_ = Load::None;
  const std::unique_ptr<SessionDescription> described = std::move(loadSession_);
  if (!loadOk_) {
    *why = loadWhy_;
    loadSim_.reset();
    play_.reset();
    playData_.clear();
    return false;
  }
  const SessionDescription& session = *described;
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session started: rules %.2f s, simulation start-up %.2f s", loadRulesSeconds_, loadStartSeconds_);
  sim_ = std::move(loadSim_);
  const std::vector<std::string> uids = std::move(loadUids_);
  heightsChanges_ = ~0ull;
  CopyHeights();
  CopyAllies();
  setupSync_ = sim_->TakeSync();
  PostPrints(*sim_);  // (what the scripts printed while the session started: the original's setup sync)
  pendingSync_.clear();
  if (session.focusArmy >= 0 && session.focusArmy < static_cast<int>(h.armies.size())) commandSource_ = h.armies[session.focusArmy].source;
  modUids_ = uids;
  sessionMods_ = h.mods;
  map_ = h.map;
  armies_ = h.armies.size();
  seed_ = h.seed;
  due_ = 0;
  StartReplayFile(session);
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  if (!replayOut.empty()) {  // the game's record: see the header
    const std::string base = replayOut + (recorded_ ? "." + std::to_string(recorded_ + 1) : "");
    ++recorded_;
    stream_ = std::fopen((base + ".stream").c_str(), "wb");
    if (!stream_) Logf(LogLevel::Warning, "moho64ui: cannot write %s.stream", base.c_str());
    else Logf(LogLevel::Info, "moho64ui: this game is recorded in %s.stream (seed %u)", base.c_str(), h.seed);
    if (std::FILE* f = std::fopen((base + ".seed").c_str(), "wb")) {
      std::fprintf(f, "seed %u\nmap %s\narmies %zu\nfocus army %d\n", h.seed, h.map.c_str(), h.armies.size(), session.focusArmy + 1);
      std::fprintf(f, "build %s\n", kBuildCommit);  // (the commit this program was built from: the engine's playback picks its build by it)
      if (!replayTable.empty()) std::fprintf(f, "session %s\n", replayTable.c_str());
      for (const std::string& folder : replayMaps) std::fprintf(f, "maps %s\n", folder.c_str());
      std::fclose(f);
    }
    // (the engine's class opens its file from MOHO64_FINGERPRINT: set to this game's for that moment, the
    // variable's own value put back after)
    if (fp_) fp_->End(fpBeats_), fp_.reset();
    const char* before = std::getenv("MOHO64_FINGERPRINT");
    const std::string kept = before ? before : "";
    const std::string mine = base + ".fp";
#ifdef _WIN32
    _putenv_s("MOHO64_FINGERPRINT", mine.c_str());
    fp_ = std::shared_ptr<Fingerprint>(Fingerprint::FromEnvironment().release());
    _putenv_s("MOHO64_FINGERPRINT", kept.c_str());
#else
    setenv("MOHO64_FINGERPRINT", mine.c_str(), 1);
    fp_ = std::shared_ptr<Fingerprint>(Fingerprint::FromEnvironment().release());
    if (before) setenv("MOHO64_FINGERPRINT", kept.c_str(), 1);
    else unsetenv("MOHO64_FINGERPRINT");
#endif
    fpBeats_ = 0;
  }
  pace.rate = session.gameSpeed;
  pace.adjustable = session.adjustableSpeed;
  pictureDue_ = false;
  if (wantPicture) {
    // (A window that is still to come: the picture opens on it in its first frame,
    // OpenPictureIfDue.)
    if (waitForWindow && !window) {
      pictureDue_ = true;
    } else {
      const auto t0 = std::chrono::steady_clock::now();
      OpenPicture();
      LongFrames().Note("(start: the world's picture opened)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  }
  if (play_)
    Logf(LogLevel::Info, "moho64ui: the replay is played %s",
         sim_->fixes ? "with the engine's script fixes, as it was played" : "without the engine's script fixes");
  return true;
}

#ifdef MOHO64UI_WORLD_PICTURE
// The interface's full-window picture for the card (maui/world_source.h, CardBackdrop): taken in a frame's drawing,
// its texels copied (premultiplied, as the renderer takes them) only when it is another picture than the one held,
// and handed to the window's picture when the frame is shown (GameSession::GiveBackdrop).
struct GameSession::CardSink : CardBackdrop {
  GameSession* session = nullptr;
  bool taken = false, wanted = false;  // this frame's drawing took one; the frame being shown has one
  float takenAlpha = 1, wantedAlpha = 1;
  uint64_t heldSerial = 0;  // the picture held (its Image::serial and texels)
  IntRect heldSrc;
  std::vector<uint32_t> pixels;
  int width = 0, height = 0;
  bool fresh = false;  // held but not handed to the card yet
  int loggedW = 0, loggedH = 0;  // (the size last named in the log)
  bool Take(const Image& image, const IntRect& src, float alpha) override {
    if (!session->windowPicture_) return false;
    taken = true;
    takenAlpha = static_cast<float>(WithAlpha(0xff000000u, alpha) >> 24) / 255.0f;  // (the software way's steps)
    if (image.serial == heldSerial && src.x0 == heldSrc.x0 && src.y0 == heldSrc.y0 && src.x1 == heldSrc.x1 && src.y1 == heldSrc.y1 &&
        !pixels.empty())
      return true;
    heldSerial = image.serial, heldSrc = src;
    width = src.x1 - src.x0, height = src.y1 - src.y0;
    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
      const uint32_t* from = image.pixels.data() + static_cast<size_t>(src.y0 + y) * static_cast<size_t>(image.width) + src.x0;
      uint32_t* to = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
      for (int x = 0; x < width; ++x) {
        const uint32_t p = from[x], a = p >> 24;
        if (a == 255) {
          to[x] = p;
        } else {
          const uint32_t rb = ((p & 0x00ff00ffu) * a + 0x00800080u) >> 8 & 0x00ff00ffu, g = ((p & 0x0000ff00u) * a + 0x00008000u) >> 8 & 0x0000ff00u;
          to[x] = a << 24 | rb | g;
        }
      }
    }
    fresh = true;
    if (width != loggedW || height != loggedH)
      Logf(LogLevel::Info, "moho64ui: a picture over the whole window (%dx%d) is drawn by the graphics card, under the interface", width, height);
    loggedW = width, loggedH = height;
    return true;
  }
};

CardBackdrop* GameSession::cardBackdrop() {
  if (!card_) card_ = std::make_unique<CardSink>(), card_->session = this;
  return card_.get();
}

void GameSession::InterfaceRendered() {
  if (!card_) return;
  card_->wanted = card_->taken, card_->wantedAlpha = card_->takenAlpha;
  card_->taken = false;
}
#else
struct GameSession::CardSink {};
CardBackdrop* GameSession::cardBackdrop() { return nullptr; }
void GameSession::InterfaceRendered() {}
#endif

bool GameSession::DrawsToWindow() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return window != nullptr && windowPicture_ != nullptr;
#else
  return false;
#endif
}

#ifdef MOHO64UI_WORLD_PICTURE
namespace {
// What a picture showed in the window, read back by the renderer (RGBA bytes), saved as a PNG.
void SaveWindowCapture(const std::vector<uint8_t>& rgba, int w, int h, const std::string& path) {
  if (rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture was not read back");
    return;
  }
  std::vector<uint32_t> argb(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (size_t i = 0; i < argb.size(); ++i)
    argb[i] = 0xff000000u | static_cast<uint32_t>(rgba[i * 4]) << 16 | static_cast<uint32_t>(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2];
  SoftwareTarget shot(w, h);
  shot.DrawPixels(argb.data(), w, h, 0, 0);
  if (shot.SavePng(path)) Logf(LogLevel::Info, "moho64ui: the window's picture is saved as %s", path.c_str());
}
}  // namespace
#endif

#ifdef MOHO64UI_WORLD_PICTURE
namespace {
// The picture on the game's window: on Windows its HWND (UseWindow); elsewhere the SDL window's
// Vulkan surface (UseHostSurface, the renderer's answer to ui request 29).
void UseGameWindow(render::WorldPicture& picture, void* window) {
#ifdef _WIN32
  picture.UseWindow(window);
#else
  auto* w = static_cast<Window*>(window);
  int width = 0, height = 0;
  w->ClientSize(&width, &height);
  picture.UseHostSurface(w->SurfaceExtensions(), [w](void* instance) { return w->CreateSurface(instance); }, width, height);
#endif
}
}  // namespace
#endif

void GameSession::WaitForNextFrame() {
#ifdef MOHO64UI_WORLD_PICTURE
  // (the window's picture; with Low Latency off it returns at once)
  if (windowPicture_) windowPicture_->picture.WaitForNextFrame();
#endif
}

// The window's picture, made when there is none yet; with `remake` (a map is begun) made anew too when the player's
// Graphics API or anti-aliasing has changed and no game's world shows in it (those are taken when the device and a
// world's pipelines are made: gSceneSamples must not change while a world lives, render/world.h; as before, a change
// shows from the next map). False: no window, or it would not open (the interface is then shown the software way, as
// with --gdi).
bool GameSession::EnsureWindowPicture(bool remake) {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!window || windowPictureFailed_) return false;
  const bool vulkan = Looks().vulkan;
  const int samples = Looks().SceneSamples();
  if (windowPicture_) {
    const WindowPicture& now = *windowPicture_;
    if (!remake || (now.vulkan == vulkan && now.samples == samples) || now.shows == WindowPicture::Kind::Game) return true;
    Logf(LogLevel::Info, "moho64ui: the window's picture is made anew (the Graphics API or the anti-aliasing has changed)");
    windowPicture_.reset();
    if (card_) card_->fresh = !card_->pixels.empty();  // (the new picture has none of it)
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (render::gSceneSamples != samples) {  // (no world lives now: the menus' background's own picture ends too)
    if (releaseWorlds) releaseWorlds();
    render::gSceneSamples = samples;
  }
  auto made = std::make_unique<WindowPicture>();
  made->vulkan = vulkan, made->samples = samples;
  made->picture.SetVulkan(vulkan);                                  // (the player's Graphics API)
  made->picture.SetLowLatency(made->lowLatency = Pacing().lowLatency);  // (the Video tab's Low Latency, before Open)
  UseGameWindow(made->picture, window);
  std::string error;
  if (!made->picture.Open(vfs_, "", &error)) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture did not open (%s): the interface is shown without the renderer", error.c_str());
    windowPictureFailed_ = true;
    return false;
  }
  Looks().rayTracingSupported = made->picture.RayTracingSupported() ? 1 : 0;  // (the Video tab's Ray Tracing)
  windowPicture_ = std::move(made);
  Logf(LogLevel::Info, "moho64ui: the window's picture (no map yet) opened in %.2f s",
       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
#else
  return false;
#endif
}

// A map opened behind the one the window's picture shows: the menus' background's, or the game being loaded's. One
// at a time: another begun before is let go of.
void GameSession::BeginWindowMap(bool game, const std::string& map) {
#ifdef MOHO64UI_WORLD_PICTURE
  using Kind = WindowPicture::Kind;
  if (map.empty() || !EnsureWindowPicture(true)) return;
  WindowPicture& wp = *windowPicture_;
  const Kind kind = game ? Kind::Game : Kind::Menu;
  if (wp.pending == kind && wp.pendingMap == map) return;
  if (wp.pending == Kind::Game) LetGoOfGameMap(wp.picture);
  else if (wp.pending != Kind::None) wp.picture.CancelMap();
  wp.pending = Kind::None;
  render::WorldPicture& p = wp.picture;
  // What the map's opening takes from the picture (as Open did): the texture detail, the season, ray tracing's shaders.
  std::function<void(render::World&)> before;
  if (game) {
    // The player's looks and the game's sky, so that the map opens for them: the world's own switches on the loading
    // thread before it opens (BeginMap's `before`; the renderer: "use `before` for anything read at open, such as the
    // look"), the picture's own held (WorldPicture::HoldLooks, render request 39): the map opens and is warmed with the
    // lighting bundles and ray tracing's shaders, the world shown meanwhile (the menus', hidden behind the loading
    // screen) is left as it is (laid on it, they made what they ask for in one frame: 7.4 s at 1080p with every look
    // on), and SwapMap lays them on: the game's first frame no longer makes them (1.3-1.6 s before).
    const LookSettings looks = Looks();
    p.SetTextureDetail(looks.textureDetail);  // (the game's Texture Detail)
    p.SetSeason(looks.season == 1 ? render::Season::Autumn : render::Season::Summer);  // (the Video tab's Season)
    p.HoldLooks(true);
    PictureLooks(p, looks);  // (ray tracing too: the surfaces' shaders made for reflections as the map opens, render request 31)
    const Sky sky = skyNow_;
    before = [looks, sky](render::World& w) {
      WorldLooks(w, looks, true);
      SkyOnWorld(w, sky.edge, sky.startHour, sky.weather);
    };
  } else {
    // (the menus' background as it always was: the picture's looks as a picture starts, the summer, the player's
    // Texture Detail as the background's own picture had it: Backdrop::Run)
    StartAfresh(p, true);
    p.SetSeason(render::Season::Summer);
    p.SetTextureDetail(Looks().textureDetail);
  }
  const bool begun = p.BeginMap(map, before);
  // (the game's hour and weather for the warm frames: the sky, clouds and light shafts made for them while it opens,
  // not in the game's first frame; SetDayAndWeather follows after the swap as before: DressGamePicture)
  if (begun && game && skyNow_.startHour >= 0)
    p.SetLoadDayAndWeather(skyNow_.startHour, skyNow_.dayLengthSeconds, WeatherByItself(skyNow_.weather));
  if (!begun && game) StartAfresh(p, true), p.HoldLooks(false);  // (the held looks give way: LetGoOfGameMap)
  if (!begun) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture cannot open %s behind", map.c_str());
    if (!game) failedMenuMap_ = map;
    return;
  }
  wp.pending = kind, wp.pendingMap = map;
  wp.begun = std::chrono::steady_clock::now();
  Logf(LogLevel::Info, "moho64ui: the window's picture opens %s behind (%s)", map.c_str(), game ? "the game's map" : "the menus' background");
#else
  (void)game, (void)map;
#endif
}

// The window's picture at the program's start, before the window shows (its device, no map: the interface shows at
// once), and the menus' background's map begun behind it when the background asks for one.
void GameSession::OpenWindowPicture() {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!EnsureWindowPicture(false)) return;
  if (!picture_ && load_ == Load::None && menuBackdrop && menuBackdrop->Direct() && menuBackdrop->map() != failedMenuMap_)
    BeginWindowMap(false, menuBackdrop->map());
#endif
}

#ifdef MOHO64UI_WORLD_PICTURE
// Once a frame, before it is drawn: the map opening behind the window's picture looked after (one that failed let go
// of; the menus' one swapped in once ready: a game's waits for its simulation, FinishLoad); the menus' background's
// map begun when it asks for one the picture neither shows nor opens.
void GameSession::ServeWindowMap() {
  using Kind = WindowPicture::Kind;
  if (!picture_ && load_ == Load::None && menuBackdrop && menuBackdrop->Direct() && menuBackdrop->map() != failedMenuMap_) {
    const std::string& want = menuBackdrop->map();
    const WindowPicture& wp = *windowPicture_;
    if (!(wp.shows == Kind::Menu && wp.shownMap == want) && !(wp.pending == Kind::Menu && wp.pendingMap == want)) BeginWindowMap(false, want);
  }
  if (!windowPicture_) return;  // (made anew and failed)
  WindowPicture& wp = *windowPicture_;
  if (wp.pending == Kind::None) return;
  std::string error;
  if (wp.picture.MapFailed(&error)) {
    Logf(LogLevel::Warning, "moho64ui: %s cannot be shown (%s)", wp.pendingMap.c_str(), error.c_str());
    (wp.pending == Kind::Menu ? failedMenuMap_ : wp.failedGameMap) = wp.pendingMap;
    if (wp.pending == Kind::Game) LetGoOfGameMap(wp.picture);
    else wp.picture.CancelMap();
    wp.pending = Kind::None;
    return;
  }
  if (wp.pending != Kind::Menu || !wp.picture.MapReady() || !wp.picture.SwapMap()) return;
  wp.shows = Kind::Menu, wp.shownMap = wp.pendingMap;
  wp.pending = Kind::None;
  Backdrop::Dress(wp.picture);
  wp.fading = true, wp.fadeFrom = std::chrono::steady_clock::now();
  const render::WorldPicture::LoadNumbers n = wp.picture.LastLoadNumbers();
  Logf(LogLevel::Info,
       "moho64ui: the menu's background: %s, in the window's picture, ready after %.1f s (open %.1f s; the frames' share %.0f ms, at most %.1f ms in "
       "one; %d warm frames, the longest %.0f ms)",
       wp.shownMap.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - wp.begun).count(), n.openSeconds, n.serviceMs,
       n.longestServiceMs, n.warmFrames, n.warmLongestMs);
}

// The picture over the window the card draws this frame (WindowPicture::Over): the interface's when its drawing gave
// one (CardSink), else the menus' map coming in from black, else none.
void GameSession::GiveBackdrop(WindowPicture& wp) {
  using Over = WindowPicture::Over;
  render::WorldPicture& p = wp.picture;
  float alpha = 1;
  Over over = Over::None;
  if (card_ && card_->wanted && !card_->pixels.empty()) {
    over = Over::Card, alpha = card_->wantedAlpha;
  } else if (wp.fading) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - wp.fadeFrom).count() / 1.5;  // (as Backdrop::Show's)
    if (t >= 1) wp.fading = false;
    else over = Over::Fade, alpha = static_cast<float>(1.0 - t);
  }
  if (over == Over::Card && (card_->fresh || wp.over != Over::Card)) {
    p.SetBackdrop(card_->pixels.data(), card_->width, card_->height, card_->width, render::WorldPicture::BackdropFit::Stretch, true);
    card_->fresh = false;
  } else if (over == Over::Fade && wp.over != Over::Fade) {
    p.SetBackdrop(&wp.black, 1, 1, 1, render::WorldPicture::BackdropFit::Stretch, true);
  } else if (over == Over::None && wp.over != Over::None) {
    p.SetBackdrop(nullptr, 0, 0, 0, render::WorldPicture::BackdropFit::Stretch, true);
  }
  if (over != Over::None && (alpha != wp.overAlpha || wp.over != over)) p.SetBackdropLook(alpha), wp.overAlpha = alpha;
  wp.over = over;
}
#endif

void GameSession::PresentToWindow(const SoftwareTarget& target, const IntRect& changed) {
#ifdef MOHO64UI_WORLD_PICTURE
  using Kind = WindowPicture::Kind;
  if (!window || !EnsureWindowPicture(false)) return;
  ServeWindowMap();
  if (!windowPicture_) return;
  WindowPicture& wp = *windowPicture_;
  const int w = target.Width(), h = target.Height();
  // (The renderer unions the changed parts until its next frame.)
  GiveLayer(wp.picture, target, changed, &wp.layerW, &wp.layerH);
  const bool vsync = Pacing().verticalSync;
  if (vsync != wp.vsync) wp.picture.SetVsync(vsync), wp.vsync = vsync;
  const bool lowLatency = Pacing().lowLatency;
  if (lowLatency != wp.lowLatency) wp.picture.SetLowLatency(lowLatency), wp.lowLatency = lowLatency;
  GiveBackdrop(wp);
  std::vector<uint8_t> rgba;
  if (!captureTo_.empty()) wp.picture.CaptureNextWindowFrame(&rgba);
  const auto t0 = std::chrono::steady_clock::now();
  FrameTimes& times = Times();
  if (picture_ && picture_->direct) {  // a game: its world from the world view's camera
    Picture& pic = *picture_;
    const double moment = pic.momentFixed ? pic.moment : PictureTick();  // (the one the anchors were of)
    pic.momentFixed = false;
    if (pic.cameraW > 0) {
      wp.picture.DrawToWindow(pic.camera, w, h, moment);
      // (the first-start graphics check's measurement: the card's time of the newest frame it has finished)
      if (FrameCost().fakeMs < 0) FrameCost().Note(wp.picture.LastStats().gpuMs);
    }
    // (The picture shows moment - 1, between the beats round it: past the newest beat it stands still.)
    if (sim_) {
      const uint32_t newest = BeatRunning() ? beatFrom_ : sim_->tick();
      if (moment - 1.0 > static_cast<double>(newest) + 1e-6) ++times.heldPictures;
    }
    times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ++times.worldDraws;
  } else {
    // No game: the menus' background's map where its movie showed this frame (Backdrop::TakeView); else black under
    // the interface (the loading screen, the lobby, a game's world that is over while the menus' map opens).
    render::Camera camera;
    double tick = 0;
    // ([views] a game's split screen or follow camera gone with its game: one view again, also before its world goes)
    if (wp.picture.ViewCount() > 1) wp.picture.SetViews({});
    const bool world = wp.shows == Kind::Menu && menuBackdrop && menuBackdrop->TakeView(wp.picture, w, h, &camera, &tick);
    if (wp.shows != Kind::None) wp.picture.SetWorldShown(world);
    wp.picture.DrawToWindow(camera, w, h, tick);
    if (world) {
      times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      ++times.worldDraws;
    }
  }
  if (!captureTo_.empty()) SaveWindowCapture(rgba, w, h, captureTo_), captureTo_.clear();
#else
  (void)target;
  (void)changed;
#endif
}

void GameSession::CaptureWindow(const std::string& path) {
#ifdef MOHO64UI_WORLD_PICTURE
  captureTo_ = path;
#else
  (void)path;
#endif
}

void GameSession::OpenPictureIfDue() {
  if (!pictureDue_ || !window || !sim_) return;
#ifdef MOHO64UI_WORLD_PICTURE
  // (the window's picture opening the game's map behind: swapped in once ready, with no beat running)
  if (windowPicture_ && windowPicture_->pending == WindowPicture::Kind::Game && windowPicture_->pendingMap == map_ &&
      (!windowPicture_->picture.MapReady() || BeatRunning()))
    return;
#endif
  pictureDue_ = false;
  WaitForWorker();  // (GameStarted reads the simulation)
  OpenPicture();
}

#ifdef MOHO64UI_WORLD_PICTURE
// A game's picture made ready: the edge of the map, the day and the weather, the simulation's start, the army it is for.
void GameSession::DressGamePicture(Picture& pic) {
  pic.picture.GameStarted(*sim_);
  // The picture is the player's: other armies' units show as the player's army knows them.
  pic.picture.SetFocusArmy(sim_->focusArmy + 1);
  pictureFocus_ = sim_->focusArmy;
  if (render::World* w = pic.picture.world()) SkyOnWorld(*w, skyNow_.edge, skyNow_.startHour, skyNow_.weather);
  Logf(LogLevel::Info, "moho64ui: session: the map's edge: %s",
       skyNow_.edge == "frame" ? "the original's frame of machinery" : "no machinery, the land beyond");
  if (skyNow_.startHour >= 0) {
    pic.picture.SetDayAndWeather(skyNow_.startHour, skyNow_.dayLengthSeconds, WeatherByItself(skyNow_.weather));
    Logf(LogLevel::Info, "moho64ui: session: the day starts at %.2f h, a day takes %.0f game seconds, weather \"%s\"",
         skyNow_.startHour, skyNow_.dayLengthSeconds, skyNow_.weather.c_str());
  }
  pic.picture.Beat(*sim_);
}
#endif

void GameSession::OpenPicture() {
#ifdef MOHO64UI_WORLD_PICTURE
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  const auto t3 = std::chrono::steady_clock::now();
  if (windowPicture_) {
    // On the game's window: its picture shows the game's map, opened behind while the loading screen showed
    // (BeginLoad; LoadDone waited for it). The renderer shows it there itself, the interface laid over it, without
    // reading it back (PresentToWindow).
    using Kind = WindowPicture::Kind;
    WindowPicture& wp = *windowPicture_;
    if (wp.pending != Kind::Game || wp.pendingMap != map_) {  // (not begun: a game loaded before the window was there)
      if (wp.failedGameMap == map_) {
        Logf(LogLevel::Warning, "moho64ui: session: no picture of the world: its map could not be opened");
        return;
      }
      BeginWindowMap(true, map_);
      pictureDue_ = windowPicture_ && windowPicture_->pending == Kind::Game;
      return;
    }
    if (!wp.picture.MapReady() || !wp.picture.SwapMap()) {
      Logf(LogLevel::Warning, "moho64ui: session: no picture of the world: its map was not ready");
      LetGoOfGameMap(wp.picture);
      wp.pending = Kind::None;
      return;
    }
    wp.shows = Kind::Game, wp.shownMap = map_;
    wp.pending = Kind::None;
    wp.fading = false;
    auto pic = std::make_unique<Picture>(&wp.picture);
    pic->session = this;
    pic->direct = true;
    StartAfresh(wp.picture, false);  // (the per-game settings: the menus' or the game before's go)
    wp.picture.SetWorldShown(true);  // (the loading screen had it off; the game's own switch comes with its first frame)
    DressGamePicture(*pic);
    picture_ = std::move(pic);
    const render::WorldPicture::LoadNumbers n = wp.picture.LastLoadNumbers();
    Logf(LogLevel::Info,
         "moho64ui: session: the world's picture is shown, %.3f s (its map opened behind the loading screen: ready %.1f s after it was begun, "
         "open %.1f s; the frames' share %.0f ms, at most %.1f ms in one; %d warm frames, the longest %.0f ms)",
         secs(t3, std::chrono::steady_clock::now()), n.readySeconds, n.openSeconds, n.serviceMs, n.longestServiceMs, n.warmFrames, n.warmLongestMs);
    return;
  }
  // Without the window's picture (a run without a window, or its picture would not open): a picture of the game's own,
  // read back each frame.
  auto pic = std::make_unique<Picture>(nullptr);
  pic->session = this;
  if (render::gSceneSamples != Looks().SceneSamples()) {  // (the player's anti-aliasing, or 1 under FSR 3: taken when a picture is made)
    if (releaseWorlds) releaseWorlds();
    render::gSceneSamples = Looks().SceneSamples();
  }
  pic->picture.SetVulkan(Looks().vulkan);  // (the player's Graphics API)
  pic->picture.SetTextureDetail(Looks().textureDetail);  // (the game's Texture Detail)
  pic->picture.SetLowLatency(Pacing().lowLatency);  // (the Video tab's Low Latency)
  pic->picture.SetSeason(Looks().season == 1 ? render::Season::Autumn : render::Season::Summer);  // (the Video tab's Season)
  // (Ray Tracing before Open: the surfaces' shaders are made for reflections when the map opens; the
  // renderer's request 31 follow-up. The first frame's looks block sets it again, harmlessly.)
  pic->picture.SetRayTracing(Looks().rayTracing);
  std::string error;
  if (pic->picture.Open(vfs_, map_, &error)) {
    Looks().rayTracingSupported = pic->picture.RayTracingSupported() ? 1 : 0;  // (the Video tab's Ray Tracing)
    DressGamePicture(*pic);
    picture_ = std::move(pic);
    Logf(LogLevel::Info, "moho64ui: session: the world's picture is open, %.2f s", secs(t3, std::chrono::steady_clock::now()));
  } else {
    Logf(LogLevel::Warning, "moho64ui: session: no picture of the world: %s", error.c_str());
  }
#endif
}

namespace {
// moho64ui_BlueprintFacts(id): the host's own function for tests (not the original's). What the interface's blueprints
// and the simulation's hold of one blueprint, from their records in the engine (BlueprintInfo; the simulation's Lua
// state is not looked at): {interface = facts, sim = facts}, facts = {count = all their blueprints, entities = their
// entity blueprints, ordinal, entityIndex, commandCaps, toggleCaps}, only the two counts when the id is not among them.
void PushBlueprintFacts(lua_State* L, const SimBlueprints* bps, const char* id) {
  lua_newtable(L);
  if (!bps) return;
  auto number = [L](const char* key, double v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  number("count", static_cast<double>(bps->Count()));
  number("entities", bps->EntityCount());
  if (const BlueprintInfo* bp = bps->Find(id)) {
    number("ordinal", bp->ordinal);
    number("entityIndex", bp->entityIndex);
    number("commandCaps", bp->commandCaps);
    number("toggleCaps", bp->toggleCaps);
  }
}
int l_BlueprintFactsForTests(lua_State* L) {
  const GameSession* session = static_cast<const GameSession*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* id = luaL_checkstring(L, 1);
  lua_newtable(L);
  lua_pushstring(L, "interface");
  PushBlueprintFacts(L, session->interfaceBlueprints(), id);
  lua_rawset(L, -3);
  lua_pushstring(L, "sim");
  PushBlueprintFacts(L, session->sim() ? &session->sim()->blueprints() : nullptr, id);
  lua_rawset(L, -3);
  return 1;
}
}  // namespace

bool GameSession::LoadInterfaceRules(lua_State* userState) {
  auto t0 = std::chrono::steady_clock::now();
  // The session's one rules state, as the original's one RRuleGameRules per session exported into both states
  // (func_WorldSessionUserLoad 0x885e53 / 0x885fe6, Sim::Sim 0x743c05): the simulation's own load, kept for this copy
  // (Sim::keepRules, set in BeginLoad; engine 7e001be, ui REQUESTS 46), its blueprints the sim's own, the engine's
  // fixes included. Only a simulation that did not keep it has the rules loaded here once more.
  rules_ = sim_ ? sim_->TakeRules() : nullptr;
  const bool shared = rules_ != nullptr;
  if (!shared) {
    rules_ = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
    rules_->SetHookDirs(hookDirs_);
    // The engine's script fixes for blueprints, as the simulation's own rules have them (a fix for one
    // of the game's mods: the campaign mod's weapons merged by label, fix 4), so that the interface's
    // copy matches the simulation's (ui REQUESTS 35). (The session has started: its fixes are final.)
    if (sim_ && sim_->fixes && sim_->RulesFixesWanted()) Sim::JoinFixesHooks(*rules_);
    SimBlueprints::StartRecording(*rules_);
    lua_State* R = rules_->L();
    if (!sessionMods_.empty() && PushSerializedLua(R, sessionMods_) && lua_istable(R, -1)) lua_setglobal(R, "__active_mods");
    else SetActiveMods(*rules_, modUids_);
    lua_settop(R, 0);
    if (!rules_->DoScript("/lua/ruleinit.lua")) return false;
  }
  lua_State* R = rules_->L();
  const auto t1 = std::chrono::steady_clock::now();
  interfaceBlueprints_ = std::make_unique<SimBlueprints>();
  interfaceBlueprints_->fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  interfaceBlueprints_->CopyToSim(R, userState);
  RegisterCategoryBindings(userState, interfaceBlueprints_.get());
  lua_pushlightuserdata(userState, this);
  lua_pushcclosure(userState, l_BlueprintFactsForTests, 1);
  lua_setglobal(userState, "moho64ui_BlueprintFacts");
  NoteRealGlobals(userState);
  rules_.reset();  // (the copy needs nothing of it afterwards)
  const auto t2 = std::chrono::steady_clock::now();
  if (shared)
    Logf(LogLevel::Info, "moho64ui: session: %zu blueprints in the interface's state, copied from the session's rules in %.2f s",
         interfaceBlueprints_->Count(), std::chrono::duration<double>(t2 - t1).count());
  else
    Logf(LogLevel::Info, "moho64ui: session: %zu blueprints in the interface's state, %.2f s (its own rules %.2f s, the copy %.2f s)",
         interfaceBlueprints_->Count(), std::chrono::duration<double>(t2 - t0).count(), std::chrono::duration<double>(t1 - t0).count(),
         std::chrono::duration<double>(t2 - t1).count());
  return true;
}

// (--fast-forward's beat, or the replay timeline's jump: seekTo_; a jump back waits for the restart)
bool GameSession::CatchingUp() const {
  return sim_ && !BeatRunning() && sim_->tick() < std::max(fastForwardTo, seekBack_ ? 0u : seekTo_);
}

void GameSession::AdvanceClock(double seconds) {
  due_ = std::min(1.0, due_ + seconds * std::pow(10.0, pace.rate / 10.0));  // (game time: the pace's rate, WLD_GetSimRate 0x88d0b0)
  lastFrameSeconds_ = seconds;
}

void GameSession::WaitForBeat() {
  if (worker_) worker_->Wait();
  if (beatToFinish_) FinishBeat();
}

void GameSession::WaitForWorker() {
  if (worker_) worker_->Wait();
}

void GameSession::StatsLines(std::vector<std::string>* lines) const {
#ifdef MOHO64UI_WORLD_PICTURE
  const render::WorldPicture* picture = windowPicture_ ? &windowPicture_->picture : picture_ ? &picture_->picture : nullptr;
  if (!picture) return;
  const render::WorldPicture::Stats& s = picture->LastStats();
  char buf[256];
  std::snprintf(buf, sizeof buf, "world picture: wait %.2f   prepare %.2f   draw %.2f   end %.2f ms   card %.2f ms", s.waitMs, s.prepareMs,
                s.drawMs, s.endMs, s.gpuMs);
  lines->push_back(buf);
  if (!s.gpuStages.empty()) {  // (the card's biggest stages)
    auto stages = s.gpuStages;
    std::stable_sort(stages.begin(), stages.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string line = "card:";
    for (size_t i = 0; i < stages.size() && i < 4; ++i) {
      std::snprintf(buf, sizeof buf, "   %s %.2f", stages[i].first ? stages[i].first : "?", stages[i].second);
      line += buf;
    }
    lines->push_back(line);
  }
  std::snprintf(buf, sizeof buf, "%d draw calls   %d models   triangles %.2fM + shadows %.2fM + ground %.2fM   particles %d (%d emitters)",
                s.drawCalls, s.models, s.triangles / 1e6, s.shadowTriangles / 1e6, s.groundTriangles / 1e6, s.particles, s.emitters);
  lines->push_back(buf);
  std::snprintf(buf, sizeof buf, "textures %.0f MB   card memory %.0f MB   %dx%d", s.textureBytes / 1048576.0, s.cardBytes / 1048576.0,
                s.width, s.height);
  lines->push_back(buf);
#else
  (void)lines;
#endif
}

void GameSession::Advance(double seconds) {
  if (!sim_) return;
  // (a test run's made-up time of the card, moho64ui_SetFrameCostForTests: every frame of a game counts, also without
  // a window)
  if (FrameCost().fakeMs >= 0) FrameCost().Note(FrameCost().fakeMs);
  if (BeatRunning()) {  // (the beats' thread is busy: the time counts, nothing else is done)
    AdvanceClock(seconds);
    return;
  }
  if (beatToFinish_) FinishBeat();
  SeekReached();
  const bool catchingUp = CatchingUp();
  if (catchingUp) {  // (--fast-forward, for measurements, or a jump of the replay timeline: up to 50 beats a frame until that beat)
    due_ = std::min<uint32_t>(std::max(fastForwardTo, seekTo_) - sim_->tick(), 50) * 0.1 + 0.01;
  } else {
    AdvanceClock(seconds);  // (after a long stall, catch up by at most a second of beats)
  }
  // On its own thread one beat runs at a time, and it starts early by about as long as the last
  // took: it is done when the picture's moment reaches it (the moment may then lie a little
  // before the beats run: due_ below 0).
  const bool onThread = beatsOnThread && !catchingUp;
  const double startAt = onThread ? 0.1 - lead_ : 0.1;
  while (due_ >= startAt) {
    due_ -= 0.1;
    if (play_) {  // a replay: its own orders, beat by beat; the player's only pause and resume it
      if (takeOrders) ViewerControls(takeOrders());
      if (ReplayHeld() && !catchingUp) {  // (the viewer holds the replay: no beat runs, and the time held does not pile
                                           // up; a jump of the timeline runs on through it)
        due_ = 0;
        break;
      }
      // (every beat its stream's orders first, as the engine's own player of the original's replays: so the recorded
      // game's pause and its resume are both applied; a paused beat while the recorded game is paused)
      if (!play_->AtEnd() && play_->ApplyUntilAdvance(*sim_)) {
        if (sim_->Paused()) {
          sim_->PausedBeat();
        } else if (onThread) {
          StartBeatOnThread();
          return;
        } else {
          TimedTick();
        }
      } else {
        continue;  // the replay is over: the game stands where it ended
      }
      NoteBeat();
      DeliverBeatSync();
      continue;
    }
    if (takeOrders) {
      std::string orders = takeOrders();
      Record(orders);
      if (!orders.empty() && !ApplyCommands(*sim_, orders)) Logf(LogLevel::Warning, "moho64ui: an order was not understood by the simulation");
    }
    if (sim_->Paused()) {
      // (recorded as every beat: the original's replays hold their paused beats (the engine: AIR 40, LAND 72), and
      // a paused beat changes the simulation's state, so a playback must run it too)
      sim_->PausedBeat();
      Record(CommandWriter::AdvanceMessage(1));
      FingerprintBeat();
    } else if (onThread) {
      Record(CommandWriter::AdvanceMessage(1));
      StartBeatOnThread();
      return;
    } else {
      TimedTick();
      Record(CommandWriter::AdvanceMessage(1));
      FingerprintBeat();
    }
    NoteBeat();
    DeliverBeatSync();
  }
  SeekReached();
}

void GameSession::DeliverBeatSync() {
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
  auto t0 = Clock::now();
  std::vector<std::string> syncs = sim_->TakeSync();
  LongFrames().Note("(beat: the simulation's sync taken)", ms(t0));
  t0 = Clock::now();
  for (std::string& sync : syncs) {
    if (onSync) onSync(sync);
    else pendingSync_.push_back(std::move(sync));
  }
  LongFrames().Note("(beat: the sync given to the interface's scripts)", ms(t0));
  PostPrints(*sim_);
}

// The tick and the picture's copy of it on the beats' thread (the renderer's viewer does the same:
// render/live_sim.cpp); nothing here touches the simulation until FinishBeat.
void GameSession::StartBeatOnThread() {
  beatFrom_ = sim_->tick();
  if (!worker_) worker_ = std::make_unique<BeatWorker>();
  beatToFinish_ = true;
  worker_->Start([this] {
    using Clock = std::chrono::steady_clock;
    const auto ts = Clock::now();
    sim_->Tick();
    const auto tp = Clock::now();
    NotePictureBeat();
    workerSimSeconds_ = std::chrono::duration<double>(tp - ts).count();
    workerPictureSeconds_ = std::chrono::duration<double>(Clock::now() - tp).count();
  });
}

// After a beat on the thread: its times, its sounds and shakes, and its sync for the interface.
void GameSession::FingerprintBeat() {  // (after each beat of a recorded live game: its line in <replayOut>.fp)
  if (fp_ && !play_ && sim_) fp_->Beat(*sim_, ++fpBeats_);
}

void GameSession::FinishBeat() {
  beatToFinish_ = false;
  FingerprintBeat();  // (the beat that ran on its own thread, done now)
  FrameTimes& times = Times();
  times.simBeats += workerSimSeconds_;
  times.pictureBeats += workerPictureSeconds_;
  ++times.beats;
  // The next beat starts this much early: the longest of the last beats, with some room (beats
  // vary; the old estimate fades slowly), at most most of a beat.
  const double speed = std::pow(10.0, pace.rate / 10.0);
  const double took = (workerSimSeconds_ + workerPictureSeconds_) * speed;
  leadBeats_ = std::max(took * 1.3, leadBeats_ * 0.9);
  // (A beat starts only in a frame: one frame's time more, or it starts up to a frame late.)
  lead_ = std::min(0.08, leadBeats_ + lastFrameSeconds_ * speed);
  static const bool beatLog = std::getenv("MOHO64UI_BEAT_LOG") != nullptr;
  const auto tHere = std::chrono::steady_clock::now();
  if (beatLog)
    Logf(LogLevel::Info, "moho64ui: beat %u on its thread: %.1f ms (sim %.1f, picture %.1f); next starts %.1f ms early; held so far %d",
         sim_->tick(), (workerSimSeconds_ + workerPictureSeconds_) * 1000.0, workerSimSeconds_ * 1000.0,
         workerPictureSeconds_ * 1000.0, lead_ / speed * 1000.0, times.heldPictures);
  NoteBeatHere();
  LongFrames().Note("(beat: after it, here: heights, allies, sounds, shakes)",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tHere).count());
  DeliverBeatSync();
}

// The player's orders while a replay plays: only his pause and resume count (the messages
// RequestPause and Resume of the command stream, sim/command_stream.cpp 4 and 5), and they pause
// the replay for him alone. INFERRED: what the original does with them in a replay (its replay
// has no command source of the player's); the replay's own pauses are played as recorded.
void GameSession::ViewerControls(const std::string& orders) {
  size_t p = 0;
  while (p + 3 <= orders.size()) {
    const int type = static_cast<unsigned char>(orders[p]);
    const size_t length = static_cast<unsigned char>(orders[p + 1]) | (static_cast<size_t>(static_cast<unsigned char>(orders[p + 2])) << 8);
    if (length < 3) break;
    if (type == 4) ReplayHeld() = true;  // (the viewer's hold, the host's own: not the recorded game's pause)
    else if (type == 5) ReplayHeld() = false;
    p += length;
  }
}

void GameSession::TimedTick() {  // (a beat of the simulation, timed for the window's log)
  const auto ts = std::chrono::steady_clock::now();
  sim_->Tick();
  Times().simBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
  ++Times().beats;
}

void GameSession::NoteBeat() {
  const auto tp = std::chrono::steady_clock::now();
  NotePictureBeat();
  Times().pictureBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count();
  NoteBeatHere();
}

// (Asked when a game's interface starts, before any change of the focus army: then the focus is
// the player's own army.)
int GameSession::OwnArmy() const { return sim_ && !sim_->isReplay ? sim_->focusArmy : -1; }

void GameSession::NotePictureBeat() {
  g_crashTick = sim_->tick();  // (a crash's line in the log names the beat: src/core/crash.h)
#ifdef MOHO64UI_WORLD_PICTURE
  if (picture_) {
    picture_->picture.Beat(*sim_);  // (it takes the beat's effect events too)
  } else if (sim_->collectHits) {  // (no picture to take the hits, impacts and health: dropped, or the lists grow)
    sim_->TakeHits();
    sim_->TakeImpacts();
    sim_->TakeHealth();
  }
#endif
}

void GameSession::NoteBeatHere() {
  const auto th = std::chrono::steady_clock::now();
  CopyHeights();
  CopyAllies();
  LongFrames().Note("(beat: the heights and allies copied)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - th).count());
#ifdef MOHO64UI_WORLD_PICTURE
  // A new focus army (SetFocusArmy; the engine applies it at a beat's Sync): the picture looks
  // through it from now on (its fog, its remembered buildings; 0: an observer's, no fog); the
  // allies above follow it already.
  if (picture_ && sim_ && pictureFocus_ != sim_->focusArmy) {
    pictureFocus_ = sim_->focusArmy;
    picture_->picture.SetFocusArmy(pictureFocus_ + 1);
  }
#endif
  if (sim_ && sim_->L()) Times().simLuaKB = static_cast<double>(lua_getgccount(sim_->L()));
  const auto tsnd = std::chrono::steady_clock::now();
  PlayBeatSounds();
  Times().soundBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - tsnd).count();
  LongFrames().Note("(beat: its sounds)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tsnd).count());
  // The beat's camera shakes (the engine's ShakeEvents) go to the world's cameras.
  std::vector<ShakeEvent> shakes;
  ShakeEvents(*sim_, &shakes);
  if (!seekTo_)  // (not while the replay timeline jumps)
    for (const ShakeEvent& e : shakes) PostCameraShake(e.position, e.radius, e.maxShakeEpicenter, e.minShakeAtRadius, e.interval);
}

// The world's sounds of the beat (the engine's SoundEvents): one-shots at their place, and the
// entities' running sounds, which then follow their units. (The second, "rumble" sound of a
// running sound is left out.)
// Whose sounds: the player's own and the allies' always; anything else only when the player's
// army sees it now (the simulation's own recon, the engine's ReconOf for a unit and ReconAt for a
// place: what belongs to no army, and a unit that is gone by now). What is only on radar or sonar
// is not heard: so a sound gives away no more than the picture does. (That choice is ours; what
// the original's sound manager does with unseen units is not read.)
void GameSession::PlayBeatSounds() {
  if (!sounds) return;
  std::vector<SoundEvent> events;
  SoundEvents(*sim_, &events);
  if (events.empty() && foreignLoops_.empty() && !sounds->HasLoops()) return;
  const int focus = sim_->focusArmy + 1;
  std::vector<bool> friendly;  // by army - 1
  for (const ArmyView& a : ArmyViews(*sim_)) {
    if (a.index < 1) continue;
    if (friendly.size() < static_cast<size_t>(a.index)) friendly.resize(static_cast<size_t>(a.index), false);
    bool ally = focus >= 1 && focus <= static_cast<int>(a.alliance.size()) && a.alliance[static_cast<size_t>(focus - 1)] == 2;
    friendly[static_cast<size_t>(a.index - 1)] = a.index == focus || ally;
  }
  auto isFriendly = [&](int army) { return army >= 1 && army <= static_cast<int>(friendly.size()) && friendly[static_cast<size_t>(army - 1)]; };
  std::vector<UnitView> units;  // in entity-id order
  UnitViews(*sim_, &units);
  auto isUnit = [&](uint32_t id) {
    auto it = std::lower_bound(units.begin(), units.end(), id, [](const UnitView& u, uint32_t v) { return u.id < v; });
    return it != units.end() && it->id == id;
  };
  auto seen = [&](uint32_t entity, const float place[3]) {
    if (focus < 1) return true;  // no army to ask for: an observer hears all
    uint32_t recon = isUnit(entity) ? ReconOf(*sim_, focus, entity) : ReconAt(*sim_, focus, place);
    return (recon & kReconSeenNow) != 0;
  };
  const bool log = std::getenv("MOHO64UI_SOUND_LOG") != nullptr;
  for (const SoundEvent& e : events) {
    bool ours = isFriendly(e.army);
    if (log)
      Logf(LogLevel::Info, "moho64ui: sound: tick %u %s entity %u army %d at %.0f %.0f %.0f: %s/%s (%s)", static_cast<unsigned>(sim_->tick()),
           e.kind == SoundEvent::Play ? "play" : "loop", static_cast<unsigned>(e.entity), e.army, e.position[0], e.position[1],
           e.position[2], e.bank.c_str(), e.cue.c_str(), ours ? "ours" : seen(e.entity, e.position) ? "seen" : "not seen");
    if (e.kind == SoundEvent::Play) {
      // (not while the replay timeline jumps: up to 50 beats a frame; the running sounds below are still followed)
      if (!seekTo_ && (ours || seen(e.entity, e.position))) sounds->PlayAt(e.bank, e.cue, e.position);
    } else if (ours) {
      sounds->SetLoop(e.entity, e.bank, e.cue, e.position);
    } else if (e.bank.empty() || e.cue.empty()) {  // another army's running sound ends
      foreignLoops_.erase(e.entity);
      if (heardLoops_.erase(e.entity)) sounds->SetLoop(e.entity, "", "", e.position);
    } else {
      ForeignLoop& loop = foreignLoops_[e.entity];
      if (heardLoops_.erase(e.entity)) sounds->SetLoop(e.entity, "", "", e.position);  // it changes: start again below
      loop.bank = e.bank, loop.cue = e.cue;
      for (int i = 0; i < 3; ++i) loop.position[i] = e.position[i];
    }
  }
  // Running sounds follow their units; another army's start and end as it comes into sight and leaves it.
  for (const UnitView& u : units) {
    auto it = foreignLoops_.find(u.id);
    if (it != foreignLoops_.end())
      for (int i = 0; i < 3; ++i) it->second.position[i] = u.position[i];
    sounds->MoveLoop(u.id, u.position);
  }
  for (auto& entry : foreignLoops_) {
    bool hear = seen(entry.first, entry.second.position), heard = heardLoops_.count(entry.first) != 0;
    if (hear && !heard) {
      sounds->SetLoop(entry.first, entry.second.bank, entry.second.cue, entry.second.position);
      heardLoops_.insert(entry.first);
    } else if (!hear && heard) {
      sounds->SetLoop(entry.first, "", "", entry.second.position);
      heardLoops_.erase(entry.first);
    }
  }
}

void GameSession::RunFirstBeat() {
  if (worker_) worker_->Wait();
  if (!sim_ || sim_->tick() != 0) return;
  // (A replay's first beat is its stream's first: what came before the first Advance, then it.)
  if (play_ && !play_->AtEnd()) play_->ApplyUntilAdvance(*sim_);
  if (fp_ && !play_) fp_->Beat(*sim_, 0);  // (beat 0: the state Start left)
  sim_->Tick();
  Record(CommandWriter::AdvanceMessage(1));
  FingerprintBeat();
  NoteBeat();
  for (std::string& sync : sim_->TakeSync()) {
    if (onSync) onSync(sync);
    else pendingSync_.push_back(std::move(sync));
  }
  PostPrints(*sim_);
}

float GameSession::TerrainHeight(float x, float z) const {
  if (heights_.empty()) return 0.0f;
  return TerrainHeightOf(heights_.data(), heightsW_, heightsH_, heightsScale_, x, z);
}

// The ground's heights copied from the simulation's map when they changed (a building flattens
// ground; the engine's change counter): only between beats, never while one runs.
// The focus army's allies (the picture's life bars and colours), worked out after each beat.
void GameSession::CopyAllies() {
  allies_.clear();
  if (!sim_) return;
  const int focus = sim_->focusArmy + 1;
  for (const ArmyView& a : ArmyViews(*sim_))
    if (focus >= 1 && a.index >= 1 && a.index != focus && focus <= static_cast<int>(a.alliance.size()) &&
        a.alliance[static_cast<size_t>(focus - 1)] == 2)
      allies_.push_back(a.index);
}

void GameSession::CopyHeights() {
  const TerrainMap* map = sim_ ? sim_->map() : nullptr;
  if (!map) {
    heights_.clear();
    return;
  }
  if (map->changes() == heightsChanges_ && !heights_.empty()) return;
  heightsW_ = map->width(), heightsH_ = map->height(), heightsScale_ = map->heightScale();
  const uint16_t* raw = map->heightData();
  heights_.assign(raw, raw + static_cast<size_t>(heightsW_ + 1) * static_cast<size_t>(heightsH_ + 1));
  heightsChanges_ = map->changes();
}

uint32_t GameSession::tick() const { return sim_ ? sim_->tick() : 0; }
size_t GameSession::unitCount() const { return sim_ ? sim_->units().size() : 0; }

std::vector<std::string> GameSession::Status() const {
  std::vector<std::string> lines;
  if (!sim_) return lines;
  char buf[200];
  uint32_t t = sim_->tick();
  lines.push_back("The simulation is running.");
  std::snprintf(buf, sizeof buf, "Map %s, %zu armies, seed %u", map_.c_str(), armies_, seed_);
  lines.push_back(buf);
  std::snprintf(buf, sizeof buf, "Game time %u:%02u (tick %u), %zu units", t / 600, (t / 10) % 60, t, sim_->units().size());
  lines.push_back(buf);
  return lines;
}

void GameSession::DrawStatus(DrawTarget& target) const {
  if (!sim_) return;
  const int size = 18;
  FontMetrics m = GetFontMetrics("Arial", size);
  float line = m.height + 6, x = 40, y = 40;
  auto lines = Status();
  float w = 0;
  for (const auto& s : lines) w = std::max(w, StringAdvance("Arial", size, s));
  target.FillRect(Rect{x - 16, y - 12, x + w + 16, y + line * static_cast<float>(lines.size()) + 12}, 0xc0101820u);
  for (const auto& s : lines) {
    DrawString(target, "Arial", size, s, x, y + m.ascent, 0xffe0e8f0u);
    y += line;
  }
}

}  // namespace moho::ui
