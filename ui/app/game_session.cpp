#include "app/game_session.h"

#include "app/backdrop.h"
#include "app/menu_flight.h"
#include "app/window.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "core/crash.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "core/workers.h"
#include "draw/draw.h"
#include "maui/font.h"
#include "net/beat_exchange_net.h"
#include "net/lobby_core.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/command_stream.h"
#include "sim/command_writer.h"
#include "sim/fingerprint.h"
#include "sim/lockstep.h"
#include "sim/moments.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/json.h"
#include "render/remasters.h"
#include "render/view.h"
#include "render/world.h"
#include "render/world_picture.h"
#endif

#include <algorithm>
#include <cctype>
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
// The player's looks that are the world's own switches (the Options menu's Remastered tab), on `w`: each frame on the
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
  w.rockHits = looks.battleEffects;  // (rocks that react to fire: chips, dust, scorch; render 1fb738a2, request 59)
  w.litSmoke = w.smokeShadows = w.heatShimmer = looks.battleEffects;
  w.unitScars = looks.battleEffects;
  // (render e8a4d56: wrecks and craters that glow and cool; dust, spray and contrails; explosions as lit volumes
  // and the nuke's set piece, which without the volume art still throws the blasts' light and the nuke's flash)
  w.hotMetal = w.movementDust = w.volumeExplosions = looks.battleEffects;
  w.realExhausts = looks.battleEffects;  // (engines' exhausts as lit plumes, render 3e6ce2e: Chris's item 8)
  w.remasteredBuild = looks.battleEffects;  // (render 6b8109e: the build effects remade; with the remastered effects and the modern look)
  w.energyShields = looks.energyShields;
  w.lampsTeamColour = looks.lampsTeamColour;  // (Unit Glow Colour: team or faction, render 1fd45da)
  w.heat.softScorch = looks.softScorch;        // (Scorch Glow: soft or bright, render 5d77cf6)
  // (the renderer's looks from Chris's big test, each with the earlier look as Off: LookSettings)
  w.water.ocean.glitter = looks.oceanGlitter ? 1 : 0;  // (Ocean Glitter)
  w.water.ocean.nearSparkle = looks.seaSparkle ? 0.3f : 0.0f;  // (Sea Sparkle, render 03fc92c; 0.3 on: render 7617828)
  w.unitDetail = looks.unitDetail ? 1.0f : 0.0f;  // (Close-up Detail, render 5ee436a)
  w.clouds.mix = looks.cloudMix ? 1.0f : 0.0f;    // (Cloud Layout: Mixed / Even, render b6e66de, request 56)
  w.stormCells = looks.stormCells;                // (Storm Clouds: Cells / Plain, render 309e718, request 57)
  w.starryNight = looks.starryNight;              // (Night Sky: Starry / Plain, render acf9384, request 58)
  w.water.shoreSoak = looks.shoreSoak;                // (Shore Soak)
  w.waterFxMirror = looks.waterFxMirror;              // (Effects Mirrored in Water, the renderer's request 69)
  // (Shield Bending: Off nothing bent; Subtle a little, units under the shield never; Full everything behind, as 10-28)
  w.energy.bend = looks.shieldBend <= 0 ? 0.0f : looks.shieldBend == 1 ? 0.35f : 1.0f;
  w.energy.steadyUnits = looks.shieldBend <= 1;
  // (Unit Glow at Night: at 1 the glow as before the big test, all of it)
  const float glow = std::max(0.0f, std::min(1.0f, looks.unitGlow));
  w.hullGlow = glow;
  w.hullReach = 0.4f + 0.6f * glow;
  w.hullAir = glow >= 1.0f ? 1.0f : 0.5f;
  w.hullEvenColours = w.hullShare = glow < 1.0f;
  w.post.sharpen = std::max(0.0f, std::min(1.0f, looks.sharpen));  // (Sharpen)
  w.hazeMist = std::max(0.0f, std::min(2.0f, looks.hazeMist));     // (Haze & Mist, render [hazeamt]: 1 as made)
  w.remasteredGate = looks.remasteredGate;                         // (Remastered Gateway)
  w.warpInColours = looks.warpInColours;                           // (Warp-in in Faction Colours)
  w.ghostLook = looks.ghostLook ? 1 : 0;                           // (Ghost Look: see-through, or solid as before)
  w.guidePull = looks.guideOverGrass ? 1.2f : 0.0f;                // (Guide over Grass)
  w.cloakLook = looks.cloakLook;                                   // (Cloak Look)
  w.stealthLook = looks.stealthLook;                               // (Stealth Veil)
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
  // The map's edge, one of the three looks (the renderer's answer to ui request 20: all three
  // switches set, as their defaults are neither look; its request 62: "surround" is the land
  // beyond with the map's own sculpted land there, where the map brings one).
  const bool frame = edge == "frame";
  w.worldBeyond = !frame;
  w.frame = frame;
  w.beyondHaze = false;
  w.mapSurround = edge == "surround";
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

bool& GameSession::FramesByCount() {
  static bool byCount = false;
  return byCount;
}

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
      p.window = v.window;  // ([head2] 1: the second window's, its rect in that window's pixels)
      list.push_back(p);
    }
    picture.SetViews(list);
    return true;
  }
  // [head2] The second head's world view goes to the renderer (a view of window 1) only while its second window is on
  // (GameSession::UseSecondWindow): the game's picture in the window.
  bool SecondWindowOn() const override { return direct && picture.SecondWindowOn(); }
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
  void UnitsAt(float x, float y, float tolerance, std::vector<uint32_t>* out) override { picture.UnitsAt(x, y, tolerance, out); }
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
      p.placing = g.placing;  // (the cursor's ghost, brighter than the ordered sites: ui request 41)
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
    if (render::World* w = picture.world()) w->steadyFrames = GameSession::FramesByCount();  // (a stepped run: game_session.h)
    // The player's own looks (the Options menu's Remastered tab), when they change.
    LookSettings looks = Looks();
    // FSR 3 or Frame Generation switched off while this picture has one sample a pixel and the player's Anti-Aliasing
    // asks for more: kept on until the next picture is made (the next map), as Anti-Aliasing itself changes only
    // then. The picture's samples are taken when it is made (1 under FSR 3 or Frame Generation, SceneSamples); with
    // the temporal pass switched off at once the game showed a bare one-sample picture, no anti-aliasing at all
    // (Chris's playtest of 2026-11-09: "without fsr 3 on units seem to lack a lot of details like no anti aliasing";
    // the renderer's request 65). The options' tips say so.
    if (render::gSceneSamples == 1 && looks.SceneSamples() > 1) {
      if (lookApplied) looks.temporalUpscale = lookNow.temporalUpscale, looks.frameGeneration = lookNow.frameGeneration;
      if (!looks.temporalUpscale && !looks.frameGeneration) looks.temporalUpscale = true;
      static bool said = false;
      if (!said) Logf(LogLevel::Info, "moho64ui: FSR 3 / Frame Generation switched off in this picture: kept on until the next map (its anti-aliasing is chosen when a map opens)");
      said = true;
    }
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
// The lighting bundles as a picture starts (world_picture.h: off, the world's own switches as at first; the hidden
// outlines on). While the picture holds looks (HoldLooks) they are only kept for the map opening behind.
void StartBundles(render::WorldPicture& p) {
  p.SetAtmosphere(false);
  p.SetSoftShadows(false);
  p.SetRayTracing(false);
  p.SetWeatherEffects(false);
  p.SetGroundDetail(false);
  p.SetFrameGeneration(false);
  p.SetHiddenOutlines(true);
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
  StartBundles(p);
  p.SetMinimapMeshes(false);
}
// The menu's flight's looks as its map opens (Backdrop::Mode::Flight): held for its map opening behind (HoldLooks; SwapMap
// lays them on), the player's Atmosphere (the light in the air and the volumetric clouds: they make the shot), Soft
// Shadows and Ground Detail; no Weather Effects (its weather is fixed and fair). Ray Tracing as the player has it: what
// is fixed when a map opens (the renderer's answer to ui request 49, R2: ray tracing's shaders, with the texture detail
// and the season, which BeginWindowMap gives) is the game's, so that a game on SETONS_R can take the flight's world over
// (KeepMenuMap); its menu look (the player's, without weather effects and frame generation) comes after the swap from
// setters that act per frame only (FlightFrameLooks). The world's own switches as the player has them (WorldLooks, the
// map's `before`: battle effects, energy shields ...). The upscaler and the effects' quality as a picture starts (StartAfresh).
void FlightLooks(render::WorldPicture& p, std::function<void(render::World&)>* before) {
  const LookSettings looks = Looks();
  p.HoldLooks(true);
  StartBundles(p);
  p.SetRayTracing(looks.rayTracing);  // (its shaders built as the map opens, and warmed with it)
  p.SetAtmosphere(looks.atmosphere);
  p.SetSoftShadows(looks.softShadows);
  p.SetGroundDetail(looks.groundDetail);
  *before = [looks](render::World& w) { WorldLooks(w, looks, true); };
}
// The flight's look once its world shows (after SwapMap or RestartMap, nothing held), by setters the renderer names as
// acting per frame (R2): the PLAYER'S looks, ray tracing included (the renderer's R4 at 4K, 2026-11-09: full looks
// 8.0 / 10.0 ms median / p95 over the whole battle, the lighter look 6.4 / 7.7, "keep the player's full looks: 8 ms is
// far inside a 60 cap"; a weak card follows the player's own preset); its ray tracing off-then-on on one world gives
// full ray tracing again (the renderer confirmed, world.cpp World::Begin). No Weather Effects (the flight's weather is
// fixed) and no frame generation; the player's Atmosphere, Soft Shadows and Ground Detail (also as a game's world after
// the menu's flight laid the picture's own back: a swap behind a game's world gives StartAfresh's first).
void FlightFrameLooks(render::WorldPicture& p) {
  const LookSettings looks = Looks();
  p.SetRayTracing(looks.rayTracing);
  p.SetWeatherEffects(false);
  p.SetFrameGeneration(false);
  p.SetAtmosphere(looks.atmosphere);
  p.SetSoftShadows(looks.softShadows);
  p.SetGroundDetail(looks.groundDetail);
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
  int pendingFrames = 0;  // (the frames drawn while it opened: ServeWindowMap's line, for what the time went on)
  // What a map was opened with that only the next map's opening changes (the renderer's answer to ui request 49, R2/R3:
  // the texture detail, the season, ray tracing's shaders), for the map opening behind and the one shown: a game takes
  // the menu's flight's world over only when they are the game's (KeepMenuMap).
  struct OpenedWith {
    int textureDetail = -1;
    int season = -1;  // (0 summer, 1 autumn: LookSettings::season)
    bool rays = false;  // (ray tracing on as it opened: its shaders built, where the card can)
    bool operator==(const OpenedWith& o) const { return textureDetail == o.textureDetail && season == o.season && rays == o.rays; }
  };
  OpenedWith pendingWith, shownWith;
  // The world's sky as its map opened it (taken at the menu's swap, before the evening was laid on: Backdrop::Dress),
  // given back when a game takes the menu's world over (a game's map opened anew has it so).
  struct OpenSky {
    bool taken = false;
    float timeOfDay = -1, cloudCover = 0, rain = 0, snow = 0, fog = 0, wet = 0, snowLying = 0;
    float cinematicHaze = 0;  // (the flight thins it: FlightHaze)
    render::Climate climate = render::Climate::Temperate;
  };
  OpenSky shownSky;
  // The menus' map begun behind a game's world that is over (ServeWindowMap: the score screen), its lighting bundles
  // held for it (HoldLooks) and the picture's other settings still the game's until it is swapped in.
  bool menuBehindGame = false;
  std::string failedGameMap;  // (a game's map the picture could not open: not tried again for that launch)
  bool vsync = true;
  int drawnW = 0, drawnH = 0;  // (the window's size as last drawn: the start camera's aspect, ServeLoad)
  bool lowLatency = false;     // (as the renderer's picture starts: SetLowLatency)
  int layerW = 0, layerH = 0;  // the interface layer's size as last given (0: none yet)
  int layer2W = 0, layer2H = 0;  // [head2] the second window's layer's (GiveSecondLayer)
  // The picture over the window drawn by the card (SetBackdrop): none, the interface's (CardSink: the loading
  // screen's film, the "IN TRANSIT" cover), or black fading out over the menus' map just swapped in (it comes in
  // from black, as the read-back way's background does, rather than at once after the seconds of black), or the
  // picture held through a jump back of the replay timeline (Frozen: GameSession::Rewind, dimmed by its tint).
  // (Still: the main menu's still picture, Backdrop::Mode::Still; Cover: black coming over the menu's flight at its
  // storyboard's end, held until the battle starts over: GameSession::ServeMenuFlight; Kept: the flight's kept first
  // frame while its map opens, which the live flight then comes in over, a Fade from it: fadeKept)
  enum class Over { None, Card, Fade, Frozen, Still, Cover, Kept };
  bool fadeKept = false;
  Over over = Over::None;
  float overAlpha = 1;
  std::chrono::steady_clock::time_point fadeFrom{};
  bool fading = false;
  uint32_t black = 0xff000000u;
  bool heldWorld = false;  // (the world hidden while a jump back held the picture: shown again after, as the game has it)
};
#else
struct GameSession::WindowPicture {};
#endif

// The main menu's flight's kept first frame (GameSession::kept_; docs/ui-menuflight.md 4.5): its file for a window's
// size, read and written on a thread of its own. The file: "MFK1", the width, the height and a key (32 bits each, the
// PC's own order), then the pixels (0xAARRGGBB, rows from the top). The key is the camera path's and the looks' the
// frame was taken with (KeptKey): a frame of another path or other looks is not shown (the live flight would not take up
// where it left). Ours.
// The first start has no such file yet, nor has the first one after a look changed or the window got another size, and
// the flight's map takes its ten to twenty seconds to open: the menu stood on black all that time (Chris's playtest of
// 2026-11-09: "initial startup I got no background"). Then the program's own picture of the flight's first view is
// shown instead (ui/data/menuflight/first_frame.jpg: taken once at 1920x1080 with the looks a new player starts with;
// `stock`), fitted over the window with its shape kept; the live flight comes in over it as over a kept frame, and this
// run's own frame is kept for the next start as always.
constexpr const char* kStockFirstFrame = "/moho64ui/menuflight/first_frame.jpg";
struct GameSession::KeptFrame {
  std::string dir;           // (beside the preferences: userDir)
  std::string stock;         // the program's own first frame's file bytes (empty: none), decoded on the thread when needed
  int w = 0, h = 0;          // the size read for (0: none yet)
  uint32_t key = 0;          // (the key it was read for)
  std::vector<uint32_t> pixels;  // the frame for w x h (empty: none there, or not read yet)
  int pw = 0, ph = 0;            // the pixels' own size: w x h, or the stock picture's
  bool isStock = false;          // the pixels are the stock picture's
  int readW = 0, readH = 0;      // (the thread's, as readPixels)
  bool readStock = false;
  bool taken = false;            // this run's taken (once a run)
  std::vector<uint32_t> fresh;   // (this run's, taken: in place of `pixels` once those are no longer over the window)
  int freshW = 0, freshH = 0;
  uint32_t freshKey = 0;
  int shownFrames = 0;     // (tests: the frames it was shown in this run)
  double takeSeconds = -1;  // (tests, the log: the photo's time)
  std::thread io;
  std::atomic<bool> busy{false};
  std::vector<uint32_t> readPixels;  // (the thread's, until it is through)
  bool reading = false;              // (the thread's job is a read: Through hands it over)
  ~KeptFrame() {
    if (io.joinable()) io.join();
  }
  std::string File(int width, int height) const {
    return dir + "/menuflight_first_" + std::to_string(width) + "x" + std::to_string(height) + ".raw";
  }
  // (once a size: read on the thread; Through hands it over)
  void Read(int width, int height, uint32_t wantKey) {
    if (busy.load() || reading || (w == width && h == height && key == wantKey)) return;
    if (io.joinable()) io.join();
    w = width, h = height, key = wantKey;
    pixels.clear();
    reading = true;
    busy.store(true);
    io = std::thread([this, file = File(width, height), width, height, wantKey] {
      std::vector<uint32_t> px;
      int rw = width, rh = height;
      bool fromStock = false;
      if (const std::optional<std::string> data = hostfs::ReadFile(file)) {
        uint32_t head[4] = {};
        const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (data->size() == sizeof head + n * 4) {
          std::memcpy(head, data->data(), sizeof head);
          if (std::memcmp(head, "MFK1", 4) == 0 && head[1] == static_cast<uint32_t>(width) && head[2] == static_cast<uint32_t>(height) &&
              head[3] == wantKey) {
            px.resize(n);
            std::memcpy(px.data(), data->data() + sizeof head, n * 4);
          }
        }
      }
      if (px.empty() && !stock.empty()) {  // (no frame of the player's own for this size and these looks: the program's)
        Image image;
        if (DecodeImage(stock, image) && image.valid()) {
          for (uint32_t& c : image.pixels) c |= 0xff000000u;  // (opaque: SetBackdrop's colours are multiplied by their alpha)
          px.swap(image.pixels), rw = image.width, rh = image.height, fromStock = true;
        }
      }
      readPixels.swap(px);
      readW = rw, readH = rh, readStock = fromStock;
      busy.store(false);
    });
  }
  bool Through() {
    if (!reading || busy.load()) return false;
    reading = false;
    io.join();
    pixels.swap(readPixels);
    readPixels.clear();
    pw = readW, ph = readH, isStock = readStock;
    if (!pixels.empty() && isStock)
      Logf(LogLevel::Info, "moho64ui: the menu's flight: no kept first frame for %dx%d and these looks yet: the program's own (%dx%d) is shown until it is live",
           w, h, pw, ph);
    else if (!pixels.empty()) Logf(LogLevel::Info, "moho64ui: the menu's flight: its kept first frame read (%dx%d), shown until it is live", w, h);
    return true;
  }
  void Write(std::vector<uint32_t> px, int width, int height, uint32_t wantKey) {
    if (busy.load() || reading) return;
    if (io.joinable()) io.join();
    busy.store(true);
    io = std::thread([this, file = File(width, height), px = std::move(px), width, height, wantKey] {
      std::string data(16 + px.size() * 4, '\0');
      const uint32_t head[4] = {0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), wantKey};
      std::memcpy(data.data(), head, sizeof head);
      std::memcpy(data.data(), "MFK1", 4);
      std::memcpy(data.data() + 16, px.data(), px.size() * 4);
      if (!hostfs::WriteFile(file, data)) Logf(LogLevel::Warning, "moho64ui: the menu's flight: its first frame not kept (%s)", file.c_str());
      busy.store(false);
    });
  }
};

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

// (aheadInLoad, game_session.h) The loading thread's beats: where it stops for the hand-over (Parked, until this thread
// says Go; Stop: the launch is dropped), and what the beats leave for FinishLoad. Written there, read here only through
// the gate's lock or after the worker's wait.
struct GameSession::LoadAhead {
  enum class Gate { Working, Parked, Go, Stop };
  std::mutex mutex;
  std::condition_variable cv;
  Gate gate = Gate::Working;
  bool jobDone = false;            // (the loading thread's job is over, beats or not)
  std::atomic<bool> stop{false};   // (StopLoadAhead: no further beat)
  bool decideSounds = false;       // (a sound system there: each beat's sounds decided as the beats' thread does)
  std::vector<std::string> setupSync;  // the setup's own sync (Start's "beat 0"), taken before beat 1
  std::vector<std::string> syncs;      // each beat's, in order
  std::vector<SoundAction> sounds;     // each beat's sounds, in order (played at the load's end)
  std::vector<ShakeEvent> shakes;      // and camera shakes
  uint32_t ranTo = 0;                  // the tick reached (0: none ran)
  double parkedAt = -1, goAt = -1, doneAt = -1;  // (seconds from BeginLoad)
  double beatsSeconds = 0, longestMs = 0;
  uint32_t longestTick = 0;
  size_t hits = 0, impacts = 0, health = 0;  // (the simulation's lists after them, for the picture: logged)
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
  uint32_t jumpsBack = 0, backLoadingScreens = 0;  // (State's: kept across the jumps back, as the timeline is)
  double lastBackSeconds = 0;
  // The player's bookmarks of the replay (user/replay_timeline.h ReplayBookmark): read from the file beside it when
  // it opened (StartMomentScan), written there at each change, kept across the jumps back. `file`: the replay's own
  // path; `digest`: which replay it is (ReplayDigest of its bytes).
  std::string file, digest;
  std::vector<ReplayBookmark> bookmarks;

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
    s.jumpsBack = jumpsBack;
    s.backLoadingScreens = backLoadingScreens;
    s.lastBackSeconds = lastBackSeconds;
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
      if (g.back_.to) g.back_.to = tick;  // (asked while a jump back runs ahead: that jump goes on to this beat)
      Logf(LogLevel::Info, "moho64ui: the replay timeline: forward from beat %u to beat %u", now, tick);
      return true;
    }
    // back: the replay starts over (the host's restart between two frames: Rewind, held; or RestartSession's Relaunch,
    // MOHO64UI_REWIND_RELOAD) and runs ahead to the beat
    g.seekTo_ = tick;
    g.seekBack_ = true;  // (not reached by this run of the replay: the next one runs ahead to it)
    g.back_ = BackClock{};
    g.back_.asked = std::chrono::steady_clock::now();
    g.back_.from = now, g.back_.to = tick;
    kept = keep;
    RequestSessionRestart();
    Logf(LogLevel::Info, "moho64ui: the replay timeline: back from beat %u to beat %u (the replay starts over)", now, tick);
    return true;
  }
  std::string TakeKept() override { return std::move(kept); }
  std::vector<ReplayBookmark> Bookmarks() const override { return bookmarks; }
  bool SetBookmarks(const std::vector<ReplayBookmark>& list, std::string* why) override {
    bookmarks = CleanBookmarks(list, ticks);
    if (!SaveReplayBookmarks(file, digest, bookmarks, why)) {
      Logf(LogLevel::Warning, "moho64ui: the replay's bookmarks are kept for this run only: %s", why->c_str());
      return false;
    }
    Logf(LogLevel::Info, "moho64ui: the replay's bookmarks: %zu, in %s", bookmarks.size(), hostfs::DisplayPath(BookmarksFile()).c_str());
    return true;
  }
  std::string BookmarksFile() const override { return ReplayBookmarksFile(file); }
  void CaptureForTests(const std::string& path) override { session->CaptureWindow(path); }
  void Watch(const std::vector<uint32_t>& ticks) override {
    GameSession& g = *session;
    std::lock_guard<std::mutex> lock(g.watchMutex_);
    g.watchTicks_ = std::set<uint32_t>(ticks.begin(), ticks.end());
    g.watched_.clear();
    g.watching_ = !g.watchTicks_.empty();
  }
  std::map<uint32_t, std::string> Watched() const override {
    std::lock_guard<std::mutex> lock(session->watchMutex_);
    return session->watched_;
  }
};

ReplayTimelineSource* GameSession::timeline() { return play_ && timeline_ ? timeline_.get() : nullptr; }

// The replay's length, and its moments looked for from now on: the engine's ScanReplayMoments (sim/moments.h) on a
// thread of its own, playing the replay a second time in a Sim of its own, mounted as this program mounted the game
// (scanInit, scanFolders, replayMaps), with the replay's mods and one worker. It goes at some 20 times the game's
// speed for a 1v1 and some 7 for a 4v4 (engine, 2026-10-23): the bar fills over minutes. scanTicks: it stops there
// (tests).
void GameSession::StartMomentScan(const ReplayHeader& header, const std::string& file, const std::vector<std::string>& uids) {
  StopMomentScan();
  const uint32_t ticks = ReplayBeats(playData_, header.streamOffset);
  Logf(LogLevel::Info, "moho64ui: the replay is %u beats long (%u:%02u); its moments are looked for", ticks, ticks / 600,
       (ticks / 10) % 60);
  timeline_ = NewScan(file, uids, ticks, scanTicks, "the replay's moments");
  // the player's bookmarks of it, from the file beside it (one of another replay's, as an earlier LastGame's, is left;
  // the scan's thread never reads these)
  Timeline& t = *timeline_;
  t.file = file;
  t.digest = ReplayDigest(playData_);
  std::string why;
  if (!LoadReplayBookmarks(file, t.digest, &t.bookmarks, &why))
    Logf(LogLevel::Warning, "moho64ui: the bookmarks in %s are not used: %s", hostfs::DisplayPath(ReplayBookmarksFile(file)).c_str(), why.c_str());
  else if (!t.bookmarks.empty())
    Logf(LogLevel::Info, "moho64ui: the replay's bookmarks: %zu, from %s", t.bookmarks.size(), hostfs::DisplayPath(ReplayBookmarksFile(file)).c_str());
}

std::unique_ptr<GameSession::Timeline> GameSession::NewScan(const std::string& file, const std::vector<std::string>& uids,
                                                            uint32_t ticks, int limit, const char* what) {
  auto made = std::make_unique<Timeline>();
  Timeline& t = *made;
  t.session = this;
  t.ticks = ticks;
  moho::ScanArgs args;
  args.init = scanInit;
  args.folders = scanFolders;
  args.maps = replayMaps;
  args.mounts = scanMounts;
  args.skipMounts = scanSkips;
  args.modUids = uids;
  args.replay = file;
  args.threads = 1;  // (the cores stay the watched game's)
  args.ticks = limit;
  t.limit = limit;
  t.scanning = true;
  const std::string name = what;
  t.scan = std::thread([&t, args, name] {
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
      Logf(LogLevel::Warning, "moho64ui: %s could not be looked for: %s", name.c_str(), error.c_str());
    else
      Logf(LogLevel::Info, "moho64ui: %s: %s at beat %d, %zu found in %.1f s (%.1f times the game's speed)", name.c_str(),
           through ? "scanned" : "the scan stopped", last, moments, secs, secs > 0 ? last / 10.0 / secs : 0.0);
  });
  return made;
}

void GameSession::StopMomentScan() {
  if (timeline_) timeline_->Stop();
  timeline_.reset();
}

// A scan let go of: told to stop, and kept until its thread is through (joined then, or with the session's host), so
// that leaving the score screen does not wait for it: a scan still loading its copy of the game (the rules, the map:
// seconds) stops only at its first beat.
void GameSession::LetGoOfScan(std::unique_ptr<Timeline> scan) {
  if (scan) {
    scan->cancel = true;
    stoppingScans_.push_back(std::move(scan));
  }
  for (size_t i = 0; i < stoppingScans_.size();) {
    if (stoppingScans_[i]->scanning) {
      ++i;
      continue;
    }
    stoppingScans_[i]->Stop();
    stoppingScans_.erase(stoppingScans_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

// A game played: its replay file as written so far (StartReplayFile, Record: every beat's orders flushed as they go)
// scanned from its start, with the game's own mods, to the end of what it holds. The file is read once when the scan
// begins (ScanReplayMoments), so the beats the game runs on after that are not in it.
ReplayTimelineSource* GameSession::GameMoments(bool start) {
  if (play_) return timeline();
  if (!sim_ || currentReplay.empty() || !replay_ || BeatRunning()) return postgame_.get();  // (a beat on its thread: asked again later)
  const uint32_t now = sim_->tick();
  // (asked again once the game is over, after a scan that read the file while the game still ran: made again)
  const bool stale = postgame_ && !postgame_->scanning && start && GameOver() && now > postgameAt_ + 1;
  if ((!postgame_ && (start || GameOver())) || stale) {
    LetGoOfScan(std::move(postgame_));
    postgameAt_ = now;
    Logf(LogLevel::Info, "moho64ui: the game's moments for its score screen: %s scanned to tick %u (%s)", currentReplay.c_str(), now,
         GameOver() ? "the game is over" : "the score screen asked while the game runs");
    postgame_ = NewScan(currentReplay, modUids_, now, 0, "the game's moments");
  }
  return postgame_.get();
}

void GameSession::SeekReached() {
  if (!seekTo_ || seekBack_ || !sim_ || BeatRunning()) return;
  if (sim_->tick() < seekTo_ && !(play_ && play_->AtEnd())) return;
  Logf(LogLevel::Info, "moho64ui: the replay timeline: at beat %u", sim_->tick());
  const bool wasBack = back_.to == seekTo_ && back_.to != 0;
  seekTo_ = 0;
  if (wasBack) {  // (a jump back, either way: its time and its parts, for the timeline and the log)
    const double total = BackSeconds();
    if (timeline_) ++timeline_->jumpsBack, timeline_->lastBackSeconds = total;
    const uint32_t ran = back_.aheadTick ? sim_->tick() - back_.aheadTick : 0;
    const double ahead = back_.aheadFrom >= 0 ? total - back_.aheadFrom : 0;
    Logf(LogLevel::Info,
         "moho64ui: the jump back from beat %u to beat %u (%s): at its beat %.2f s after it was asked (the session's end %.2f s, the load "
         "through at %.2f s, the run ahead from %.2f s: %u beats in %.2f s, %.0f a second)",
         back_.from, back_.to, rewinding_ ? "held" : "the replay's restart shown", total, back_.ended, back_.loaded, back_.aheadFrom, ran,
         ahead, ahead > 0 ? ran / ahead : 0.0);
    back_.to = 0;
  }
  if (rewinding_) rewindArrived_ = true;  // (the hold ends in the host's next frame: RewindArrived, EndRewind)
}

// --- the jump back, held (game_session.h Rewind) ---

double GameSession::BackSeconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - back_.asked).count(); }

bool GameSession::JumpBackDue() const {
  static const bool reload = [] {
    const char* v = std::getenv("MOHO64UI_REWIND_RELOAD");
    return v && *v && std::string(v) != "0";
  }();
  return !reload && seekBack_ && play_ && sim_ && lastWasReplay_;
}

void GameSession::FreezePicture() {
  frozenTaken_ = false;
  freezeWaits_ = 0;
  if (rewinding_) {  // (a jump back asked while one is held: the picture held stays; the new interface was never shown)
    freezeAsked_ = false;
    frozenTaken_ = true;
    return;
  }
#ifdef MOHO64UI_WORLD_PICTURE
  if (window && windowPicture_) {
    freezeAsked_ = true;
    return;
  }
#endif
  frozen_.clear(), frozenW_ = frozenH_ = 0;
  frozenTaken_ = true;  // (no picture in a window to hold: the jump goes on at once)
}

bool GameSession::PictureFrozen() const {
#ifdef MOHO64UI_WORLD_PICTURE
  if (freezeAsked_ && !(window && windowPicture_)) return true;  // (the picture went meanwhile)
#endif
  return frozenTaken_;
}

// (the window's frame read back as RGBA, kept as the interface's pixels are: opaque, so already premultiplied)
void GameSession::TakeFrozen(const std::vector<uint8_t>& rgba, int w, int h) {
  freezeAsked_ = false;
  frozenTaken_ = true;
  frozen_.clear(), frozenW_ = frozenH_ = 0;
  if (rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    Logf(LogLevel::Warning, "moho64ui: the jump back: the window's picture was not read back (it is held black)");
    return;
  }
  frozen_.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (size_t i = 0; i < frozen_.size(); ++i)
    frozen_[i] = 0xff000000u | static_cast<uint32_t>(rgba[i * 4]) << 16 | static_cast<uint32_t>(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2];
  frozenW_ = w, frozenH_ = h;
}

bool GameSession::Rewind(std::string* why) {
  rewinding_ = true;
  rewindArrived_ = false;
  KeepMapForRewind();
  if (!Relaunch(why)) {
    keepMap_ = false;
    EndRewind();
    return false;
  }
  Logf(LogLevel::Info, "moho64ui: the jump back: the replay starts over behind the picture held (%.2f s after the jump was asked)%s", back_.ended,
       keepMap_ ? ", on the map shown (no map opened again)" : "");
  return true;
}

// The same replay starts over on the same map: when the window's picture shows this game's map, the new session is
// begun on it at the load's end (WorldPicture::RestartMap, the renderer's answer to request 45 R1: the world as the map
// was opened, its ground, textures, pipelines and the models made kept), so that the load opens nothing behind (on a
// 4v4 2-3 s of each jump back went there: the same map opened again, the session's models made again). A menus' map
// begun behind the game that is over (the replay played to its end: ServeWindowMap) is let go of first, the game's looks
// laid back on. Ours.
void GameSession::KeepMapForRewind() {
  keepMap_ = false;
#ifdef MOHO64UI_WORLD_PICTURE
  static const bool reopen = [] {
    const char* v = std::getenv("MOHO64UI_REWIND_REOPEN");
    return v && *v && std::string(v) != "0";
  }();
  if (reopen || !wantPicture || !window || !windowPicture_ || !picture_ || !picture_->direct) return;
  using Kind = WindowPicture::Kind;
  WindowPicture& wp = *windowPicture_;
  if (wp.shows != Kind::Game || wp.shownMap != map_ || !wp.picture.world() || wp.pending == Kind::Game) return;
  if (wp.pending == Kind::Menu) {
    wp.picture.CancelMap();
    if (wp.menuBehindGame) GameLooksBack(wp);
    wp.pending = Kind::None;
  }
  keepMap_ = true;
#endif
}

// A game launched from the main menu on the map its flight shows (SETONS_R, the prime map: docs/ui-menuflight.md 4.3):
// the flight's world is taken over at the load's end (WorldPicture::RestartMap, OpenPicture) instead of the map being
// opened again behind the loading screen (some 6 s of the load there). The renderer's answer to ui request 49: R3,
// RestartMap is valid with a different session on the same map (the world keeps only the map's own things: ground,
// water, scenery; the game's armies and blueprints come with GameStarted as for any game); R2, what stays is what is
// fixed when the map opened: ray tracing's shaders, the texture detail, the season, the anti-aliasing (the picture's
// scene samples). So only when those are the game's: the flight's map opened with the player's ray tracing and Texture
// Detail (FlightLooks) and the player's Season (MenuSeason: an Autumn player's menu flies over autumn), and the window's picture not to be
// made anew for the game (the Graphics API and the anti-aliasing as it was made with: EnsureWindowPicture). Any other
// map, or a setting changed since the flight's map opened: opened again as before. MOHO64UI_FLIGHT_REOPEN=1: always
// opened again (the measure's other half). Ours, not the original's: its menu's background is a film.
void GameSession::KeepMenuMap(const std::string& map) {
  keepMenuMap_ = false;
#ifdef MOHO64UI_WORLD_PICTURE
  if (!windowPicture_ || map != MenuFlight::kMap) return;
  using Kind = WindowPicture::Kind;
  WindowPicture& wp = *windowPicture_;
  if (wp.shows != Kind::Menu || wp.shownMap != map) return;
  static const bool reopen = [] {
    const char* v = std::getenv("MOHO64UI_FLIGHT_REOPEN");
    return v && *v && std::string(v) != "0";
  }();
  const LookSettings looks = Looks();
  const WindowPicture::OpenedWith game{looks.textureDetail, looks.season == 1 ? 1 : 0, looks.rayTracing && wp.picture.RayTracingSupported()};
  std::string why;
  if (reopen) why = "MOHO64UI_FLIGHT_REOPEN";
  else if (!wp.picture.world()) why = "no world shown";
  else if (wp.pending == Kind::Game) why = "a game's map opening behind already";
  else if (wp.vulkan != looks.vulkan || wp.samples != looks.SceneSamples()) why = "the Graphics API or the anti-aliasing changed: the picture is made anew";
  else if (!(wp.shownWith == game)) {
    char b[160];
    std::snprintf(b, sizeof b, "opened with texture detail %d, season %d, ray tracing %d; the game's %d, %d, %d", wp.shownWith.textureDetail,
                  wp.shownWith.season, wp.shownWith.rays ? 1 : 0, game.textureDetail, game.season, game.rays ? 1 : 0);
    why = b;
  }
  if (!why.empty()) {
    ++handoversReopened_;
    handoverWhy_ = why;
    Logf(LogLevel::Info, "moho64ui: the game's map is the menu's flight's: it is opened again (%s)", why.c_str());
    return;
  }
  if (wp.pending == Kind::Menu) wp.picture.CancelMap(), wp.pending = Kind::None;  // (a menus' map opening behind: not wanted now)
  ++handoversKept_;
  handoverWhy_.clear();
  keepMap_ = keepMenuMap_ = true;
  Logf(LogLevel::Info, "moho64ui: the game's map is the menu's flight's: its world is taken over at the load's end (no map opened again)");
#else
  (void)map;
#endif
}

// The beat is reached (or the replay closes): the picture held goes (PresentToWindow: the world shown again, the
// interface over it), and the interface is stepped and drawn as ever from then on.
void GameSession::EndRewind() {
  rewinding_ = rewindArrived_ = false;
  freezeAsked_ = frozenTaken_ = false;
  std::vector<uint32_t>().swap(frozen_);
  frozenW_ = frozenH_ = 0;
}

void GameSession::NoteLoadingScreen() {
  if (seekTo_ && timeline_) ++timeline_->backLoadingScreens;
}

int GameSession::JumpBackPercent() const {
  const uint32_t to = seekTo_ ? seekTo_ : back_.to;
  if (!back_.to || !sim_ || loadSession_ || seekBack_ || to == 0) return -1;  // (the replay still to start over, or loading)
  return static_cast<int>(std::min<uint32_t>(100, static_cast<uint32_t>(100.0 * sim_->tick() / to)));
}

// The host's note over the held picture: where the jump goes, and how far the run ahead has got.
void GameSession::DrawHeld(DrawTarget& target) const {
  if (!rewinding_) return;
  const uint32_t to = seekTo_ ? seekTo_ : back_.to;
  char text[96];
  const int percent = JumpBackPercent();
  if (percent >= 0)
    std::snprintf(text, sizeof text, "Rewinding to %u:%02u ...  %d%%", to / 600, (to / 10) % 60, percent);
  else
    std::snprintf(text, sizeof text, "Rewinding to %u:%02u ...", to / 600, (to / 10) % 60);
  const int size = 20;
  const FontMetrics m = GetFontMetrics("Arial", size);
  const float w = StringAdvance("Arial", size, text);
  // (in the middle of the window, a little below the centre: the game's own pause and wait notes stand above it)
  const float W = static_cast<float>(target.Width()), H = static_cast<float>(target.Height());
  const float x = std::floor((W - w) * 0.5f), y = std::floor(H * 0.58f);
  target.FillRect(Rect{x - 18, y - 12, x + w + 18, y + m.height + 12}, 0xc0101820u);
  DrawString(target, "Arial", size, text, x, y + m.ascent, 0xffe0e8f0u);
}

GameSession::GameSession(Vfs* vfs, std::vector<std::string> hookDirs) : vfs_(vfs), hookDirs_(std::move(hookDirs)) {}
GameSession::~GameSession() {
  StopLoadAhead();
  if (worker_) worker_->Wait();
  StopMomentScan();
}

WorldSource* GameSession::world() const { return picture_.get(); }

bool GameSession::BeatRunning() const { return worker_ && worker_->Running(); }
bool GameSession::GameOver() const { return sim_ && !play_ && !BeatRunning() && (pace.ended || sim_->endGame); }
int GameSession::SyncsOwed() const {
  return (BeatRunning() || beatToFinish_ ? 1 : 0) + static_cast<int>(syncsToGive_.size());
}

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

// A network game's beats (LaunchNetwork): the lockstep over the lobby's connector, and the session's clients.
struct GameSession::Net {
  std::unique_ptr<net::NetBeatExchange> exchange;
  std::unique_ptr<Lockstep> lockstep;
  std::vector<NetClient> clients;  // (in client order; their links asked when shown: NetClients)
  uint8_t local = 0;
  bool readySent = false;      // (NetReady: READY sent, the lockstep started)
  bool everyoneReady = false;  // (every client READY or ejected: the beats run)
  bool waitingShown = false;   // (the provider's waiting dialog asked for)
  NetEvents events;            // (for the interface's next frame: NetGameLink::TakeEvents)
  std::atomic<uint32_t> corruptAt{0};  // (moho64ui_NetTestForTests("corrupt", beat): our checksum of that beat changed)
  std::unique_ptr<NetGameLink> link;
  bool waiting = false;
  std::chrono::steady_clock::time_point waitingSince{};
  bool waitTold = false;
  uint32_t beat = 0;      // the beat whose orders were taken last
  bool afterDue = false;  // (that beat has run, or runs on its thread: AfterBeat is due)
  std::string digest;     // its state's digest, every 50th beat (made on the thread that ran it, read after it)
  // (ours, after a desync: NetSendParts / NetCompareParts) this copy's state line of the last checksum beats (the
  // engine's Fingerprint::StateLine without its lua part: the line LockstepDigest hashes), and of each of the last
  // beats its pulse (the random draws so far, the units, the projectiles: a few numbers read, for the first beat a
  // desync shows in). Both made in NetDigest on the thread that ran the beat and read here after it, as `digest`.
  std::vector<std::pair<uint32_t, std::string>> lines;
  struct Pulse {
    uint32_t beat = 0;
    uint64_t draws = 0;
    uint32_t units = 0, projectiles = 0;
  };
  std::array<Pulse, 128> pulses{};
  std::map<uint8_t, uint32_t> firstDesync;           // per client, the checksum beat it was first out of step at
  std::set<std::pair<uint8_t, uint32_t>> partsSent;  // (client, beat)
  bool desyncFileStarted = false;
  std::string detailPath;  // where the full listing of beat detailAt goes (asked for at the game's first desync)
  uint32_t detailAt = 0;
  bool detailAsked = false;
  // the test switches that change this copy's state or its line (NetTestSwitch; set before the game's beats come)
  std::string corruptPart;
  std::atomic<uint32_t> moveAt{0}, drawAt{0};
  std::atomic<int> moveArmy{0};
  // MOHO64UI_NET_BEAT_LOG=1 (tests): a line per beat, when it began, how long it waited for its orders and how much
  // of that with every client's orders in (the wait for the ACKs); see NetBeatOrders
  std::chrono::steady_clock::time_point askedAt{}, inAt{}, lastBegun{};
  uint32_t askedFor = 0;
  bool inSeen = false;
  // (message 57, this program's maxSP: GameSession::measure_, the same in every game)
};

// The interface's side of it (user/user_state.h): what GetSessionClients, EjectSessionClient, SessionSendChatMessage
// and SetGameSpeed ask of the game, and the events for the next frame.
class NetLinkOf final : public NetGameLink {
 public:
  explicit NetLinkOf(GameSession* session) : session_(session) {}
  std::vector<SessionClient> Clients() override {
    std::vector<SessionClient> out;
    for (const GameSession::NetClient& c : session_->NetClients()) {
      SessionClient s;
      s.name = c.name, s.uid = c.uid, s.connected = c.connected, s.local = c.local, s.ping = c.ping, s.quiet = c.quiet;
      s.sources = c.sources, s.ejectedBy = c.ejectedBy, s.maxSP = c.maxSP;
      out.push_back(std::move(s));
    }
    return out;
  }
  void Eject(int client) override { session_->NetEject(client); }
  void SendChat(const std::vector<int>& clients, const std::string& message) override { session_->NetChat(clients, message); }
  void RequestSpeed(int rate) override { session_->NetSpeed(rate); }
  NetEvents TakeEvents() override { return session_->NetTakeEvents(); }
  void TestSwitch(const std::string& what, uint32_t beat, const std::string& arg) override {
    session_->NetTestSwitch(what, beat, arg);
  }

 private:
  GameSession* session_;
};

uint32_t GameSession::InputDelay(int rate) {
  const double msPerBeat = 100.0 / std::pow(10.0, rate / 10.0);
  return static_cast<uint32_t>(std::floor(kNetLagMs / msPerBeat + 1e-9));
}

bool GameSession::Launch(lua_State* L, int config, std::string* why) { return LaunchWith(L, config, nullptr, why); }

bool GameSession::LaunchNetwork(lua_State* L, int config, net::LobbyCore* lobby, std::string* why) {
  return LaunchWith(L, config, lobby, why);
}

// (lobby: a network game's lobby; nullptr: a local game)
bool GameSession::LaunchWith(lua_State* L, int config, net::LobbyCore* lobby, std::string* why) {
  if (config < 0) config = lua_gettop(L) + config + 1;
  if (sim_ || Loading()) {
    *why = "a session is running already";
    return false;
  }
  auto t0 = std::chrono::steady_clock::now();
  SessionDescription session;
  LaunchParams params;
  params.seed = seedGiven ? seed : static_cast<uint32_t>(t0.time_since_epoch().count());
  if (lobby) {  // (the launch info: the seed is the host's nonce, lobby+0xc4; the focus army by our uid, lobby+0xac)
    params.seed = lobby->Nonce();
    params.localOwner = lobby->Uid();
    params.network = true;
  }
  // The simulation's order (Chris, 2026-10-31, multicore option 2: our parallel-friendly order for new games, the
  // original's kept for the original's replays): the lobby's option RemasterSimOrder ("parallel" / "original"); with
  // "parallel" its version beside it, as src/sim/replay.h ScenarioSimOrder reads them. Every client of a network game
  // adds it alike from the same options, and the record keeps both, so its replay plays in the order it was made in.
  lua_getfield(L, config, "GameOptions");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "RemasterSimOrder");
    const bool parallel = lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "parallel";
    lua_pop(L, 1);
    lua_getfield(L, -1, "RemasterSimOrderVersion");
    const bool hasVersion = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (parallel && !hasVersion) {
      lua_pushstring(L, "RemasterSimOrderVersion");
      lua_pushnumber(L, moho::Sim::kSimOrderVersion);  // (this engine's parallel order version)
      lua_rawset(L, -3);
    }
  }
  lua_pop(L, 1);
  if (!SessionFromGameConfig(L, config, params, &session, why)) return false;

  // The session's clients (CLobby::LaunchGame 0x7c38c0): the human armies in army order, then the observers, each
  // owner given a client index (AssignClientIndex 0x7c4e80, called at 0x7c468c / 0x7c4949) and a command source
  // (AssignCommandSource 0x7c4f60, at 0x7c46b8 / 0x7c497d) on its first appearance, both counted from 0 in the same
  // walk: so client i speaks for source i, and the host is client 0 only when it has the first human army.
  std::unique_ptr<Net> net;
  if (lobby) {
    net = std::make_unique<Net>();
    std::vector<int> owners;
    auto note = [&](int owner, const std::string& name) {
      for (int o : owners)
        if (o == owner) return;
      NetClient c;
      c.uid = owner, c.name = name, c.client = static_cast<uint8_t>(owners.size());
      c.sources.push_back(static_cast<int>(owners.size()));
      c.local = owner == lobby->Uid();
      owners.push_back(owner);
      net->clients.push_back(c);
    };
    auto ownerOf = [&](int t) {  // (OwnerID is a string read with _atol, 0x7c461c; PlayerName beside it)
      lua_getfield(L, t, "OwnerID");
      const int owner = lua_isstring(L, -1) ? std::atoi(lua_tostring(L, -1)) : 0;
      lua_getfield(L, t, "PlayerName");
      const std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
      lua_pop(L, 2);
      note(owner, name);
    };
    const int top = lua_gettop(L);
    for (const ReplayHeader::Army& a : session.header.armies) {
      if (a.sources.empty()) continue;  // (not human: no client, no source)
      if (PushSerializedLua(L, a.options) && lua_istable(L, -1)) ownerOf(lua_gettop(L));
      lua_settop(L, top);
    }
    lua_getfield(L, config, "Observers");
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, top + 1) != 0) {
        if (lua_istable(L, -1)) ownerOf(lua_gettop(L));
        lua_pop(L, 1);
      }
    }
    lua_settop(L, top);
    if (net->clients.size() != session.header.sources.size())
      Logf(LogLevel::Warning, "moho64ui: the network game has %zu clients but %zu command sources", net->clients.size(),
           session.header.sources.size());
    bool inIt = false;
    for (const NetClient& c : net->clients)
      if (c.local) inIt = true, net->local = c.client;
    if (!inIt) {  // INFERRED: the original would go on with no client of its own (index -1); we refuse
      *why = "this player is neither a player nor an observer of the game";
      return false;
    }
    // The lobby's network goes to the game; each other client is the lobby's connection to its uid (none: lost).
    net::GameLink link = lobby->TakeForGame();
    std::vector<net::GameClient> peers;
    std::vector<LockstepClient> all;
    for (const NetClient& c : net->clients) {
      all.push_back(LockstepClient{c.client, c.sources});
      if (c.local) continue;
      auto it = link.connByUid.find(c.uid);
      peers.push_back(net::GameClient{c.client, it != link.connByUid.end() ? it->second : 0});
      if (it == link.connByUid.end())
        Logf(LogLevel::Warning, "moho64ui: the network game's client %u (%s, uid %d) has no connection", c.client, c.name.c_str(),
             c.uid);
    }
    net->exchange = std::make_unique<net::NetBeatExchange>(std::move(link.connector), net->local, std::move(peers),
                                                           session.adjustableSpeed, session.gameSpeed);
    net->exchange->TakeEarly(std::move(link.early));
    const uint32_t delay = InputDelay(session.gameSpeed);
    net->lockstep = std::make_unique<Lockstep>(net->exchange.get(), net->local, std::move(all), delay);
    net->link = std::make_unique<NetLinkOf>(this);
    Logf(LogLevel::Info, "moho64ui: a network game: %zu clients, this one is client %u (uid %d); input delay %u beats; seed %u",
         net->clients.size(), net->local, lobby->Uid(), delay, session.header.seed);
  }

  // The session's mods are the configuration's GameMods, by uid, in order.
  lua_getfield(L, config, "GameMods");
  const std::vector<std::string> uids = ModUids(L, lua_gettop(L));
  lua_pop(L, 1);

  // The day and the weather: the game's options where it has them (the lobby's Remaster...
  // options), the host's own otherwise.
  skyNow_ = sky;
  seed_ = session.header.seed;
  lua_getfield(L, config, "GameOptions");
  aheadable_ = true;  // (see Aheadable)
  if (lua_istable(L, -1)) {
    ReadLookOptions(L, lua_gettop(L));
    lua_getfield(L, -1, "PrebuiltUnits");
    if (lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "On") aheadable_ = false;
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  // (a network game runs no beats behind the loading screen (those are ours): its beats wait for every client's READY,
  // which each one sends once its game screen is up)
  if (lobby) aheadable_ = false;
  profile_ = ProfileName(L);
  playData_.clear();
  play_.reset();
  PrepareSession(session, uids);
  net_ = std::move(net);
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = false;
  lastWasNetwork_ = net_ != nullptr;
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
  // The simulation's order of a new operation (no lobby to choose it: Options > Gameplay's "Simulation Order for New
  // Games", our own order unless chosen; the engine, 2026-11-03), and its version, as LaunchWith gives a lobby's game.
  lua_getfield(L, info, "scenarioInfo");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "Options");
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushstring(L, "Options");
      lua_pushvalue(L, -2);
      lua_rawset(L, -4);
    }
    lua_getfield(L, -1, "RemasterSimOrder");
    const bool given = lua_isstring(L, -1);
    lua_pop(L, 1);
    if (!given) {
      std::string order = "parallel";
      lua_getglobal(L, "GetOptions");
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "remaster_sim_order");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_isstring(L, -1)) order = lua_tostring(L, -1);
      }
      lua_pop(L, 1);
      lua_pushstring(L, "RemasterSimOrder");
      lua_pushstring(L, order == "original" ? "original" : "parallel");
      lua_rawset(L, -3);
      if (order != "original") {
        lua_pushstring(L, "RemasterSimOrderVersion");
        lua_pushnumber(L, moho::Sim::kSimOrderVersion);  // (this engine's parallel order version)
        lua_rawset(L, -3);
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (!SessionFromSinglePlayerInfo(L, info, params, &session, why)) return false;
  lua_getfield(L, info, "scenarioMods");
  const std::vector<std::string> uids = ModUids(L, lua_gettop(L));
  lua_pop(L, 1);
  skyNow_ = sky;
  seed_ = session.header.seed;
  aheadable_ = false;  // (a campaign's operation: its opening plays from beat 1)
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
  lastWasNetwork_ = false;
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
  if (edge == "frame" || edge == "beyond" || edge == "surround") skyNow_.edge = edge;
}

namespace {
// What a replay needs before its load may begin, looked at in its header alone (LaunchReplay): "" when it can start,
// else why not. Until 2026-11-10 only the header's own form was read before the load began: a replay of a map that
// is not there, or whose lists were damaged, failed later, on the loading thread ("the simulation did not start"),
// with nothing for the player but the menu coming back; one whose mods are not there played without them, a
// different game from the first beat. Refused here, LaunchReplaySession answers false and the game's own Replay
// dialog says so (/lua/ui/dialogs/replay.lua 69, "Unable to launch replay"); the reason goes to the log.
// The original's own checks before its session begins (VCR_SetupReplaySession 0x8765e0): the file opens (0x87664d);
// its version line is this exe's own (0x876678-0x8766d7: ours plays 3839 and 3840 and tries any other by 3839's
// rules with a warning, the engine's rule in Sim::Start, so that is no refusal here); the 13 bytes after it are
// "Replay v1.9" and its line end (read at 0x8767a1, compared with the text at 0xdfe640 up to 0x87681d). The map, the lists and the mods are OURS: the original starts
// such a replay and fails or plays wrong in its load (INFERRED: not read there).
std::string ReplayCannotStart(lua_State* L, const Vfs& vfs, const ReplayHeader& h) {
  if (h.replayVersion != "Replay v1.9")
    return "it is a recording of another format (\"" + h.replayVersion + "\"; this game plays \"Replay v1.9\")";
  const int top = lua_gettop(L);
  auto text = [&](int table, const char* key) {
    lua_getfield(L, table, key);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
  };
  // (the scenario: the session's ScenarioInfo, and the files the simulation's start reads by it)
  if (!PushSerializedLua(L, h.scenario) || !lua_istable(L, -1)) {
    lua_settop(L, top);
    return "its scenario cannot be read (the file is damaged)";
  }
  const std::string save = text(top + 1, "save"), script = text(top + 1, "script");
  lua_settop(L, top);
  if (h.map.empty()) return "it names no map";
  if (!vfs.Exists(h.map)) return "its map " + h.map + " is not installed";
  for (const std::string& part : {save, script})
    if (!part.empty() && !vfs.Exists(part)) return "its map's file " + part + " is missing";
  // (the mods: each one's table as the launch had it, with the folder its files are read from; a game without mods
  // holds an empty table, or nothing. A mod of the interface alone is no part of the simulation.)
  if (h.mods.empty()) return "";
  if (!PushSerializedLua(L, h.mods) || !lua_istable(L, -1)) {
    lua_settop(L, top);
    return "its list of mods cannot be read (the file is damaged)";
  }
  std::string missing;
  int count = 0;
  for (int i = 1;; ++i) {
    lua_rawgeti(L, top + 1, i);
    if (!lua_istable(L, -1)) break;
    const int mod = lua_gettop(L);
    lua_getfield(L, mod, "ui_only");
    const bool uiOnly = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    const std::string location = text(mod, "location");
    if (!uiOnly && !location.empty() && !vfs.Exists(location + "/mod_info.lua")) {
      std::string name = text(mod, "name");
      if (name.empty()) name = text(mod, "uid");
      missing += (count++ ? ", " : "") + name + " (" + location + ")";
    }
    lua_pop(L, 1);
  }
  lua_settop(L, top);
  if (count) return std::string(count == 1 ? "its mod " : "its mods ") + missing + (count == 1 ? " is" : " are") + " not installed";
  return "";
}
}  // namespace

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
  if (const std::string cannot = ReplayCannotStart(L, *vfs_, *h); !cannot.empty()) {
    *why = "the replay cannot start: " + cannot;
    return false;
  }
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
  aheadable_ = false;
  playData_ = std::move(*data);
  play_.reset();
  PrepareSession(session, uids);
  play_ = std::make_unique<CommandStream>(playData_, h->streamOffset);
  ReplayHeld() = false;  // (a new replay plays: no hold of a replay watched before)
  seekTo_ = 0;
  seekBack_ = false;
  back_ = BackClock{};
  StartMomentScan(*h, file, uids);
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = true;
  lastWasNetwork_ = false;
  Logf(LogLevel::Info, "moho64ui: playing the replay %s (%zu bytes, %zu armies)", file.c_str(), playData_.size(), h->armies.size());
  return true;
}

// The replay file of a game being played (LastGame), the original's format (the header as the
// engine's ReadReplayHeader reads it, sim/replay.cpp, then the command stream): so the game's
// Replay dialog lists it and the original game could open it too.
void GameSession::StartReplayFile(const SessionDescription& session, const Sim& sim) {
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
  if (sim.fixes) {
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
  StopLoadAhead();  // (beats in the load: the loading thread stops at the hand-over or after its beat)
  if (worker_) worker_->Wait();  // (a beat on its thread ends first; its sounds and sync are dropped)
  loadAhead_.reset();
  recordsOpen_ = false;
  beatToFinish_ = false;
  aheadTo_ = 0, aheadRanTo_ = 0;
  load_ = Load::None;  // (a launch still loading is dropped)
  loadSim_.reset();
  loadSession_.reset();
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  if (fp_) fp_->End(fpBeats_), fp_.reset();
  if (replay_) std::fclose(replay_), replay_ = nullptr;
  play_.reset();
  LetGoOfScan(std::move(postgame_));  // (a played game's moments' scan stops: GameMoments)
  if (!relaunching_) {  // (the replay closes: its moments' scan stops, a jump under way with it)
    // (Let go of, not waited for, as the played game's scan above: a scan still loading its copy of the game looks
    // at `cancel` only from its first beat on, so a replay left in its first seconds held the window for the whole
    // of that load; the review of 2026-11-09. The bookmarks are in their file already: written at each change.)
    LetGoOfScan(std::move(timeline_));
    seekTo_ = 0;
    seekBack_ = false;
    back_ = BackClock{};
    keepMap_ = keepMenuMap_ = false;
    EndRewind();
  }
  if (sounds) {
    sounds->StopWorld();
    if (int n = sounds->ResumeAll()) Logf(LogLevel::Info, "moho64ui: sound: %d paused categor%s resumed at the session's end", n, n == 1 ? "y" : "ies");
  }
  onSync = nullptr;
  takeOrders = nullptr;
  net_.reset();  // (its connections close: the other players hear us gone)
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
  pendingSync_.clear();
  setupSync_.clear();
  foreignLoops_.clear();
  heardLoops_.clear();
  goneLoopsSaid_ = 0;
  syncsToGive_.clear();
  beatSounds_.clear(), beatSoundsDecided_ = false;
  due_ = 0;
  pace = GamePace();
}

bool GameSession::Relaunch(std::string* why) {
  if (!last_) {
    *why = "no game was launched";
    return false;
  }
  if (lastWasNetwork_) {  // (FAF's scripts offer no restart in a multiplayer game: chat command Restart.lua)
    *why = "a network game cannot be started over";
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
    if (back_.to) back_.ended = BackSeconds();  // (a jump back's clock, either way: SeekReached's line)
    if (seekTo_) Logf(LogLevel::Info, "moho64ui: the replay starts over and runs ahead to beat %u", seekTo_);
    return true;
  }
  if (!seedGiven) again.header.seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  PrepareSession(again, lastUids_);
  tableAgain_ = true;  // (no lobby writes this game's table: the record's start copies the last one)
  return true;
}

namespace {
std::string Numbered(const std::string& prefix, int k) { return k <= 1 ? prefix : prefix + "." + std::to_string(k); }
}  // namespace

int GameSession::RecordIndex() const {
  for (int k = lastIndex_ + 1;; ++k) {
    const std::string base = Numbered(replayOut, k);
    if (!hostfs::IsFile(base + ".stream") && !hostfs::IsFile(base + ".seed")) return k;
  }
}

std::string GameSession::TableFile() const {
  if (!numberTables) return replayTable;
  const int k = RecordIndex();
  return k <= 1 ? replayTable : Numbered(replayOut, k) + ".session.lua";
}

// The simulation's print lines of a beat, to the game's console (the original: Sim::Printf 0x7462a0 keeps them,
// the beat's sync carries them, the interface's side prints each with CON_Printf 0x895260; engine: Sim::TakePrints,
// ui REQUESTS 41). Posted: they reach the console's receivers on the main thread's next frame.
static void PostPrints(Sim& sim) {
  for (const std::string& line : sim.TakePrints()) ConsolePost(line);
}

// The launch call's part (the original's WLD_SetupSessionInfo + WLD_BeginSession, 0x88cbc0 /
// 0x88d060): the session is described and its simulation made, nothing loaded yet.
// The remastered units of the mods this game has on (--remasters-mods, visuals/work_mods: its MODS.json names each
// mod's folder and mod_uid; visuals, 2026-11-04; Chris's item 22 of 2026-10-20: no mod unit's look unless its mod is
// on), mounted as the game begins, in the game's own order of its mods: of two mods that define one unit the later
// wins, as the later's blueprint does (INFERRED: the order the session lists them; Antares and Wyvern both define
// WES0401). A folder once mounted stays for the later games of the run (the file system has no unmount): only such an
// overlap could then show the earlier game's look. A unit's folder with a HOLD is skipped (MountRemasters).
void GameSession::MountModRemasters(const std::vector<std::string>& uids) {
#ifdef MOHO64UI_WORLD_PICTURE
  if (remastersModsRoot.empty() || uids.empty()) return;
  const std::string file = remastersModsRoot + "/MODS.json";
  const std::optional<std::string> text = hostfs::ReadFile(file);
  render::JsonValue json;
  std::string error;
  if (!text || !render::ReadJson(*text, &json, &error)) {
    Logf(LogLevel::Warning, "moho64ui: --remasters-mods: %s cannot be read%s%s", hostfs::DisplayPath(file).c_str(),
         error.empty() ? "" : ": ", error.c_str());
    return;
  }
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  for (const std::string& uid : uids) {
    for (const auto& [key, mod] : json["mods"].members) {
      const std::string modUid = mod["mod_uid"].StringOr("");
      if (modUid.empty() || lower(modUid) != lower(uid) || !modRemastersMounted_.insert(key).second) continue;
      // (first whoever still reads the file system on another thread is waited for: the mount changes its list, and
      // a launcher that starts a game at once came here while the start-up's readers ran, the host's own wait for
      // them being a frame later, at the load: the review's row G, 2026-11-10)
      if (beforeMount) beforeMount();
      const std::string folder = remastersModsRoot + "/" + key;
      Logf(LogLevel::Info, "moho64ui: remastered units of the mod %s: %d from %s", key.c_str(), render::MountRemasters(vfs_, folder),
           hostfs::DisplayPath(folder).c_str());
    }
  }
#else
  (void)uids;
#endif
}

void GameSession::PrepareSession(const SessionDescription& session, const std::vector<std::string>& uids) {
  // (the main menu's flight ends first: one simulation at a time, and the file system's mounts change below; its world
  // stays shown, hidden under the loading screen, until the game's map is swapped in)
  if (menuFlight) {
    menuFlight->Stop();
    flightGiven_ = flightShows_ = flightCovering_ = false;
    if (sounds) sounds->StopWorld();
  }
  MountModRemasters(uids);  // (before the picture opens behind the loading screen: its units' looks are read then)
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session %s: %zu armies, %zu command sources, focus army %d, seed %u, %zu mods",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, uids.size());
  launchAt_ = std::chrono::steady_clock::now();
  loadSession_ = std::make_unique<SessionDescription>(session);
  {
    ThreadsOverride threads(simThreads);  // (the pool is made with the Sim)
    loadSim_ = std::make_unique<Sim>(vfs_, hookDirs_, uids);
  }
  Logf(LogLevel::Info, "moho64ui: the session's simulation made (its worker pool with it) in %.2f s",
       std::chrono::duration<double>(std::chrono::steady_clock::now() - launchAt_).count());
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
  loadBegan_ = std::chrono::steady_clock::now();
  loadThreadDone_ = loadHandedAt_ = loadMapReadyAt_ = -1;
  loadStateSeconds_ = 0;
  loadNextNote_ = 10;
  Logf(LogLevel::Info, "moho64ui: the load begun, %.2f s after the launch call", std::chrono::duration<double>(loadBegan_ - launchAt_).count());
  {
    LongFrameLog& lf = LongFrames();  // (MOHO64UI_LONG_FRAMES: what the renderer makes off the frames' thread meanwhile)
    lf.asides.clear(), lf.longestAsides.clear();
    lf.loading = lf.thresholdMs > 0;
  }
  if (!worker_) worker_ = std::make_unique<BeatWorker>();
  recordsOpen_ = false;
  loadAhead_.reset();
  if (aheadInLoad && aheadable_ && !net_ && !play_) {  // (only a local game's: Aheadable)
    loadAhead_ = std::make_unique<LoadAhead>();
    loadAhead_->decideSounds = sounds != nullptr;
  }
  worker_->Start([this] {
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    // (the job's end, whichever way it ends, for a wait at the hand-over: WaitForWorker)
    struct Done {
      LoadAhead* a;
      ~Done() {
        if (!a) return;
        std::lock_guard<std::mutex> lock(a->mutex);
        a->jobDone = true;
        a->cv.notify_all();
      }
    } done{loadAhead_.get()};
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
    sim->keepSnapshots = true;          // building sites and reclaimables copied at each sync point (UserState::BuildSite)
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
    // Then the interface's own state for the session, here too (the original's "Map loader" task, after its rules:
    // the session's user Lua state made, 0x885ec9, its functions registered, 0x885f37..0x885f8a, the rules exported
    // into it, 0x885ff2, and /lua/SessionInit.lua run in it, 0x88603e); the main thread only takes it (FinishLoad).
    const auto t2 = std::chrono::steady_clock::now();
    if (prepareInterface) prepareInterface(sim, loadUids_, loadSession_->header.mods);
    const auto t3 = std::chrono::steady_clock::now();
    loadStateSeconds_ = secs(t2, t3);
    // Then, for an aheadInLoad game, its first beats (game_session.h): after the hand-over, while the map still opens.
    if (loadAhead_) RunLoadAhead(sim);
    loadThreadDone_ = secs(loadBegan_, std::chrono::steady_clock::now());  // (read on the main thread once the worker is through: its release)
  });
  // Meanwhile the game's map opens behind the window's picture (render request 37), the loading screen drawn over
  // what it shows; FinishLoad swaps it in.
#ifdef MOHO64UI_WORLD_PICTURE
  if (windowPicture_) windowPicture_->failedGameMap.clear();
#endif
  if (wantPicture && window && !keepMap_) KeepMenuMap(loadSession_->header.map);  // (the menu's flight's map: taken over)
  if (wantPicture && window && !keepMap_) BeginWindowMap(true, loadSession_->header.map);  // (a jump back: KeepMapForRewind)
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
  if (net_) net_->exchange->Service();  // (a network game: the others keep hearing from this program while it loads)
  NoteLoadProgress();
  // (aheadInLoad: the loading thread waits for this one before its beats, HandOverLoad. The map opening behind is given
  // the session at that hand-over, at tick 0, while the thread is parked (no beat running: the renderer's rule 1, ui
  // REQUESTS 46), so that the map opens WHILE the beats run; its first beat after the swap takes beats 1..29's hits,
  // health and props. Until render 8ba6be79 (ui REQUESTS 47) the renderer numbered its world's lists by the blueprints'
  // places in tick 0 and its main frames afresh at tick 29, and a commander was drawn with another unit's model and
  // icon (SETONS_R, tick 70); it now keeps the numbering across AdoptArmies.)
  const bool parked = LoadParked();
#ifdef MOHO64UI_WORLD_PICTURE
  if (load_ == Load::Running && !loadPrepared_ && (!BeatRunning() || parked) && loadOk_ && loadSim_ && windowPicture_ &&
      windowPicture_->pending == WindowPicture::Kind::Game) {
    loadPrepared_ = true;
    // (and the camera the game will first be seen by, the whole map from above for its first second, for the warm
    // frames: the renderer's own is a nearer one over the map's middle)
    if (const TerrainMap* map = loadSim_->map()) {
      const float mw = static_cast<float>(map->width()), mh = static_cast<float>(map->height());
      const WindowPicture& wp = *windowPicture_;
      const float aspect = wp.drawnH > 0 ? static_cast<float>(wp.drawnW) / static_cast<float>(wp.drawnH) : 16.0f / 9.0f;
      const WorldCamera start = StartWorldCamera(mw, mh, loadSim_->playableRect, aspect, map->SurfaceHeight(mw * 0.5f, mh * 0.5f));
      windowPicture_->picture.SetLoadCamera(Picture::ToRender(start));
    }
    if (windowPicture_->picture.PrepareSession(*loadSim_)) {
      loadHandedAt_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadBegan_).count();
      Logf(LogLevel::Info, "moho64ui: the game's map opening behind makes the simulation's models ahead (%.2f s into the load)", loadHandedAt_);
    }
  }
#endif
  if (parked) HandOverLoad();
}

// (the load's parts as this thread's frames see them: the loading thread through, the map opening behind ready; while it
// loads, a line every 10 s naming what it still waits for)
void GameSession::NoteLoadProgress() {
  if (load_ != Load::Running) return;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadBegan_).count();
  const bool threadDone = !BeatRunning();
  bool behind = false, ready = false;
#ifdef MOHO64UI_WORLD_PICTURE
  behind = windowPicture_ && windowPicture_->pending == WindowPicture::Kind::Game;
  ready = behind && (windowPicture_->picture.MapReady() || windowPicture_->picture.MapFailed());
  if (ready && loadMapReadyAt_ < 0) loadMapReadyAt_ = now;
#endif
  if (now < loadNextNote_) return;
  loadNextNote_ += 10;
  std::string thread = threadDone ? "through" : "at work";
  if (threadDone && loadThreadDone_ >= 0) {
    char at[48];
    std::snprintf(at, sizeof at, " (at %.2f s)", loadThreadDone_);
    thread += at;
  }
  std::string map = !behind ? "none opening behind" : ready ? "ready" : loadHandedAt_ >= 0 ? "not ready (given the session)" : "not ready";
  Logf(LogLevel::Info, "moho64ui: loading for %.0f s: the loading thread (rules, the simulation's start, the game's state) %s; the map behind: %s",
       now, thread.c_str(), map.c_str());
}

// --- aheadInLoad: the first beats on the loading thread (game_session.h) ---

bool GameSession::LoadParked() const {
  if (!loadAhead_) return false;
  std::lock_guard<std::mutex> lock(loadAhead_->mutex);
  return loadAhead_->gate == LoadAhead::Gate::Parked;
}

// This thread's part of the hand-over (ServeLoad): the game's record and replay file opened now, so that the loading
// thread writes each beat's advance and fingerprint line as it runs (no orders in those beats: the same bytes
// RunFirstBeat and RunAhead write); the beats' sounds start from no running sound (End stopped the world's). Then it
// goes on. (The map opening behind gets the session just before this, at tick 0: ServeLoad.)
void GameSession::HandOverLoad() {
  LoadAhead& a = *loadAhead_;
  OpenRecords(*loadSession_, *loadSim_);
  loopEntities_.clear(), seekingAtBeat_ = false;
  std::lock_guard<std::mutex> lock(a.mutex);
  a.gate = LoadAhead::Gate::Go;
  a.goAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadBegan_).count();
  a.cv.notify_all();
}

void GameSession::StopLoadAhead() {
  if (!loadAhead_) return;
  LoadAhead& a = *loadAhead_;
  a.stop.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(a.mutex);
  if (a.gate != LoadAhead::Gate::Go) a.gate = LoadAhead::Gate::Stop;
  a.cv.notify_all();
}

// On the loading thread, right after the simulation's start and the interface's state: the setup's sync taken (it is
// Start's own, "beat 0", which FinishLoad would take), then the wait for the hand-over, then beats 1..kAheadTicks one
// after another, each as RunFirstBeat / StartBeatOnThread runs one without orders: the tick, its advance in the record,
// its fingerprint line, its sync and prints kept, its sounds decided (DecideBeatSounds, as on the beats' thread) and its
// camera shakes kept for the load's end. Nothing is given to the picture (the renderer's rules for request 46: no
// GameStarted or Beat before SwapMap; the effects and props' changes are left in the simulation for PrepareSession,
// after these beats, which keeps them for the first Beat after SwapMap; the hits, impacts and health are not dropped
// here as NotePictureBeat does without a picture: how many there were is logged).
void GameSession::RunLoadAhead(Sim* sim) {
  LoadAhead& a = *loadAhead_;
  using Clock = std::chrono::steady_clock;
  auto since = [this] { return std::chrono::duration<double>(Clock::now() - loadBegan_).count(); };
  a.setupSync = sim->TakeSync();
  PostPrints(*sim);  // (the setup's lines, as FinishLoad posts them; the console's list is guarded)
  {
    std::unique_lock<std::mutex> lock(a.mutex);
    if (a.gate == LoadAhead::Gate::Working) a.gate = LoadAhead::Gate::Parked;
    a.parkedAt = since();
    a.cv.notify_all();
    a.cv.wait(lock, [&a] { return a.gate == LoadAhead::Gate::Go || a.gate == LoadAhead::Gate::Stop; });
    if (a.gate == LoadAhead::Gate::Stop) return;
  }
  const auto t0 = Clock::now();
  if (fp_) fp_->Beat(*sim, 0);  // (beat 0: the state Start left, as RunFirstBeat)
  std::vector<SoundAction> beatSounds;
  std::vector<ShakeEvent> shakes;
  while (sim->tick() < kAheadTicks && !a.stop.load(std::memory_order_acquire)) {
    const auto tb = Clock::now();
    sim->Tick();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - tb).count();
    if (ms > a.longestMs) a.longestMs = ms, a.longestTick = sim->tick();
    g_crashTick = sim->tick();
    Record(CommandWriter::AdvanceMessage(1));
    if (fp_) fp_->Beat(*sim, ++fpBeats_);
    for (std::string& sync : sim->TakeSync()) a.syncs.push_back(std::move(sync));
    PostPrints(*sim);
    if (a.decideSounds) {
      DecideBeatSounds(*sim, &beatSounds);
      // (the running sounds as the sound system will have them once these are played, for the next beat's decision:
      // SoundSystem::SetLoop keeps a loop for a bank and cue, and lets it go for none)
      for (const SoundAction& s : beatSounds) {
        if (s.kind != SoundAction::Loop) continue;
        auto it = std::lower_bound(loopEntities_.begin(), loopEntities_.end(), s.entity);
        const bool has = it != loopEntities_.end() && *it == s.entity;
        if (!s.bank.empty() && !s.cue.empty()) {
          if (!has) loopEntities_.insert(it, s.entity);
        } else if (has) {
          loopEntities_.erase(it);
        }
      }
      a.sounds.insert(a.sounds.end(), std::make_move_iterator(beatSounds.begin()), std::make_move_iterator(beatSounds.end()));
    }
    ShakeEvents(*sim, &shakes);
    a.shakes.insert(a.shakes.end(), shakes.begin(), shakes.end());
  }
  a.ranTo = sim->tick();
  a.hits = sim->hitEvents.size(), a.impacts = sim->impactEvents.size(), a.health = sim->healthEvents.size();
  a.beatsSeconds = std::chrono::duration<double>(Clock::now() - t0).count();
  a.doneAt = since();
}

// FinishLoad's part for those beats (the worker waited for): their syncs where the beats after the load would have
// left them (the setup's apart, then each beat's in order, for the game's screen), their sounds played and their camera
// shakes posted now, and the beats' early start learnt afresh (as RunAhead's end does).
void GameSession::TakeLoadAhead() {
  LoadAhead& a = *loadAhead_;
  setupSync_ = std::move(a.setupSync);
  pendingSync_ = std::move(a.syncs);
  aheadRanTo_ = a.ranTo;
  if (sounds) ApplyBeatSounds(a.sounds);
  if (!seekTo_)
    for (const ShakeEvent& e : a.shakes) PostCameraShake(e.position, e.radius, e.maxShakeEpicenter, e.minShakeAtRadius, e.interval);
  leadBeats_ = 0, lead_ = 0;
  Logf(LogLevel::Info,
       "moho64ui: beats 1 to %u ran in the load (no prebuilt units: nothing to see or do yet): the loading thread waited for the "
       "hand-over from %.2f to %.2f s, then %.2f s of beats (the longest, beat %u, %.0f ms), through at %.2f s; %zu syncs, %zu sounds and "
       "%zu camera shakes kept for the game's screen; %zu hits, %zu impacts and %zu health changes in them",
       a.ranTo, a.parkedAt, a.goAt, a.beatsSeconds, a.longestTick, a.longestMs, a.doneAt, pendingSync_.size(), a.sounds.size(),
       a.shakes.size(), a.hits, a.impacts, a.health);
  loadAhead_.reset();
}

double GameSession::LaunchToLoadEnd() const { return std::chrono::duration<double>(loadEndedAt_ - launchAt_).count(); }
double GameSession::SinceLoadEnd() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - loadEndedAt_).count(); }

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
  if (LongFrames().loading) {
    LongFrames().loading = false;
    if (!LongFrames().asides.empty())
      Logf(LogLevel::Info, "moho64ui: made off the frames' thread while it loaded: %s", LongFrames().AsideSummary().c_str());
  }
  const std::unique_ptr<SessionDescription> described = std::move(loadSession_);
  if (!loadOk_) {
    *why = loadWhy_;
    if (keepMenuMap_) keepMap_ = keepMenuMap_ = false;  // (the menu's world stays the menu's: its flight begins again)
    loadSim_.reset();
    play_.reset();
    playData_.clear();
    return false;
  }
  const SessionDescription& session = *described;
  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session started: rules %.2f s, simulation start-up %.2f s", loadRulesSeconds_, loadStartSeconds_);
  {
    // (where the load's time went, seconds from its start: what the loading thread did, and what came after it)
    const double end = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadBegan_).count();
    char map[160] = "no map opening behind";
    if (keepMap_)
      std::snprintf(map, sizeof map, "no map opening behind (%s: the map shown is started over)",
                    keepMenuMap_ ? "the main menu's flight's map" : "a jump back");
    // (aheadInLoad: the loading thread's own work ended at the hand-over; its beats came after, while the map opened)
    const double stateDone = loadAhead_ ? loadAhead_->parkedAt : loadThreadDone_;
    if (loadMapReadyAt_ >= 0)
      std::snprintf(map, sizeof map, "the session given to the map opening behind at %.2f s, that map ready at %.2f s (%.2f s after the loading thread)",
                    loadHandedAt_, loadMapReadyAt_, loadMapReadyAt_ - stateDone);
    char beats[96] = "";
    if (loadAhead_)
      std::snprintf(beats, sizeof beats, "; its beats in the load from %.2f to %.2f s", loadAhead_->goAt, loadAhead_->doneAt);
    Logf(LogLevel::Info,
         "moho64ui: the load's parts: the loading thread through at %.2f s (rules %.2f, the simulation's start %.2f, the game's state %.2f)%s; %s; "
         "finished at %.2f s",
         stateDone, loadRulesSeconds_, loadStartSeconds_, loadStateSeconds_, beats, map, end);
  }
  sim_ = std::move(loadSim_);
  const std::vector<std::string> uids = std::move(loadUids_);
  heightsChanges_ = ~0ull;
  CopyHeights();
  CopyAllies();
  setupSync_ = sim_->TakeSync();
  PostPrints(*sim_);  // (what the scripts printed while the session started: the original's setup sync)
  pendingSync_.clear();
  if (session.focusArmy >= 0 && session.focusArmy < static_cast<int>(h.armies.size())) commandSource_ = h.armies[session.focusArmy].source;
  // (a network game: this client's own source, the one AssignCommandSource gave its owner, CLobby::LaunchGame 0x7c46b8 /
  // 0x7c497d; a player's is its army's, an observer's commands no army (its orders reach every sim and are dropped
  // there, Sim::IssueCommand 0x749290); without this an observer's orders named source 0, the host's, and the
  // lockstep dropped them as not its own)
  if (net_)
    for (const NetClient& c : net_->clients)
      if (c.local && !c.sources.empty()) commandSource_ = c.sources.front();
  modUids_ = uids;
  sessionMods_ = h.mods;
  map_ = h.map;
  armies_ = h.armies.size();
  seed_ = h.seed;
  due_ = 0;
  if (!recordsOpen_) OpenRecords(session, *sim_);  // (aheadInLoad: opened at the hand-over, the beats in them)
  recordsOpen_ = false;  // (the next game's are its own)
  if (loadAhead_) TakeLoadAhead();
  // A replay played back with MOHO64_FINGERPRINT set: its fingerprint there too, line for line as the recorded game's
  // .fp (a measurement with the hashing's cost in it, and a check of the playback against the game; ours)
  fpPlayback_ = false;
  if (play_ && std::getenv("MOHO64_FINGERPRINT")) {
    if (fp_) fp_->End(fpBeats_), fp_.reset();
    fp_ = std::shared_ptr<Fingerprint>(Fingerprint::FromEnvironment().release());
    fpBeats_ = 0;
    fpPlayback_ = fp_ != nullptr;
  }
  pace.rate = pace.asked = session.gameSpeed;
  pace.adjustable = session.adjustableSpeed;
  // This program's maxSP afresh (the original's sim driver and client manager are made anew for each session): in a
  // local game or a replay the local client's 50 until the first measure sent, or a test's value (MOHO64UI_FORCE_MAXSP,
  // as if sent before the first beat).
  measure_ = SpeedMeasure{};
  pace.maxSP = 50;
  pace.replay = play_ != nullptr;
  if (const char* forced = std::getenv("MOHO64UI_FORCE_MAXSP"); forced && *forced && !net_) {
    measure_.forced = true;
    measure_.sent = pace.maxSP = std::atoi(forced);
    Logf(LogLevel::Info, "moho64ui: our maxSP is %d (MOHO64UI_FORCE_MAXSP)", pace.maxSP);
  }
  if (!net_) pace.rate = pace.LocalRate();
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
  if (back_.to) back_.loaded = BackSeconds();  // (a jump back's clock: SeekReached's line)
  loadEndedAt_ = std::chrono::steady_clock::now();
  return true;
}

// The game's record (--replay-out: its stream, seed file, the table's copy and the fingerprint) and its replay file
// (LastGame), opened as the game starts: in FinishLoad, or at the hand-over when the first beats run in the load
// (aheadInLoad: each beat's advance and fingerprint line written by the loading thread as it runs).
void GameSession::OpenRecords(const SessionDescription& session, const Sim& sim) {
  recordsOpen_ = true;
  const ReplayHeader& h = session.header;
  StartReplayFile(session, sim);
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  recordBase_.clear();
  if (!replayOut.empty()) {  // the game's record: see the header
    const int index = RecordIndex();
    const std::string base = Numbered(replayOut, index);
    const std::string table = TableFile();
    // (a restart: no lobby wrote this game's table; it is the game before's)
    if (tableAgain_ && table != lastTable_ && !lastTable_.empty()) {
      std::optional<std::string> text = hostfs::ReadFile(lastTable_);
      if (!text || !hostfs::WriteFile(table, *text))
        Logf(LogLevel::Warning, "moho64ui: cannot copy %s to %s", lastTable_.c_str(), table.c_str());
    }
    lastTable_ = table;
    tableAgain_ = false;
    ++recorded_;
    lastIndex_ = index;
    recordBase_ = base;
    stream_ = std::fopen((base + ".stream").c_str(), "wb");
    if (!stream_) Logf(LogLevel::Warning, "moho64ui: cannot write %s.stream", base.c_str());
    else Logf(LogLevel::Info, "moho64ui: this game is recorded in %s.stream (seed %u)", base.c_str(), h.seed);
    if (std::FILE* f = std::fopen((base + ".seed").c_str(), "wb")) {
      std::fprintf(f, "seed %u\nmap %s\narmies %zu\nfocus army %d\n", h.seed, h.map.c_str(), h.armies.size(), session.focusArmy + 1);
      std::fprintf(f, "build %s\n", kBuildCommit);  // (the commit this program was built from: the engine's playback picks its build by it)
      if (!table.empty()) std::fprintf(f, "session %s\n", table.c_str());
      for (const std::string& folder : replayMaps) std::fprintf(f, "maps %s\n", folder.c_str());
      std::fclose(f);
    }
    // (the engine's class opens its file from MOHO64_FINGERPRINT: set to this game's for that moment, the
    // variable's own value put back after). Only when recordFingerprint: a played game's record is its stream, seed
    // and table, and its fingerprint is rebuilt exactly by playing it back headless (the engine, 2026-11-03: in Chris's
    // big test it cost 21 ms a beat on the beat's thread, 50-60 ms late in the game).
    if (fp_) fp_->End(fpBeats_), fp_.reset();
    if (!recordFingerprint) {
      fpBeats_ = 0;
    } else {
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
  }
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
// [head2] The second head's window on the picture (the renderer's answer to ui request 48): its HWND on Windows
// (UseSecondWindow); elsewhere the SDL window's Vulkan surface (UseSecondHostSurface; UNTESTED, as the SDL path).
// Before Open it is made with the device (the answer then comes with Open: SecondWindowOn).
bool UseSecondOn(render::WorldPicture& picture, void* window, int width, int height) {
#ifdef _WIN32
  return picture.UseSecondWindow(window, width, height);
#else
  auto* w = static_cast<Window*>(window);
  return picture.UseSecondHostSurface([w](void* instance) { return w->CreateSurface(instance); }, width, height);
#endif
}
}  // namespace
#endif

bool GameSession::UseSecondWindow(void* handle, int width, int height) {
#ifdef MOHO64UI_WORLD_PICTURE
  window2_ = handle, window2W_ = width, window2H_ = height;
  if (!windowPicture_) return false;  // (none yet, or it failed: EnsureWindowPicture asks for it with the next one)
  render::WorldPicture& p = windowPicture_->picture;
  windowPicture_->layer2W = windowPicture_->layer2H = 0;
  if (!UseSecondOn(p, handle, width, height) || !p.SecondWindowOn()) {
    Logf(LogLevel::Warning, "moho64ui: the renderer's second window did not open (%s): head 1's interface is shown by the plain copy, its world black",
         p.SecondWindowError().c_str());
    return false;
  }
  return true;
#else
  (void)handle, (void)width, (void)height;
  return false;
#endif
}

void GameSession::CloseSecondWindow() {
#ifdef MOHO64UI_WORLD_PICTURE
  if (windowPicture_) windowPicture_->picture.CloseSecondWindow(), windowPicture_->layer2W = windowPicture_->layer2H = 0;
#endif
  window2_ = nullptr;
}

bool GameSession::SecondWindowOn() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return window2_ && windowPicture_ && windowPicture_->picture.SecondWindowOn();
#else
  return false;
#endif
}

bool GameSession::GiveSecondLayer(const SoftwareTarget& target, const IntRect& changed) {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!SecondWindowOn()) return false;
  WindowPicture& wp = *windowPicture_;
  // (as GiveLayer: the changed part, all of it the first time and after a change of size; a new size also resizes the
  // second window's swap chain, the renderer's answer)
  const int w = target.Width(), h = target.Height();
  IntRect part = changed.Intersect(IntRect{0, 0, w, h});
  if (wp.layer2W != w || wp.layer2H != h) part = IntRect{0, 0, w, h}, wp.layer2W = w, wp.layer2H = h;
  if (part.empty()) part = IntRect{};
  wp.picture.SetSecondInterfaceLayer(target.pixels().data(), w, h, w, part.x0, part.y0, part.x1 - part.x0, part.y1 - part.y0);
  return true;
#else
  (void)target, (void)changed;
  return false;
#endif
}

bool GameSession::CaptureSecondWindow(const std::string& path) {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!SecondWindowOn()) return false;
  capture2To_ = path;
  capture2Rgba_.clear();
  windowPicture_->picture.CaptureNextSecondWindowFrame(&capture2Rgba_);
  return true;
#else
  (void)path;
  return false;
#endif
}

bool GameSession::SecondWindowDrawnLastFrame() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return SecondWindowOn() && windowPicture_->picture.SecondWindowDrawnLastFrame();
#else
  return false;
#endif
}

void GameSession::WaitForNextFrame() {
#ifdef MOHO64UI_WORLD_PICTURE
  // (the window's picture; with Low Latency off it returns at once)
  if (windowPicture_) windowPicture_->picture.WaitForNextFrame();
#endif
}

bool GameSession::FrameGenerationOn() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return windowPicture_ && windowPicture_->picture.FrameGenerationOn();
#else
  return false;
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
  made->picture.SetLowLatency(made->lowLatency = Pacing().lowLatency);  // (the Remastered tab's Low Latency, before Open)
  UseGameWindow(made->picture, window);
  if (window2_) UseSecondOn(made->picture, window2_, window2W_, window2H_);  // ([head2] made with the device)
  std::string error;
  if (!made->picture.Open(vfs_, "", &error)) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture did not open (%s): the interface is shown without the renderer", error.c_str());
    windowPictureFailed_ = true;
    return false;
  }
  if (window2_ && !made->picture.SecondWindowOn())
    Logf(LogLevel::Warning, "moho64ui: the renderer's second window did not open (%s): head 1's interface is shown by the plain copy, its world black",
         made->picture.SecondWindowError().c_str());
  Looks().rayTracingSupported = made->picture.RayTracingSupported() ? 1 : 0;  // (the Remastered tab's Ray Tracing)
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
  wp.menuBehindGame = false;
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
    p.SetSeason(looks.season == 1 ? render::Season::Autumn : render::Season::Summer);  // (the Remastered tab's Season)
    p.HoldLooks(true);
    PictureLooks(p, looks);  // (ray tracing too: the surfaces' shaders made for reflections as the map opens, render request 31)
    const Sky sky = skyNow_;
    before = [looks, sky](render::World& w) {
      WorldLooks(w, looks, true);
      SkyOnWorld(w, sky.edge, sky.startHour, sky.weather);
    };
  } else if (wp.shows == Kind::Game) {
    // (behind a game's world that is over, ServeWindowMap: the menus' lighting bundles held for the map, so that it
    // opens and is warmed with them, the game's world left as it is; the picture's other settings stay the game's while
    // its world shows and are put back as a picture starts at the swap, StartAfresh there. The season and the Texture
    // Detail are only kept for the map's opening.)
    p.HoldLooks(true);
    StartBundles(p);
    if (map == MenuFlight::kMap) FlightLooks(p, &before);
    p.SetSeason(static_cast<render::Season>(MenuSeason(map)));
    p.SetTextureDetail(Looks().textureDetail);
    wp.menuBehindGame = true;
  } else {
    // (the menus' background as it always was: the picture's looks as a picture starts, the summer, the player's
    // Texture Detail as the background's own picture had it: Backdrop::Run)
    StartAfresh(p, true);
    if (map == MenuFlight::kMap) FlightLooks(p, &before);  // (the flight's: held for it)
    p.SetSeason(static_cast<render::Season>(MenuSeason(map)));
    p.SetTextureDetail(Looks().textureDetail);
  }
  const bool begun = p.BeginMap(map, before);
  // (the game's hour and weather for the warm frames: the sky, clouds and light shafts made for them while it opens,
  // not in the game's first frame; SetDayAndWeather follows after the swap as before: DressGamePicture)
  if (begun && game && skyNow_.startHour >= 0)
    p.SetLoadDayAndWeather(skyNow_.startHour, skyNow_.dayLengthSeconds, WeatherByItself(skyNow_.weather));
  if (!begun && game) StartAfresh(p, true), p.HoldLooks(false);  // (the held looks give way: LetGoOfGameMap)
  if (!begun && wp.menuBehindGame) GameLooksBack(wp);
  if (!begun && !game && !wp.menuBehindGame && map == MenuFlight::kMap) StartAfresh(p, true), p.HoldLooks(false);  // (the flight's held looks give way)
  if (!begun) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture cannot open %s behind", map.c_str());
    if (!game) failedMenuMap_ = map;
    return;
  }
  wp.pending = kind, wp.pendingMap = map;
  wp.begun = std::chrono::steady_clock::now();
  wp.pendingFrames = 0;
  ++mapsBegun_;
  {
    // (what it opens with that stays until the next opening: a game's as the player has them; the menus' the player's
    // Texture Detail, the flight's the player's Season (MenuSeason) and its ray tracing, the dusk background's the summer)
    const LookSettings looks = Looks();
    const bool rays = (game || map == MenuFlight::kMap) && looks.rayTracing && p.RayTracingSupported();
    wp.pendingWith = WindowPicture::OpenedWith{looks.textureDetail, game ? (looks.season == 1 ? 1 : 0) : MenuSeason(map), rays};
  }
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
  FollowMenuMode();
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
  if (!picture_ && load_ == Load::None) FollowMenuMode();
  if (!picture_ && load_ == Load::None && menuBackdrop && menuBackdrop->Direct() && menuBackdrop->map() != failedMenuMap_) {
    const std::string& want = menuBackdrop->map();
    const WindowPicture& wp = *windowPicture_;
    if (!(wp.shows == Kind::Menu && wp.shownMap == want) && !(wp.pending == Kind::Menu && wp.pendingMap == want)) BeginWindowMap(false, want);
  } else if (picture_ && load_ == Load::None && menuBackdrop && menuBackdrop->BegunDirect() && menuBackdrop->map() != failedMenuMap_ &&
             windowPicture_ && windowPicture_->shows == Kind::Game && windowPicture_->pending == Kind::None && GameOver()) {
    // The game is over (the victory or defeat, then the score screen: FAF's score.lua covers the window in black): the
    // menus' map is begun behind the game's world now, so that it is ready when the score screen's Continue leaves
    // the game. Begun only after the game had left, it kept the menu over black for its opening (Chris's big test
    // 2026-11-01: 2.9 s after Continue, the window's picture made anew too as his FSR had changed the scene's
    // samples; that remake now waits for the next game's map, behind its loading screen: EnsureWindowPicture keeps
    // the picture while a game's world shows). (The background itself is stopped while a game runs, main.cpp; the next
    // front end's begins it again and finds its map opening.) INFERRED, not the original's way: its front end's
    // background is a film.
    BeginWindowMap(false, menuBackdrop->map());
  }
  if (!windowPicture_) return;  // (made anew and failed)
  WindowPicture& wp = *windowPicture_;
  if (wp.pending == Kind::None) return;
  ++wp.pendingFrames;
  std::string error;
  if (wp.picture.MapFailed(&error)) {
    Logf(LogLevel::Warning, "moho64ui: %s cannot be shown (%s)", wp.pendingMap.c_str(), error.c_str());
    if (menuFlight && wp.pending == Kind::Menu && wp.pendingMap == MenuFlight::kMap) {
      menuFlight->Fail("its map cannot be opened (" + error + ")"), flightGiven_ = false;
      failedMenuMap_.clear();  // (the background falls back to Dusk's map: FollowMenuMode)
      wp.picture.CancelMap();
      wp.pending = Kind::None;
      return;
    }
    (wp.pending == Kind::Menu ? failedMenuMap_ : wp.failedGameMap) = wp.pendingMap;
    if (wp.pending == Kind::Game) LetGoOfGameMap(wp.picture);
    else wp.picture.CancelMap();
    if (wp.pending == Kind::Menu && wp.menuBehindGame) GameLooksBack(wp);
    wp.pending = Kind::None;
    return;
  }
  // (one begun behind a game's world: swapped in once that world is let go of, End; the picture's own settings put
  // back as a picture starts first, as BeginWindowMap does for the menus' map otherwise: the game's frames may have
  // given their own meanwhile, and SwapMap keeps the picture's settings and lays the bundles on as they are)
  // (the flight's map waits for its simulation, handed to it before the swap: ServeMenuFlight)
  const bool flightMap = menuFlight && wp.pendingMap == MenuFlight::kMap && menuBackdrop && menuBackdrop->mode() == Backdrop::Mode::Flight;
  if (wp.pending == Kind::Menu && flightMap && !flightGiven_) return;
  if (wp.pending != Kind::Menu || !wp.picture.MapReady() || (wp.menuBehindGame && wp.shows == Kind::Game)) return;
  if (wp.menuBehindGame) StartAfresh(wp.picture, true);
  if (!wp.picture.SwapMap()) return;
  wp.shows = Kind::Menu, wp.shownMap = wp.pendingMap;
  wp.pending = Kind::None;
  wp.menuBehindGame = false;
  wp.shownWith = wp.pendingWith;
  wp.shownSky = WindowPicture::OpenSky{};
  if (const render::World* w = wp.picture.world()) {  // (its sky as opened, before the evening: KeepMenuMap gives it back)
    WindowPicture::OpenSky& s = wp.shownSky;
    s.taken = true;
    s.timeOfDay = w->timeOfDay, s.cloudCover = w->cloudCover, s.rain = w->rain, s.snow = w->snow, s.fog = w->fog;
    s.wet = w->wet, s.snowLying = w->snowLying, s.climate = w->climate;
    s.cinematicHaze = w->cinematicHaze;
  }
  Backdrop::Dress(wp.picture);
  wp.fading = true, wp.fadeFrom = std::chrono::steady_clock::now();
  flightShows_ = false;
  if (flightMap) StartFlightOnPicture(wp, false);
  const render::WorldPicture::LoadNumbers n = wp.picture.LastLoadNumbers();
  Logf(LogLevel::Info,
       "moho64ui: the menu's background: %s, in the window's picture, ready after %.1f s (open %.1f s, ready %.1f s; %d frames meanwhile; "
       "the frames' share %.0f ms in %ld asks, at most %.1f ms in one; %d warm frames, the longest %.0f ms)",
       wp.shownMap.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - wp.begun).count(), n.openSeconds, n.readySeconds,
       wp.pendingFrames, n.serviceMs, n.asks, n.longestServiceMs, n.warmFrames, n.warmLongestMs);
}

// The menus' map begun behind a game's world not to be had after all (it failed): the game's looks laid on its world
// again as they were before (they were held, StartBundles), the hold let go.
void GameSession::GameLooksBack(WindowPicture& wp) {
  wp.menuBehindGame = false;
  PictureLooks(wp.picture, Looks());
  wp.picture.HoldLooks(false);
}

// The main menu's background's mode (Backdrop::Mode) as the player's option has it (Menu Background, live), unless the
// flight cannot be had this run (no SETONS_R, its simulation or map failed, its frames cost too much): Dusk then, the
// option kept as chosen. Leaving Flight stops its simulation (its world stays shown until the next map is swapped in).
void GameSession::FollowMenuMode() {
  if (!menuBackdrop) return;
  using Mode = Backdrop::Mode;
  const int option = Looks().menuBackground;
  Mode want = option == 1 ? Mode::Dusk : option == 2 ? Mode::Still : Mode::Flight;
  std::string why;
  if (want == Mode::Flight && (!menuFlight || menuFlight->state() == MenuFlight::State::Failed)) {
    want = Mode::Dusk;
    why = menuFlight ? menuFlight->FailedWhy() : "no flight in this program";
  }
  const Mode was = menuBackdrop->mode();
  if (was == want) return;
  Logf(LogLevel::Info, "moho64ui: the menu's background: %s -> %s%s%s%s", Backdrop::ModeName(was), Backdrop::ModeName(want), why.empty() ? "" : " (",
       why.c_str(), why.empty() ? "" : ")");
  if (was == Mode::Flight && menuFlight) {
    menuFlight->Stop();
    flightGiven_ = flightShows_ = flightCovering_ = false;
    if (sounds) sounds->StopWorld();
  }
  menuBackdrop->SetMode(want);
  still_.clear(), stillW_ = stillH_ = 0, stillShown_ = false;
}

// Once a frame (PresentToWindow): the flight's simulation begun while no game's session is there (it waits for one to
// end: one simulation at a time), handed to its map opening behind once it is made (PrepareSession: its models made
// there; the map's warm frames from the path's first view); a battle made anew (its start, or its loop's) started on
// the flight's map when that is the world shown (RestartMap); the loop's cover once its storyboard has ended.
void GameSession::ServeMenuFlight(WindowPicture& wp) {
  using Kind = WindowPicture::Kind;
  if (!menuFlight || !menuBackdrop || menuBackdrop->mode() != Backdrop::Mode::Flight || !menuBackdrop->Direct()) return;
  MenuFlight& f = *menuFlight;
  const bool noGame = !picture_ && !sim_ && load_ == Load::None;
  if (!noGame) return;
  if (!f.onBeat) f.onBeat = [this](const Sim& sim) { windowPicture_->picture.Beat(sim); };  // (on the flight's thread)
  if (f.state() == MenuFlight::State::Idle && f.Described()) {
    f.collectSounds = Looks().menuBattleSound;  // (Menu Battle Sound: from each start of the battle)
    f.sounds = sounds;
    f.Begin();
  }
  f.Poll();  // (its load through: Ready, to be handed over below)
  if (f.state() != MenuFlight::State::Ready) {
    if (f.state() == MenuFlight::State::Ended && flightShows_ && !flightCovering_) {
      flightCovering_ = true;
      coverFrom_ = std::chrono::steady_clock::now();
      Logf(LogLevel::Info, "moho64ui: the menu's flight: its end, covered (tick %u)", f.tick());
    }
    return;
  }
  // (the battle made: to the map opening behind, or started over on the flight's map shown)
  if (wp.pending == Kind::Menu && wp.pendingMap == MenuFlight::kMap && !flightGiven_) {
    render::Camera start;
    if (f.StartView(&start)) wp.picture.SetLoadCamera(start);
    wp.picture.SetLoadDayAndWeather(Backdrop::kDuskHour, 0, false);  // (its warm frames at the hour it shows: StartFlightOnPicture)
    if (wp.picture.PrepareSession(*f.sim())) {
      flightGiven_ = true;
      Logf(LogLevel::Info, "moho64ui: the menu's flight: its simulation handed to its map opening behind (models made ahead)");
    }
  } else if (wp.pending == Kind::None && wp.shows == Kind::Menu && wp.shownMap == MenuFlight::kMap && !flightShows_) {
    StartFlightOnPicture(wp, true);
  }
}

// The flight's air: the least haze its cinematic camera sees (World::cinematicHaze), at 0.4 of the renderer's (Chris on
// the flight's dusk, 2026-11-09: "a bit more clearer if anything"; measured at 1080p, tick 500, the far land's darkest
// twentieth 0.210 at all of it, 0.195 at 0.65, 0.164 at none: 0.65 could hardly be seen). Always from the amount the
// world was opened with (`opened`, kept at the menu's swap), so a flight started again does not thin it twice. The
// player's own Haze & Mist amount (LookSettings::hazeMist) is on top of it: the renderer multiplies a cinematic
// camera's least haze by World::hazeMist, so the flight is the player's amount times this share, in a game the amount.
// MOHO64UI_FLIGHT_HAZE: another share, for the window checks' pictures side by side.
static void FlightHaze(render::World* w, bool taken, float opened) {
  if (!w || !taken) return;
  float share = 0.4f;
  if (const char* v = std::getenv("MOHO64UI_FLIGHT_HAZE"); v && *v) share = std::max(0.0f, std::min(2.0f, static_cast<float>(std::atof(v))));
  w->cinematicHaze = opened * share;
}

// The flight's battle on the world shown: just swapped in with its simulation handed over (`restart` false), or the
// map shown started over for a battle made anew (its loop, the option back to Flight). Then as a game's picture is
// dressed (DressGamePicture: GameStarted, Beat), every army shown (no player's view), nothing of the interface in the
// world (a cinematic camera's: CineCamera's header), the menu's evening as Dusk has it (Backdrop::Dress); a cut.
void GameSession::StartFlightOnPicture(WindowPicture& wp, bool restart) {
  MenuFlight& f = *menuFlight;
  render::WorldPicture& p = wp.picture;
  Sim* sim = f.sim();
  if (!sim) return;
  if (restart && !p.RestartMap()) {
    f.Fail("the map shown could not be started over (RestartMap)");
    return;
  }
  flightGiven_ = false;
  FlightFrameLooks(p);  // (its menu look, per frame only: the map stays as a game would open it)
  p.SetFocusArmy(0);
  p.SetLifeBars(false);
  p.SetSelectionMarks(false);
  p.SetWorkBars(false);
  p.GameStarted(*sim);
  if (render::World* w = p.world()) SkyOnWorld(*w, "beyond", -1, "");  // (the map's edge as a game's default)
  Backdrop::Dress(p);
  FlightHaze(p.world(), wp.shownSky.taken, wp.shownSky.cinematicHaze);
  // (the hour as the picture's day (a session's world takes its light from it, not from Dress's timeOfDay: without it
  // the flight showed the map's own noon light, 2026-11-09's window check); Dusk's evening, standing still, fair)
  p.SetDayAndWeather(Backdrop::kDuskHour, 0, false);
  p.Beat(*sim);
  p.CameraCut();
  flightGround_ = p.CineGround();
  flightShows_ = true;
  if (flightCovering_) {  // (the loop's cover goes: from black, as the map came in first)
    flightCovering_ = false;
    wp.fading = true, wp.fadeFrom = std::chrono::steady_clock::now();
  }
  if (flightShownAt_ == std::chrono::steady_clock::time_point{}) flightShownAt_ = std::chrono::steady_clock::now();
  Logf(LogLevel::Info, "moho64ui: the menu's flight: the battle on the world shown (%s, loop %d)", restart ? "the map started over" : "swapped in",
       f.loops());
}

// The flight's first frame of this run kept (KeptFrame): the world alone (TakePhoto: nothing of the interface; the
// menu's veil is the interface's and lies over it as over the live world) from the camera and at the moment that frame
// shows, at the window's size, a few settling frames first (the clouds, the light in the air), written beside the
// preferences on the kept frame's thread. Once a run, under the black or the kept frame the flight comes in from (the
// photo's time, some frames', is not seen there).
void GameSession::TakeKeptFrame(WindowPicture& wp, const render::Camera& camera, double tick, int width, int height) {
  KeptFrame& k = *kept_;
  k.taken = true;
  std::vector<uint32_t> pixels;
  int mw = 0, mh = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = wp.picture.TakePhoto(camera, width, height, tick, &pixels, &mw, &mh, 1, 8);
  k.takeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!ok || mw != width || mh != height || pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
    Logf(LogLevel::Warning, "moho64ui: the menu's flight: its first frame could not be kept (%dx%d asked, %dx%d made)", width, height, mw, mh);
    return;
  }
  Logf(LogLevel::Info, "moho64ui: the menu's flight: its first frame kept for the next start (%dx%d, %.2f s, %s)", width, height, k.takeSeconds,
       k.File(width, height).c_str());
  k.fresh = pixels, k.freshW = width, k.freshH = height, k.freshKey = KeptKey();
  k.Write(std::move(pixels), width, height, k.freshKey);
}

// (FNV-1a over the camera path's text and the looks the flight's frames take from the player: another path, or a look
// changed, and the frame kept would not be the flight's first)
// The season a menu map opens in: the flight's map the player's own Season (2026-11-09, the lead: so an Autumn player's
// menu shows autumn and a skirmish on SETONS_R still takes the flight's map over, the season being fixed at the map's
// opening, renderer R2); the dusk background's and any other menu map the summer, as before.
int GameSession::MenuSeason(const std::string& map) const {
  return static_cast<int>(map == MenuFlight::kMap && Looks().season == 1 ? render::Season::Autumn : render::Season::Summer);
}

uint32_t GameSession::KeptKey() const {
  uint32_t h = 2166136261u;
  auto add = [&h](const void* data, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(data)[i]) * 16777619u;
  };
  if (menuFlight) add(menuFlight->pathText().data(), menuFlight->pathText().size());
  const LookSettings looks = Looks();
  const int parts[] = {looks.atmosphere, looks.softShadows, looks.groundDetail, looks.originalLook, looks.textureDetail, looks.richGround,
                       looks.fidelity, looks.shadows, looks.bloom, looks.photoLook, static_cast<int>(looks.upscale * 100), looks.SceneSamples(),
                       looks.season};
  add(parts, sizeof parts);
  return h;
}

void GameSession::PushWindowInfo(lua_State* L) const {
  auto num = [L](const char* k, double v) {
    lua_pushstring(L, k);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  auto boolean = [L](const char* k, bool v) {
    lua_pushstring(L, k);
    lua_pushboolean(L, v);
    lua_rawset(L, -3);
  };
  auto str = [L](const char* k, const std::string& v) {
    lua_pushstring(L, k);
    lua_pushstring(L, v.c_str());
    lua_rawset(L, -3);
  };
  num("handoversKept", handoversKept_);
  num("handoversReopened", handoversReopened_);
  str("handoverWhy", handoverWhy_);
  num("mapsBegun", mapsBegun_);
  boolean("flightShows", flightShows_);
  boolean("keptShown", keptShown_);
  num("keptShownFrames", kept_ ? kept_->shownFrames : 0);
  boolean("keptTaken", kept_ && kept_->taken);
  num("keptTakeSeconds", kept_ ? kept_->takeSeconds : -1);
#ifdef MOHO64UI_WORLD_PICTURE
  if (!windowPicture_) return;
  const WindowPicture& wp = *windowPicture_;
  static const char* const kKinds[] = {"none", "menu", "game", "left"};
  str("worldShows", kKinds[static_cast<int>(wp.shows)]);
  str("worldMap", wp.shownMap);
  str("worldPending", kKinds[static_cast<int>(wp.pending)]);
  if (const render::World* w = windowPicture_->picture.world()) {
    boolean("worldBounceLight", w->bounceLight);
    boolean("worldRayReflections", w->rayReflections);
    num("worldCinematicHaze", w->cinematicHaze);
    num("worldOpenedCinematicHaze", wp.shownSky.taken ? wp.shownSky.cinematicHaze : -1);
    boolean("worldVolumetricClouds", w->volumetricClouds);
    boolean("worldSoftShadows", w->softShadows);
    boolean("worldWaterDetail", w->waterDetail);
    boolean("worldGroundCover", w->groundCover);
    num("worldTimeOfDay", w->timeOfDay);
    num("worldCloudCover", w->cloudCover);
  }
  boolean("rayTracingSupported", windowPicture_->picture.RayTracingSupported());
#endif
}

double GameSession::MenuFramePeriod() const {
  return menuFlight && (flightShows_ || keptShown_) && !picture_ && !sim_ && load_ == Load::None ? 1.0 / 60.0 : 0.0;
}

// The picture over the window the card draws this frame (WindowPicture::Over): the interface's when its drawing gave
// one (CardSink), else the menus' map coming in from black, else none.
void GameSession::GiveBackdrop(WindowPicture& wp, int width, int height) {
  using Over = WindowPicture::Over;
  render::WorldPicture& p = wp.picture;
  float alpha = 1;
  Over over = Over::None;
  // (the flight's kept first frame: read for the window's size while the flight is wanted and not live yet; shown where
  // the movie shows until the flight's world is there, docs/ui-menuflight.md 4.5)
  bool keptWanted = false;
  if (menuFlight && menuBackdrop && menuBackdrop->mode() == Backdrop::Mode::Flight && menuBackdrop->Direct() && !picture_ && !sim_ &&
      load_ == Load::None && width > 0 && height > 0) {
    if (!kept_) {
      kept_ = std::make_unique<KeptFrame>(), kept_->dir = userDir.empty() ? std::string(".") : userDir;
      if (auto stock = vfs_->ReadFile(kStockFirstFrame)) kept_->stock = std::move(*stock);  // (the program's own: KeptFrame)
    }
    KeptFrame& k = *kept_;
    // (this run's frame taken: the one shown from now, once the one read is no longer over the window)
    if (!k.fresh.empty() && wp.over != Over::Kept && !(wp.over == Over::Fade && wp.fadeKept))
      k.pixels.swap(k.fresh), k.fresh.clear(), k.w = k.pw = k.freshW, k.h = k.ph = k.freshH, k.key = k.freshKey, k.isStock = false;
    if (!flightShows_ && menuFlight->Described()) {
      k.Read(width, height, KeptKey());
      k.Through();
      keptWanted = !flightCovering_ && menuBackdrop->ShownThisFrame() && !k.busy.load() && !k.reading && !k.pixels.empty() &&
                   k.w == width && k.h == height && k.key == KeptKey();
    }
  }
  if (card_ && card_->wanted && !card_->pixels.empty()) {
    over = Over::Card, alpha = card_->wantedAlpha;
  } else if (flightCovering_) {  // (the flight's loop: black over a second, held until it starts over)
    over = Over::Cover;
    alpha = static_cast<float>(std::min(1.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - coverFrom_).count()));
  } else if (wp.fading) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - wp.fadeFrom).count() / 1.5;  // (as Backdrop::Show's)
    if (t >= 1) wp.fading = false;
    else over = Over::Fade, alpha = static_cast<float>(1.0 - t);
  } else if (stillShown_ && !still_.empty()) {  // (Still: its one picture, where the movie shows)
    over = Over::Still;
  } else if (keptWanted) {
    over = Over::Kept;
  }
  if (over == Over::Card && (card_->fresh || wp.over != Over::Card)) {
    p.SetBackdrop(card_->pixels.data(), card_->width, card_->height, card_->width, render::WorldPicture::BackdropFit::Stretch, true);
    card_->fresh = false;
  } else if ((over == Over::Fade || over == Over::Cover) && wp.over != Over::Fade && wp.over != Over::Cover) {
    // (the flight swapped in while its kept frame showed: it comes in over that frame, not over black)
    wp.fadeKept = over == Over::Fade && wp.over == Over::Kept;
    if (wp.fadeKept) Logf(LogLevel::Info, "moho64ui: the menu's flight comes in over its kept first frame");
    else p.SetBackdrop(&wp.black, 1, 1, 1, render::WorldPicture::BackdropFit::Stretch, true);
  } else if (over == Over::Still && wp.over != Over::Still) {
    p.SetBackdrop(still_.data(), stillW_, stillH_, stillW_, render::WorldPicture::BackdropFit::Stretch, true);
  } else if (over == Over::Kept && wp.over != Over::Kept) {
    p.SetBackdrop(kept_->pixels.data(), kept_->pw, kept_->ph, kept_->pw,
                  kept_->isStock ? render::WorldPicture::BackdropFit::Cover : render::WorldPicture::BackdropFit::Stretch, true);
  } else if (over == Over::None && wp.over != Over::None) {
    p.SetBackdrop(nullptr, 0, 0, 0, render::WorldPicture::BackdropFit::Stretch, true);
  }
  if (over == Over::Kept) ++kept_->shownFrames;
  if (over != Over::Fade) wp.fadeKept = false;
  keptShown_ = over == Over::Kept;
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
  ServeMenuFlight(wp);
  const int w = target.Width(), h = target.Height();
  // (The renderer unions the changed parts until its next frame.)
  GiveLayer(wp.picture, target, changed, &wp.layerW, &wp.layerH);
  const bool vsync = Pacing().verticalSync;
  if (vsync != wp.vsync) wp.picture.SetVsync(vsync), wp.vsync = vsync;
  const bool lowLatency = Pacing().lowLatency;
  if (lowLatency != wp.lowLatency) wp.picture.SetLowLatency(lowLatency), wp.lowLatency = lowLatency;
  if (!rewinding_) GiveBackdrop(wp, w, h);
  // (a frame read back: CaptureWindow's, or the picture a jump back holds, FreezePicture; into a member, as the
  // renderer fills it in the next DrawToWindow, whichever frame that is)
  const bool capturing = !captureTo_.empty() || freezeAsked_;
  if (capturing) captureRgba_.clear(), wp.picture.CaptureNextWindowFrame(&captureRgba_);
  bool drawn = false;
  const auto t0 = std::chrono::steady_clock::now();
  FrameTimes& times = Times();
  if (rewinding_) {
    // A jump back held (Rewind): the picture read back at the click, over the whole window and dimmed, the host's note
    // over it (the interface's layer, nothing else in it); no world drawn (the game's that is over, or the new one
    // as the replay runs ahead behind it). (The renderer still prepares its world each frame, render request R2.)
    using Over = WindowPicture::Over;
    if (wp.over != Over::Frozen) {
      if (!frozen_.empty()) wp.picture.SetBackdrop(frozen_.data(), frozenW_, frozenH_, frozenW_, render::WorldPicture::BackdropFit::Stretch, true);
      else wp.picture.SetBackdrop(&wp.black, 1, 1, 1, render::WorldPicture::BackdropFit::Stretch, true);
      wp.picture.SetBackdropLook(1, 0x8c8c8c);  // (dimmed to some 55%)
      wp.over = Over::Frozen, wp.overAlpha = 1;
    }
    if (wp.picture.ViewCount() > 1) wp.picture.SetViews({});
    wp.picture.SetWorldShown(false);
    wp.heldWorld = true;
    wp.picture.DrawToWindow(render::Camera{}, w, h, 0);
    wp.drawnW = w, wp.drawnH = h;
    drawn = true;
  } else if (picture_ && picture_->direct && !holdBeats_) {  // a game: its world from the world view's camera
    Picture& pic = *picture_;
    const double moment = pic.momentFixed ? pic.moment : PictureTick();  // (the one the anchors were of)
    pic.momentFixed = false;
    if (wp.heldWorld) wp.picture.SetWorldShown(Looks().worldShown), wp.heldWorld = false;  // (the game's ren_Oblivion)
    if (pic.cameraW > 0) {
      wp.picture.DrawToWindow(pic.camera, w, h, moment);
      wp.drawnW = w, wp.drawnH = h;
      drawn = true;
      // (the first-start graphics check's measurement: the card's time of the newest frame it has finished)
      if (FrameCost().fakeMs < 0) FrameCost().Note(wp.picture.LastStats().gpuMs);
    }
    // (The picture shows moment - 1, between the beats round it: past the newest beat it stands still.)
    if (sim_) {
      const uint32_t newest = BeatRunning() ? beatFrom_ : sim_->tick();
      if (moment - 1.0 > static_cast<double>(newest) + 1e-6) {
        ++times.heldPictures;
        // (MOHO64UI_HELD_LOG=1: each held frame with where the beats stood, to find what holds them; 2026-10-28)
        static const bool heldLog = std::getenv("MOHO64UI_HELD_LOG") != nullptr;
        if (heldLog) {
          const auto now = std::chrono::steady_clock::now();
          auto ago = [&](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(now - t).count(); };
          Logf(LogLevel::Info,
               "moho64ui: held: moment %.3f (%s) newest %u, beat %s from %u (sim tick %u), due %.3f, lead %.3f, the beat began %.1f ms "
               "ago, the last finished here %.1f ms ago",
               moment, pic.momentFixed ? "fixed" : "now", newest, BeatRunning() ? "running" : "idle", beatFrom_, sim_->tick(), due_, lead_,
               ago(beatBegunAt_), ago(beatFinishedAt_));
        }
      }
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
    const bool movie = wp.shows == Kind::Menu && menuBackdrop && menuBackdrop->TakeView(wp.picture, w, h, &camera, &tick);
    bool world = movie;
    // The main menu's flight (ServeMenuFlight): its battle's clock and beats run while the movie shows, and the camera
    // is its path's at the battle's moment; under the loop's cover, once black, the battle starts over (the world not
    // drawn until it is back).
    if (menuFlight && flightShows_ && wp.shows == Kind::Menu && wp.shownMap == MenuFlight::kMap) {
      const auto now = std::chrono::steady_clock::now();
      const double dt = flightFrameAt_ == std::chrono::steady_clock::time_point{}
                            ? 0.0
                            : std::min(0.25, std::chrono::duration<double>(now - flightFrameAt_).count());
      flightFrameAt_ = now;
      MenuFlight& f = *menuFlight;
      const bool black = flightCovering_ && std::chrono::duration<double>(now - coverFrom_).count() >= 1.05;
      if (black && f.state() == MenuFlight::State::Ended) {
        if (sounds) sounds->StopWorld();
        f.collectSounds = Looks().menuBattleSound;
        f.Restart();  // (its simulation made anew on its thread: ServeMenuFlight starts it on the map shown when ready)
        flightShows_ = false;
      }
      if (flightShows_) {
        f.Advance(dt, movie && !flightCovering_);
        render::Camera cine;
        if (movie && flightGround_ && f.View(flightGround_, &cine)) {
          menuBackdrop->FitToMovie(&cine, h);
          camera = cine;
          tick = f.Moment();
          if (f.TakeCut()) wp.picture.CameraCut();
          // (its first frames of this run: kept for the next start, docs/ui-menuflight.md 4.5. At the battle's third beat,
          // a third of a second in: the storyboard's bases and fronts are made at its first, and the live flight comes in
          // over the kept frame from its beat 0, the units there from its first beat; the camera has moved a few metres)
          if (kept_ && !kept_->taken && f.loops() == 0 && f.tick() >= 3 && f.tick() <= 20 && !f.fast && f.fastUntil == 0)
            TakeKeptFrame(wp, camera, tick, w, h);
          if (f.collectSounds && sounds) {  // (Distant: heard from high over the camera, so all of it from afar)
            render::Vec3 right, up, back;
            camera.Axes(&right, &up, &back);
            const render::Vec3 eye = camera.Eye();
            const float e[3] = {eye.x, eye.y + 400.0f, eye.z}, focus[3] = {camera.target.x, camera.target.y, camera.target.z};
            const float r[3] = {right.x, 0, right.z};
            sounds->SetListener(e, focus, r, 2000.0f);
          }
        }
      }
      if (flightCovering_ && !flightShows_) world = false;
      if (world && flightShows_) {  // (every 10 s: what it costs)
        if (flightLogFrom_ == std::chrono::steady_clock::time_point{}) flightLogFrom_ = now;
        flightLogGpu_ += wp.picture.LastStats().gpuMs, ++flightLogFrames_;
        flightLogFrameMax_ = std::max(flightLogFrameMax_, dt * 1000.0);
        flightLogBeatMax_ = std::max(flightLogBeatMax_, f.lastBeatMs());
        const double span = std::chrono::duration<double>(now - flightLogFrom_).count();
        if (span >= 10) {
          Logf(LogLevel::Info,
               "moho64ui: the menu's flight: tick %u, %d frames in %.1f s (%.1f a second, the longest %.1f ms), the card %.2f ms a frame, "
               "beats up to %.1f ms",
               f.tick(), flightLogFrames_, span, flightLogFrames_ / span, flightLogFrameMax_, flightLogGpu_ / flightLogFrames_, flightLogBeatMax_);
          flightLogFrom_ = now, flightLogGpu_ = 0, flightLogFrames_ = 0, flightLogFrameMax_ = 0, flightLogBeatMax_ = 0;
        }
      }
      // (the card's time of the flight's frames, from 10 s to 15 s after it first showed: over the threshold, Dusk for
      // the run)
      if (world && !flightCostChecked_ && flightShownAt_ != std::chrono::steady_clock::time_point{}) {
        const double since = std::chrono::duration<double>(now - flightShownAt_).count();
        if (since >= 10 && since < 15) flightGpuMs_ += wp.picture.LastStats().gpuMs, ++flightGpuFrames_;
        if (since >= 15 && flightGpuFrames_ > 0) {
          flightCostChecked_ = true;
          static const double limit = std::getenv("MOHO64UI_MENUFLIGHT_GPU_MS") ? std::atof(std::getenv("MOHO64UI_MENUFLIGHT_GPU_MS")) : 12.0;
          const double ms = flightGpuMs_ / flightGpuFrames_;
          Logf(LogLevel::Info, "moho64ui: the menu's flight: the card's time %.2f ms a frame over %d frames (10 to 15 s after it showed; the limit %.1f)",
               ms, flightGpuFrames_, limit);
          if (ms > limit) {
            char why[96];
            std::snprintf(why, sizeof why, "its frames cost the card %.1f ms, over %.1f", ms, limit);
            f.Fail(why);
            flightShows_ = false;
          }
        }
      }
    }
    // Still (Backdrop::Mode::Still): one picture of the world from where Dusk starts, taken once (a size), then shown
    // over the window where the movie shows, the world no longer drawn (GiveBackdrop, from the next frame).
    if (menuBackdrop && menuBackdrop->mode() == Backdrop::Mode::Still && wp.shows == Kind::Menu && wp.shownMap == Backdrop::kDuskMap) {
      if (movie && (stillW_ != w || stillH_ != h)) {
        int mw = 0, mh = 0;
        std::vector<uint32_t> pixels;
        const auto t0 = std::chrono::steady_clock::now();
        if (wp.picture.TakePhoto(camera, w, h, tick, &pixels, &mw, &mh, 1, 8) && mw > 0 && mh > 0) {
          still_.swap(pixels), stillW_ = mw, stillH_ = mh;
          wp.over = WindowPicture::Over::None;  // (given anew: GiveBackdrop)
          Logf(LogLevel::Info, "moho64ui: the menu's background: the still picture taken, %dx%d, %.2f s", mw, mh,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } else {
          stillW_ = w, stillH_ = h;  // (not again at this size)
        }
      }
      stillShown_ = movie && !still_.empty();
      if (stillShown_) world = false;
    } else {
      stillShown_ = false;
    }
    if (wp.shows != Kind::None) wp.picture.SetWorldShown(world);
    wp.heldWorld = false;
    wp.picture.DrawToWindow(camera, w, h, tick);
    wp.drawnW = w, wp.drawnH = h;
    drawn = true;
    if (world) {
      times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      ++times.worldDraws;
    }
  }
  if (menuBackdrop) menuBackdrop->EndFrame();
  if (freezeAsked_ && drawn) TakeFrozen(captureRgba_, w, h);
  else if (freezeAsked_ && ++freezeWaits_ >= 3) TakeFrozen({}, w, h);  // (no frame of the world drawn: held black)
  if (!captureTo_.empty()) SaveWindowCapture(captureRgba_, w, h, captureTo_), captureTo_.clear();
  // ([head2] the second window's picture, once the renderer has read it back: the frame it was next drawn in)
  if (!capture2To_.empty() && wp.layer2W > 0 &&
      capture2Rgba_.size() == static_cast<size_t>(wp.layer2W) * static_cast<size_t>(wp.layer2H) * 4)
    SaveWindowCapture(capture2Rgba_, wp.layer2W, wp.layer2H, capture2To_), capture2To_.clear();
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
       skyNow_.edge == "frame" ? "the original's frame of machinery"
       : skyNow_.edge == "surround" ? "no machinery, the map's own sculpted land beyond (where it has one)"
                                    : "no machinery, the land beyond");
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
    if (keepMap_) {
      // A jump back on the map shown (KeepMapForRewind): the world put back as the map was opened, between frames, no
      // beat running (FinishLoad waited for the loading thread; the old session's beats ended with it), then the new
      // session given to the picture as after a swap. If the renderer cannot (no world, a photo being made), the map is
      // opened again below, as for a game loaded before the window, and swapped in once ready (OpenPictureIfDue).
      // A game launched from the main menu on its flight's map (KeepMenuMap) likewise: the flight's world taken over,
      // the game's looks laid back on it first (the flight's menu look, its evening and its picture's settings go).
      keepMap_ = false;
      const bool fromMenu = keepMenuMap_;
      keepMenuMap_ = false;
      const Kind kept = fromMenu ? Kind::Menu : Kind::Left;
      if (wp.pending == Kind::None && wp.shows == kept && wp.shownMap == map_ && wp.picture.RestartMap()) {
        wp.shows = Kind::Game;
        wp.fading = false;
        auto pic = std::make_unique<Picture>(&wp.picture);
        pic->session = this;
        pic->direct = true;
        StartAfresh(wp.picture, false);  // (the per-game settings, as at a swap)
        if (fromMenu) {
          // (the game's looks as BeginWindowMap gives a game's map: the picture's own bundles and settings, and the
          // world's own switches, which Picture::SetScene gives again each frame; the sky as the map opened it, for
          // the game's own (DressGamePicture's SkyOnWorld, SetDayAndWeather); a cut from the flight's last camera)
          const LookSettings looks = Looks();
          PictureLooks(wp.picture, looks);
          if (render::World* w = wp.picture.world()) {
            WorldLooks(*w, looks, true);
            const WindowPicture::OpenSky& s = wp.shownSky;
            if (s.taken) {
              w->timeOfDay = s.timeOfDay, w->cloudCover = s.cloudCover, w->rain = s.rain, w->snow = s.snow, w->fog = s.fog;
              w->wet = s.wet, w->snowLying = s.snowLying, w->climate = s.climate;
              w->cinematicHaze = s.cinematicHaze;  // (a replay's cinematic camera: the renderer's own again)
            }
          }
          wp.picture.CameraCut();
        }
        wp.picture.SetWorldShown(true);
        DressGamePicture(*pic);
        picture_ = std::move(pic);
        Logf(LogLevel::Info, "moho64ui: session: the world's picture is shown, %.3f s (the map shown started over: RestartMap%s)",
             secs(t3, std::chrono::steady_clock::now()), fromMenu ? ", the main menu's flight's world taken over" : "");
        return;
      }
      Logf(LogLevel::Warning, "moho64ui: session: the map shown could not be started over (RestartMap): it is opened again");
    }
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
    wp.shownWith = wp.pendingWith;
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
  pic->picture.SetLowLatency(Pacing().lowLatency);  // (the Remastered tab's Low Latency)
  pic->picture.SetSeason(Looks().season == 1 ? render::Season::Autumn : render::Season::Summer);  // (the Remastered tab's Season)
  // (Ray Tracing before Open: the surfaces' shaders are made for reflections when the map opens; the
  // renderer's request 31 follow-up. The first frame's looks block sets it again, harmlessly.)
  pic->picture.SetRayTracing(Looks().rayTracing);
  std::string error;
  if (pic->picture.Open(vfs_, map_, &error)) {
    Looks().rayTracingSupported = pic->picture.RayTracingSupported() ? 1 : 0;  // (the Remastered tab's Ray Tracing)
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

bool GameSession::LoadInterfaceRules(lua_State* userState, Sim* sim, const std::vector<std::string>& uids, const std::string& sessionMods) {
  auto t0 = std::chrono::steady_clock::now();
  // The session's one rules state, as the original's one RRuleGameRules per session exported into both states
  // (func_WorldSessionUserLoad 0x885e53 / 0x885fe6, Sim::Sim 0x743c05): the simulation's own load, kept for this copy
  // (Sim::keepRules, set in BeginLoad; engine 7e001be, ui REQUESTS 46), its blueprints the sim's own, the engine's
  // fixes included. Only a simulation that did not keep it has the rules loaded here once more.
  std::unique_ptr<ScriptState> rules = sim ? sim->TakeRules() : nullptr;
  const bool shared = rules != nullptr;
  if (!shared) {
    rules = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
    rules->SetHookDirs(hookDirs_);
    // The engine's script fixes for blueprints, as the simulation's own rules have them (a fix for one
    // of the game's mods: the campaign mod's weapons merged by label, fix 4), so that the interface's
    // copy matches the simulation's (ui REQUESTS 35). (The session has started: its fixes are final.)
    if (sim && sim->fixes && sim->RulesFixesWanted()) Sim::JoinFixesHooks(*rules);
    SimBlueprints::StartRecording(*rules);
    lua_State* R = rules->L();
    if (!sessionMods.empty() && PushSerializedLua(R, sessionMods) && lua_istable(R, -1)) lua_setglobal(R, "__active_mods");
    else SetActiveMods(*rules, uids);
    lua_settop(R, 0);
    if (!rules->DoScript("/lua/ruleinit.lua")) return false;
  }
  lua_State* R = rules->L();
  const auto t1 = std::chrono::steady_clock::now();
  interfaceBlueprints_ = std::make_unique<SimBlueprints>();
  interfaceBlueprints_->fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  interfaceBlueprints_->CopyToSim(R, userState);
  RegisterCategoryBindings(userState, interfaceBlueprints_.get());
  lua_pushlightuserdata(userState, this);
  lua_pushcclosure(userState, l_BlueprintFactsForTests, 1);
  lua_setglobal(userState, "moho64ui_BlueprintFacts");
  NoteRealGlobals(userState);
  rules.reset();  // (the copy needs nothing of it afterwards)
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
  GiveBeatSync();
}

void GameSession::WaitForWorker() {
  // (aheadInLoad: a wait for the loading thread serves its hand-over first, or it would wait for this one for ever)
  if (loadAhead_ && load_ == Load::Running) {
    LoadAhead& a = *loadAhead_;
    bool parked = false;
    {
      std::unique_lock<std::mutex> lock(a.mutex);
      a.cv.wait(lock, [&a] { return a.gate != LoadAhead::Gate::Working || a.jobDone; });
      parked = a.gate == LoadAhead::Gate::Parked;
    }
    if (parked) ServeLoad();
  }
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

// The beats due now (due_), one after another; true when one was begun on its thread (the caller then stops).
bool GameSession::RunDueBeats(bool catchingUp) {
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
      BeatOrdersTaken();  // (the beat's time for maxSP from here: its orders, then its tick)
      const bool ending = replayEndAsked_ && !sim_->endGame;  // (the viewer's End Replay: one last beat, see above)
      if (ending) sim_->endGame = true;
      replayEndAsked_ = false;
      if (ending || (!play_->AtEnd() && play_->ApplyUntilAdvance(*sim_))) {
        if (sim_->Paused() && !ending) {
          sim_->PausedBeat();
          FingerprintBeat();
        } else if (onThread) {
          StartBeatOnThread();
          return true;
        } else {
          TimedTick();
          NoteBeatTime();
          FingerprintBeat();
          MaxSimRate();
        }
      } else {
        continue;  // the replay is over: the game stands where it ended
      }
      NoteBeat();
      DeliverBeatSync();
      continue;
    }
    // The beat's orders: the player's own; in a network game every client's, merged (the engine's Lockstep: the
    // player's go out to the others for a later beat), and a beat whose orders are not all in waits: the frame goes
    // on without it (the picture keeps drawing, the interface runs).
    std::string orders;
    if (net_) {
      if (!NetEveryoneReady()) {  // (the time waited does not pile up, as a replay's viewer's hold)
        due_ = 0;
        break;
      }
      if (takeOrders) net_->lockstep->Queue(takeOrders());
      if (!NetBeatOrders(&orders)) {
        due_ += 0.1;
        if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // (a run without a window: no spinning)
        break;
      }
    } else if (takeOrders) {
      orders = takeOrders();
    }
    BeatOrdersTaken();  // (the beat's time for maxSP from here, in every game: MaxSimRate)
    Record(orders);
    if (!orders.empty() && !ApplyCommands(*sim_, orders)) Logf(LogLevel::Warning, "moho64ui: an order was not understood by the simulation");
    // (a network game's beat 1 runs here, not before the game's screen: its fingerprint's beat 0 as RunFirstBeat's)
    if (net_ && sim_->tick() == 0 && Fingerprinting()) fp_->Beat(*sim_, 0);
    if (sim_->Paused()) {
      // (recorded as every beat: the original's replays hold their paused beats (the engine: AIR 40, LAND 72), and
      // a paused beat changes the simulation's state, so a playback must run it too)
      sim_->PausedBeat();
      NetDigest();
      Record(CommandWriter::AdvanceMessage(1));
      FingerprintBeat();
      NetAfterBeat();
      MaxSimRate();  // (a paused beat has no time: nothing goes in)
    } else if (onThread) {
      Record(CommandWriter::AdvanceMessage(1));
      StartBeatOnThread();
      return true;
    } else {
      TimedTick();
      NoteBeatTime();
      NetDigest();
      Record(CommandWriter::AdvanceMessage(1));
      FingerprintBeat();
      NetAfterBeat();
      MaxSimRate();
    }
    NoteBeat();
    DeliverBeatSync();
  }
  return false;
}

// A network game's next beat's orders: true with every client's, merged (and the beat's AfterBeat then due); false:
// wait (a client's are not in). A wait of more than two seconds is logged once, with the clients missing.
bool GameSession::NetBeatOrders(std::string* merged) {
  Net& n = *net_;
  const uint32_t beat = n.lockstep->nextBeat();
  static const bool beatLog = std::getenv("MOHO64UI_NET_BEAT_LOG") != nullptr;
  if (beatLog && n.askedFor != beat) n.askedFor = beat, n.askedAt = std::chrono::steady_clock::now(), n.inSeen = false;
  if (!n.lockstep->Ready(merged)) {
    const auto now = std::chrono::steady_clock::now();
    if (beatLog && !n.inSeen && n.exchange->OrdersIn(beat)) n.inSeen = true, n.inAt = now;
    if (!n.waiting) n.waiting = true, n.waitingSince = now, n.waitTold = false;
    if (!n.waitTold && now - n.waitingSince > std::chrono::seconds(2)) {
      n.waitTold = true;
      std::string missing, lost;
      for (const NetClient& c : n.clients) {
        if (c.local) continue;
        if (n.exchange->ReceivedBeats(c.client) < beat) missing += " " + std::to_string(c.client) + " (" + c.name + ")";
      }
      for (uint8_t c : n.exchange->LostClients()) lost += " " + std::to_string(c);
      Logf(LogLevel::Info, "moho64ui: the network game waits for beat %u: missing from client(s)%s; lost:%s", beat,
           missing.empty() ? " none" : missing.c_str(), lost.empty() ? " none" : lost.c_str());
    }
    return false;
  }
  if (beatLog) {
    const auto now = std::chrono::steady_clock::now();
    auto ms = [](std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    Logf(LogLevel::Info, "moho64ui: net beat %u: %.1f ms since the last began; waited %.1f ms, %.1f of it with every client's orders in",
         beat, n.lastBegun.time_since_epoch().count() ? ms(now - n.lastBegun) : 0.0, ms(now - n.askedAt),
         n.inSeen ? ms(now - n.inAt) : 0.0);
    n.lastBegun = now;
  }
  if (n.waiting && n.waitTold) Logf(LogLevel::Info, "moho64ui: the network game goes on with beat %u", beat);
  n.waiting = false;
  n.beat = beat;
  n.digest.clear();
  n.afterDue = true;
  return true;
}

// After a network game's beat, on the thread that ran it (the engine's: on the beats' thread right after the beat):
// its digest for the checksum, every 50th beat only (not free). Beside it, ours for a desync (NetSendParts): the
// beat's pulse (a few numbers read, every beat) and, every 50th beat, the state line the digest is made of (handed out
// by LockstepDigest: ui request 51).
void GameSession::NetDigest() {
  if (!net_ || !net_->afterDue) return;
  Net& n = *net_;
  const uint32_t beat = n.beat;
  // the test switches that change this copy's state, right after the beat's tick: a true desync from this beat on
  if (beat == n.drawAt) {
    sim_->NextUInt32();
    Logf(LogLevel::Info, "moho64ui: beat %u: one random number more drawn on purpose (moho64ui_NetTestForTests)", beat);
  }
  if (beat == n.moveAt) {
    Unit* pick = nullptr;
    for (Unit* u : sim_->units())
      if (u && !u->dead && u->army && u->army->index == n.moveArmy && (!pick || u->id < pick->id)) pick = u;
    if (pick) {
      pick->position.x += 0.25f;
      Logf(LogLevel::Info, "moho64ui: beat %u: unit %#x of army %d moved on purpose (moho64ui_NetTestForTests)", beat,
           pick->id, n.moveArmy.load());
    }
  }
  Net::Pulse& p = n.pulses[beat % n.pulses.size()];
  p.beat = beat;
  p.draws = sim_->rngDraws_;
  p.units = static_cast<uint32_t>(sim_->units().size());
  p.projectiles = static_cast<uint32_t>(sim_->projectiles.size());
  if (beat % kLockstepChecksumPeriod != 0) return;
  std::string line;
  n.digest = LockstepDigest(*sim_, beat, &line);
  if (!n.detailPath.empty() && beat == n.detailAt) {  // (asked for at the game's first desync: NetAskDetail)
    Fingerprint::WriteDetail(*sim_, beat, n.detailPath + ".detail." + std::to_string(beat),
                             "the next checksum beat after the game's first desync");
    if (FILE* f = std::fopen(n.detailPath.c_str(), "w")) {
      std::fprintf(f, "%s\n", line.c_str());
      std::fclose(f);
    }
    n.detailPath.clear();
  }
  if (const size_t lua = line.rfind(" lua="); lua != std::string::npos) line.resize(lua);  // (as LockstepDigest hashes it)
  if (beat == n.corruptAt && !n.digest.empty()) {  // (the test switch: a desync on purpose)
    n.digest[0] = static_cast<char>(n.digest[0] ^ 0x5a);
    // (and, when named, that part of our line, as if the state's part differed: the state itself is left as it is)
    const size_t at = n.corruptPart.empty() ? std::string::npos : line.find(" " + n.corruptPart + "=");
    if (at != std::string::npos) {
      const size_t end = line.find(' ', at + 1);
      char& c = line[(end == std::string::npos ? line.size() : end) - 1];
      c = c == '0' ? '1' : '0';
    }
    Logf(LogLevel::Info, "moho64ui: beat %u: our checksum is changed on purpose (moho64ui_NetTestForTests)%s%s", beat,
         at != std::string::npos ? ", and our line's part " : "", at != std::string::npos ? n.corruptPart.c_str() : "");
  }
  n.lines.emplace_back(beat, std::move(line));
  // (kept 300 beats: a peer's parts come a round trip after it found the desync, itself up to its input delay and its
  // network behind)
  n.lines.erase(std::remove_if(n.lines.begin(), n.lines.end(), [&](const auto& l) { return l.first + 300 < beat; }),
                n.lines.end());
}

// Then, here: the lockstep hears of it (sends our checksum, compares the others'); a difference goes to the desync
// dialog: as the original's CWldSession::DoBeat 0x894530 after a beat whose sync names desyncs, one
// uimain.ShowDesyncDialog(beat, strings) (0x895434) with the beat just run (CWldSession+0x458, 0x895429) and, for
// each difference, the name of the client whose checksum differs (0x895364: its entry in the session's client list;
// GPGNET_ReportDesync 0x7b9ac0 beside it is not ours). Then ours: our parts of that beat to that client, and the
// parts other clients sent compared with ours.
void GameSession::NetAfterBeat() {
  if (!net_ || !net_->afterDue) return;
  net_->afterDue = false;
  net_->lockstep->AfterBeat(net_->digest);
  std::vector<LockstepDesync>& desyncs = net_->lockstep->desyncs();
  std::vector<std::string> names;
  for (const LockstepDesync& d : desyncs) {
    Logf(LogLevel::Warning, "moho64ui: the network game is out of step at beat %u: client %u's checksum differs", d.beat, d.client);
    std::string name;
    for (const NetClient& c : net_->clients)
      if (c.client == d.client) name = c.name;
    names.push_back(name);
    NetSendParts(d.client, d.beat);
  }
  if (!names.empty()) net_->events.desyncs.emplace_back(net_->beat, std::move(names));
  desyncs.clear();
  for (const auto& [client, bytes] : net_->exchange->TakeDesyncParts()) NetCompareParts(client, bytes);
}

namespace {
// QuantizeSimRateSample 0x55ae10: 10 log10(x) (x87: ln x / ln 10.0 (the double at 0xe4f790), times 10.0f, stored as a
// float), rounded down (frndint, then one less when the float is below it, 0x55ae45..0x55ae53). (Ours in double
// precision where the original's is in the x87's extended one: the two differ only for a float a hair from a whole
// number.)
int QuantizeSimRateSample(float x) {
  const float v = static_cast<float>(std::log(static_cast<double>(x)) / std::log(10.0) * 10.0);
  return static_cast<int>(std::floor(v));
}
}  // namespace

// This program's maxSP, after each beat, in every game: the original's sim driver after each dispatch (GPG's
// 0x73da03..0x73dab5, the same in FAF's 0x128ff9f..0x1290051): unless the sim is paused (+0x68, set from the beat's
// sync by FinalizeSyncDispatchLocked 0x73dcae; INFERRED that the sync's two fields it reads, +0x250 != -1 or +0x270,
// mean the pause), the beat's time in ms (a Timer from the dispatch's start, 0x128fdf9, to after the sync:
// CyclesToMilliseconds 0x9554e0) goes into the last 25's ring (when full, the oldest out: 24 kept, 0x128ffbd..
// 0x1290008); their median (0x48bf00: the kept ones sorted, the one at count / 2) makes x = 1000 / median x 0.1
// (single precision, 0x1290010..0x129001c: the beats a second this PC could run, over the 10 of normal speed), and
// QuantizeSimRateSample(x) is sent when it differs from the value sent last (0x129003a..0x1290051: BroadcastIntParam,
// the client manager's slot 16, 0x53e850: message 57 to every client, this one's own among them).
// So a client whose beats take 100 ms sends 0, 10 ms 10, 1 ms 20, and the game runs at the smaller of the speed asked
// for and every connected client's value (GetSimRate 0x53e720). A measure of this PC's speed: the copies' values
// differ, never their simulations (the speed is not in the sim's state).
// In every game alike (READ): the sim driver's Dispatch (0x73c250 -> 0x73c393: 0x73d8c0, FAF's jump to 0x128fde0)
// asks nothing of the clients before it, and the path above only the pause. A single-player session's client manager
// has one client, the local one (WLD_SetupSessionInfo 0x88cbc0 -> 0x88bbb0: CLIENT_CreateClientManager(1, ...) at
// 0x88bcb9, CreateLocalClient at 0x88bceb), a replay's two, its CReplayClient (client 0, 0x876c45) and the local one
// (0x876c9e); BroadcastIntParam gives message 57 to each client's Process: the local client takes it as its own
// (CLocalClient::Process 0x53d190 -> CClientBase::Process: its +0xd4), a CReplayClient drops it (0x53d99c: types
// above 53), so its +0xd4 stays the 50 of CClientBase's ctor (0x53be07). So: a network game sends our value to the
// others (NetBeatExchange::RunRate holds the speed run down by every client's); a local game's or a replay's speed
// run is the smaller of the speed asked for and ours (GamePace::LocalRate; a replay's 50 too).
// Ours times the beat from its orders taken (BeatOrdersTaken, in RunDueBeats: a network game's when NetBeatOrders has
// them, a local game's from the player, a replay's before its stream's) to the end of its tick (NoteBeatTime, on
// whichever thread ran it); INFERRED: the original's span is the same work (its client manager's UpdateStates hands
// the beat's messages to the sim, the Advance among them, CDecoder::DecodeAdvance 0x6e4400, runs it) and also holds
// the sync's making, which ours does after it (TakeBeatSync).
// Ours, not the original's: a local game or a replay measures only while its time follows the PC's clock
// (paceByClock: a window, or MOHO64UI_REAL_TIME), not in a headless run's stepped frames (each waits for its beats,
// so the PC always keeps up, and the run stays the same on a busy machine); and not the beats behind the loading
// screen (the first, RunFirstBeat, and RunAhead's: M28's start among them, moved there so that the game starts
// without its stall). A test's switch puts a value of its own in place of the measure (a network game's
// moho64ui_NetTestForTests("maxsp"), a local game's or a replay's MOHO64UI_FORCE_MAXSP).
void GameSession::NoteBeatTime() {
  measure_.beatMs = sim_->Paused() ? -1.0  // (a paused beat has none: the sim driver skips it, 0x128ff9f)
                                   : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                measure_.ordersTakenAt).count();
}

void GameSession::MaxSimRate() {
  SpeedMeasure& m = measure_;
  const double ms = m.beatMs;
  m.beatMs = -1;
  if (ms < 0 || m.forced) return;
  if (!net_ && (!paceByClock || aheadTo_)) return;
  constexpr int kKept = 25;  // (RollingFloat<25>: SpeedMeasure::beatTimes)
  if ((m.next + 1) % kKept == m.first) m.first = (m.first + 1) % kKept;
  m.beatTimes[m.next] = static_cast<float>(ms);
  m.next = (m.next + 1) % kKept;
  std::array<float, kKept> sorted{};
  size_t count = 0;
  for (int i = m.first; i != m.next; i = (i + 1) % kKept) sorted[count++] = m.beatTimes[i];
  std::sort(sorted.begin(), sorted.begin() + count);
  const float median = sorted[count / 2];
  if (!(median > 0)) return;  // (a beat of no time: none of ours; the original's would send INT_MIN)
  const int value = QuantizeSimRateSample(1000.0f / median * 0.1f);
  if (value == m.sent) return;
  m.sent = value;
  if (net_) {
    net_->exchange->SendMaxSimRate(value);
  } else {  // (the local client's +0xd4, and the speed run with it at once: GetSimRate asks it each time)
    pace.maxSP = value;
    pace.rate = pace.LocalRate();
  }
  static const bool netBeatLog = std::getenv("MOHO64UI_NET_BEAT_LOG") != nullptr;
  static const bool beatLog = std::getenv("MOHO64UI_BEAT_LOG") != nullptr;
  if (net_ ? netBeatLog : (beatLog || netBeatLog))
    Logf(LogLevel::Info, "moho64ui: beat %u: our maxSP %s: %d (the median beat %.2f ms of the last %zu)",
         net_ ? net_->beat : sim_->tick(), net_ ? "sent" : "now", value, static_cast<double>(median), count);
}

namespace {

// A state line (sim/fingerprint.h: "<beat> <tick> <total> <part>=<hash> ...", the lua part cut) taken apart.
struct StateParts {
  std::string beat, tick, total;
  std::vector<std::pair<std::string, std::string>> parts;  // in the line's order
};

StateParts SplitStateLine(const std::string& line) {
  StateParts out;
  size_t p = 0;
  for (int field = 0; p < line.size(); ++field) {
    size_t end = line.find(' ', p);
    if (end == std::string::npos) end = line.size();
    const std::string w = line.substr(p, end - p);
    p = end + 1;
    if (w.empty()) continue;
    if (field == 0) out.beat = w;
    else if (field == 1) out.tick = w;
    else if (field == 2) out.total = w;
    else if (const size_t eq = w.find('='); eq != std::string::npos) out.parts.emplace_back(w.substr(0, eq), w.substr(eq + 1));
  }
  return out;
}

// A part of the state line in a player's words, and whether it is plural ("differ" or "differs"): the parts as
// sim/fingerprint.h lists them; an army's by its player's name when it has one.
std::pair<std::string, bool> PartInWords(const std::string& part, const Sim& sim) {
  if (part == "rng") return {"the random numbers", true};
  if (part == "sim") return {"the game's own state (orders, path searches, pause)", false};
  if (part == "proj") return {"the projectiles", true};
  if (part == "props") return {"the props (wrecks, trees, rocks)", true};
  if (part == "ents") return {"the other entities (shields, beams, blips)", true};
  if (part == "u0") return {"the units of no army", true};
  if (part.size() > 1 && (part[0] == 'a' || part[0] == 'u') && std::isdigit(static_cast<unsigned char>(part[1]))) {
    const int index = std::atoi(part.c_str() + 1);
    std::string who;
    for (const auto& a : sim.armies())
      if (a->index == index) who = a->nickname;
    const bool units = part[0] == 'u';
    const std::string what = units ? "units" : "economy";
    if (who.empty()) return {"army " + std::to_string(index) + "'s " + what, units};
    return {who + "'s " + what + " (army " + std::to_string(index) + ")", units};
  }
  return {"the part " + part, false};
}

std::string NowText() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[40];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  return b;
}

}  // namespace

// Where a desync's files go: beside the game's record (--replay-out) or, without one, its replay
// (replays/<profile>/LastGame.SCFAReplay: LastGame.desync.txt beside it, overwritten by the next game's desync as
// the replay is by the next game). Empty: none (the log alone).
std::string GameSession::NetDesyncBase() const {
  if (!recordBase_.empty()) return recordBase_;
  if (currentReplay.empty()) return std::string();
  const size_t dot = currentReplay.find_last_of('.');
  const size_t slash = currentReplay.find_last_of("/\\");
  return dot != std::string::npos && (slash == std::string::npos || dot > slash) ? currentReplay.substr(0, dot) : currentReplay;
}

// At the game's first desync: this copy's full listing of a beat both sides can still write, the next checksum beat
// (the beat found out of step has run here already, and its state is gone; both copies find a desync within a few
// beats of its checksum beat, so both list the same beat). The listing is the engine's (Fingerprint::WriteDetail: one
// line per army, unit, projectile, prop and entity), written on the beats' thread at that beat (NetDigest):
// <base>.desync.fp (that beat's line) and <base>.desync.fp.detail.<beat>.
void GameSession::NetAskDetail() {
  Net& n = *net_;
  if (n.detailAsked) return;
  n.detailAsked = true;
  const std::string base = NetDesyncBase();
  if (base.empty()) {
    Logf(LogLevel::Info, "moho64ui: no record or replay of this game: the desync's listing is not written");
    return;
  }
  n.detailAt = (n.beat / kLockstepChecksumPeriod + 1) * kLockstepChecksumPeriod;
  n.detailPath = base + ".desync.fp";
  Logf(LogLevel::Info, "moho64ui: the state of beat %u (the next checksum beat) will be listed in %s.detail.%u",
       n.detailAt, n.detailPath.c_str(), n.detailAt);
}

// A desync with `client` at checksum beat `beat` (the lockstep found its checksum differs from ours): our parts of that
// beat to it, once (ours, message 60: net/beat_exchange_net.h): the beat, then each beat's pulse from the checksum beat
// before it on (as far as kept: the first beat whose pulse differs narrows where the difference began), then our state
// line of that beat.
void GameSession::NetSendParts(uint8_t client, uint32_t beat) {
  Net& n = *net_;
  n.firstDesync.emplace(client, beat);
  NetAskDetail();
  if (!n.partsSent.insert({client, beat}).second) return;
  auto mine = std::find_if(n.lines.begin(), n.lines.end(), [&](const auto& l) { return l.first == beat; });
  if (mine == n.lines.end()) {
    Logf(LogLevel::Warning, "moho64ui: no state line of ours is kept for beat %u: its parts are not sent", beat);
    return;
  }
  uint32_t from = beat > kLockstepChecksumPeriod ? beat - kLockstepChecksumPeriod + 1 : 1;
  while (from <= beat && n.pulses[from % n.pulses.size()].beat != from) ++from;  // (the oldest kept)
  net::MessageWriter w;
  w.U32(beat).U32(from).U16(static_cast<uint16_t>(beat + 1 - from));
  for (uint32_t b = from; b <= beat; ++b) {
    const Net::Pulse& p = n.pulses[b % n.pulses.size()];
    w.U32(static_cast<uint32_t>(p.draws)).U32(static_cast<uint32_t>(p.draws >> 32)).U32(p.units).U32(p.projectiles);
  }
  w.Str(mine->second);
  n.exchange->SendDesyncParts(client, w.Done(0).payload);
  Logf(LogLevel::Info, "moho64ui: our state's parts of beat %u sent to client %u", beat, client);
}

// A client's parts of a beat (NetSendParts on its side) compared with ours: the parts that differ, in a player's words,
// and the first beat it shows in as closely as the pulses tell: after the checksum beat before (the same then, or the
// desync would have been found there), and no later than the first beat whose pulse differs. The game's first desync
// with that client goes to the console (DeliverNetEvents); every one to the log and to <base>.desync.txt.
void GameSession::NetCompareParts(uint8_t client, const std::string& bytes) {
  Net& n = *net_;
  net::MessageReader r(bytes);
  const uint32_t beat = r.U32(), from = r.U32();
  const uint16_t count = r.U16();
  std::vector<Net::Pulse> theirs;
  for (uint16_t i = 0; i < count && r.Ok(); ++i) {
    Net::Pulse p;
    p.beat = from + i;
    const uint32_t lo = r.U32(), hi = r.U32();
    p.draws = lo | (static_cast<uint64_t>(hi) << 32);
    p.units = r.U32();
    p.projectiles = r.U32();
    theirs.push_back(p);
  }
  const std::string theirLine = r.Str();
  if (!r.Ok()) {
    Logf(LogLevel::Warning, "moho64ui: client %u's desync parts could not be read (%zu bytes)", client, bytes.size());
    return;
  }
  auto mine = std::find_if(n.lines.begin(), n.lines.end(), [&](const auto& l) { return l.first == beat; });
  if (mine == n.lines.end()) {
    Logf(LogLevel::Warning, "moho64ui: client %u's parts of beat %u came, but ours of that beat are no longer kept", client, beat);
    return;
  }
  std::string name;
  for (const NetClient& c : n.clients)
    if (c.client == client) name = c.name;
  n.firstDesync.emplace(client, beat);
  NetAskDetail();
  const bool first = n.firstDesync[client] == beat;
  // which parts differ (a part only one side has differs too)
  const StateParts a = SplitStateLine(mine->second), b = SplitStateLine(theirLine);
  std::vector<std::pair<std::string, bool>> differ;
  if (a.tick != b.tick) differ.push_back({"the game's tick", false});
  std::vector<std::string> parts;
  for (const auto& kv : a.parts) parts.push_back(kv.first);
  for (const auto& kv : b.parts)
    if (std::find(parts.begin(), parts.end(), kv.first) == parts.end()) parts.push_back(kv.first);
  auto valueOf = [](const StateParts& s, const std::string& part) {
    for (const auto& kv : s.parts)
      if (kv.first == part) return kv.second;
    return std::string();
  };
  for (const std::string& part : parts)
    if (valueOf(a, part) != valueOf(b, part)) differ.push_back(PartInWords(part, *sim_));
  std::string what;
  const size_t shown = differ.size() > 6 ? 5 : differ.size();
  for (size_t i = 0; i < shown; ++i)
    what += (i == 0 ? "" : i + 1 == shown && shown == differ.size() ? " and " : ", ") + differ[i].first;
  if (shown < differ.size()) what += " and " + std::to_string(differ.size() - shown) + " more parts";
  if (differ.empty()) what = "every part of the state is the same; only the checksums differ";
  else what += differ.size() == 1 && !differ[0].second ? " differs" : " differ";
  // where it began: lo .. hi
  const uint32_t lo = beat > kLockstepChecksumPeriod ? beat - kLockstepChecksumPeriod + 1 : 1;
  uint32_t hi = beat, pulseAt = 0;
  for (const Net::Pulse& t : theirs) {
    const Net::Pulse& o = n.pulses[t.beat % n.pulses.size()];
    if (t.beat < lo || t.beat > beat || o.beat != t.beat) continue;
    if (o.draws != t.draws || o.units != t.units || o.projectiles != t.projectiles) {
      hi = pulseAt = t.beat;
      break;
    }
  }
  std::string when;
  if (!first) when = "out of step since beat " + std::to_string(n.firstDesync[client]);
  else if (lo == hi) when = "first at beat " + std::to_string(lo);
  else when = "first between beats " + std::to_string(lo) + " and " + std::to_string(hi);
  const std::string text = "Desync at beat " + std::to_string(beat) + " with " + name + ": " + what + " (" + when + ")";
  Logf(LogLevel::Warning, "moho64ui: %s\n  ours   %s\n  theirs %s", text.c_str(), mine->second.c_str(), theirLine.c_str());
  if (first) n.events.notes.push_back(text);
  // the same in the desync's file, with both lines and the pulses, for taking it apart afterwards
  const std::string base = NetDesyncBase();
  if (base.empty()) return;
  const std::string file = base + ".desync.txt";
  std::FILE* f = std::fopen(file.c_str(), n.desyncFileStarted ? "ab" : "wb");
  if (!f) {
    Logf(LogLevel::Warning, "moho64ui: cannot write %s", file.c_str());
    return;
  }
  if (!n.desyncFileStarted) {
    n.desyncFileStarted = true;
    std::string self;
    for (const NetClient& c : n.clients)
      if (c.local) self = c.name;
    std::fprintf(f,
                 "# FA Remastered: a network game's desyncs as this copy saw them (docs/ui-net-game.md, \"A desync's parts\")\n"
                 "# written %s by client %u (%s); seed %u, map %s\n"
                 "# the lines: <beat> <tick> <total> <part>=<hash> ... (sim/fingerprint.h); pulses: <beat> <random draws> "
                 "<units> <projectiles>\n",
                 NowText().c_str(), n.local, self.c_str(), seed_, map_.c_str());
  }
  std::fprintf(f, "\n%s\n  ours   %s\n  theirs %s\n", text.c_str(), mine->second.c_str(), theirLine.c_str());
  if (pulseAt) {
    const Net::Pulse& o = n.pulses[pulseAt % n.pulses.size()];
    const Net::Pulse& t = theirs[pulseAt - from];
    std::fprintf(f, "  the first pulse that differs: ours %u %llu %u %u, theirs %u %llu %u %u\n", o.beat,
                 static_cast<unsigned long long>(o.draws), o.units, o.projectiles, t.beat,
                 static_cast<unsigned long long>(t.draws), t.units, t.projectiles);
  } else if (!theirs.empty()) {
    std::fprintf(f, "  the pulses of beats %u..%u are the same\n", std::max(lo, from), beat);
  }
  if (n.detailAt)
    std::fprintf(f, "  this copy's full listing of beat %u: %s.desync.fp.detail.%u\n", n.detailAt, base.c_str(), n.detailAt);
  std::fclose(f);
}

// The game's screen is up (main.cpp, after CreateGameInterface; the original sends READY at the end of DoInitializing,
// 0x88c664 -> CClientManagerImpl slot 11, 0x53e4b0): READY to every client and the lockstep's start (beats 1 .. the
// input delay, empty: the original closes them at once after its first sync).
void GameSession::NetReady() {
  if (!net_ || net_->readySent) return;
  net_->readySent = true;
  net_->lockstep->Start();
  net_->exchange->SendReady();
  Logf(LogLevel::Info, "moho64ui: this program's game screen is up: READY sent to the other players");
}

// Before each beat of a network game: true once every client is ready (READY, or ejected: IsEveryoneReady 0x53e560).
// Until then no beat runs and the provider's StartWaitingDialog shows (DoPostInitializing 0x88c6d0 asks it when not
// everyone is ready at once, DoWaiting 0x88c750 its StopWaitingDialog once they are; the disconnect dialog is updated
// meanwhile, user_state.cpp).
bool GameSession::NetEveryoneReady() {
  Net& n = *net_;
  if (n.everyoneReady) return true;
  if (!n.readySent) return false;
  if (n.exchange->EveryoneReady()) {
    n.everyoneReady = true;
    if (n.waitingShown) n.events.waiting = -1;
    Logf(LogLevel::Info, "moho64ui: every player is ready: the network game's beats run");
    return true;
  }
  if (!n.waitingShown) {
    n.waitingShown = true;
    n.events.waiting = 1;
    Logf(LogLevel::Info, "moho64ui: waiting for the other players to be ready");
  }
  return false;
}

std::vector<GameSession::NetClient> GameSession::NetClients() const {
  std::vector<NetClient> out;
  if (!net_) return out;
  out = net_->clients;
  for (NetClient& c : out) {
    const net::ClientLink l = net_->exchange->Link(c.client);
    c.connected = l.connected, c.ping = l.ping, c.quiet = l.quiet, c.maxSP = l.maxSP;
    c.ejectedBy.assign(l.ejectedBy.begin(), l.ejectedBy.end());
  }
  return out;
}

NetGameLink* GameSession::netLink() const { return net_ ? net_->link.get() : nullptr; }

void GameSession::NetEject(int client) {
  if (!net_) return;
  for (const NetClient& c : net_->clients)
    if (c.client == client && !c.local) {
      Logf(LogLevel::Info, "moho64ui: this player votes to eject client %d (%s)", client, c.name.c_str());
      net_->exchange->RequestEject(static_cast<uint8_t>(client));
    }
}

void GameSession::NetChat(const std::vector<int>& clients, const std::string& message) {
  if (!net_) return;
  for (int c : clients) net_->exchange->SendChat(static_cast<uint8_t>(c), message);
}

void GameSession::NetSpeed(int rate) {
  if (!net_) return;
  net_->exchange->RequestRate(rate);
  pace.asked = net_->exchange->Rate();  // (ours is taken here at once, as the original's own broadcast is: GetGameSpeed)
  pace.rate = net_->exchange->RunRate();
}

NetEvents GameSession::NetTakeEvents() {
  NetEvents out;
  if (!net_) return out;
  auto nameOf = [&](uint8_t client) {
    for (const NetClient& c : net_->clients)
      if (c.client == client) return c.name;
    return std::string();
  };
  out = std::move(net_->events);
  net_->events = NetEvents{};
  for (auto& [client, message] : net_->exchange->TakeChats()) out.chats.emplace_back(nameOf(client), std::move(message));
  for (const auto& [client, rate] : net_->exchange->TakeRateNotes()) out.speeds.emplace_back(client, rate);
  for (uint8_t client : net_->exchange->TakeDisconnects()) {
    Logf(LogLevel::Info, "moho64ui: client %u (%s) disconnected: ejected", client, nameOf(client).c_str());
    out.disconnects.push_back(nameOf(client));
  }
  return out;
}

void GameSession::NetTestSwitch(const std::string& what, uint32_t beat, const std::string& arg) {
  if (!net_) return;
  if (what == "freeze") {
    Logf(LogLevel::Info, "moho64ui: this program's network game is frozen (moho64ui_NetTestForTests)");
    net_->exchange->FreezeForTests();
  } else if (what == "corrupt") {
    net_->corruptPart = arg;  // (before the beat: NetDigest reads it on the beats' thread once corruptAt is that beat)
    net_->corruptAt = beat;
  } else if (what == "move") {
    net_->moveArmy = std::atoi(arg.c_str());
    net_->moveAt = beat;
  } else if (what == "draw") {
    net_->drawAt = beat;
  } else if (what == "maxsp") {  // (message 57: this value from now on in place of the measure; "": measured again)
    measure_.forced = !arg.empty();
    if (measure_.forced) {
      measure_.sent = std::atoi(arg.c_str());
      net_->exchange->SendMaxSimRate(measure_.sent);
    }
    Logf(LogLevel::Info, "moho64ui: our maxSP %s%s (moho64ui_NetTestForTests)", measure_.forced ? "is now " : "is measured again",
         measure_.forced ? arg.c_str() : "");
  } else if (what == "split") {  // (our next beat with orders to that client alone (0-based), then this program freezes)
    net_->exchange->SplitForTests(static_cast<uint8_t>(std::atoi(arg.c_str())));
    Logf(LogLevel::Info, "moho64ui: our next beat with orders goes to client %s alone (moho64ui_NetTestForTests)", arg.c_str());
  }
}

void GameSession::RunAhead(uint32_t toTick) {
  aheadTo_ = toTick;
  Logf(LogLevel::Info, "moho64ui: beats %u to %u run behind the loading screen (no prebuilt units: nothing to see or do yet)",
       sim_ ? sim_->tick() + 1 : 1, toTick);
}

bool GameSession::RunningAhead() const { return aheadTo_ != 0; }

void GameSession::Advance(double seconds) {
  if (!sim_) return;
  if (!postgame_ && !play_ && GameOver()) GameMoments(false);  // (the game's moments looked for from its end on, for the score screen)
  if (net_) {  // (a network game's speed: the one its clients agreed on, held down by every client's maxSP)
    pace.asked = net_->exchange->Rate();
    pace.rate = net_->exchange->RunRate();
  }
  if (aheadTo_) {  // (RunAhead: a beat as soon as the last is through, whatever the time; no time counts meanwhile)
    if (BeatRunning()) return;
    if (beatToFinish_) FinishBeat();
    if (sim_->tick() >= aheadTo_) {
      Logf(LogLevel::Info, "moho64ui: the beats behind the loading screen are through (tick %u)", sim_->tick());
      aheadRanTo_ = sim_->tick();
      aheadTo_ = 0, due_ = 0;
      // (and the beats' early start learnt afresh: those beats' times, M28's start among them, would have it start
      // each beat at the most for seconds, the interface waiting on the beats meanwhile; measured on the 4K live game)
      leadBeats_ = 0, lead_ = 0;
      GiveBeatSync();  // (the last of them with the others, for the game's screen)
      return;
    }
    due_ = 0.1;  // (exactly one beat below: on the thread it returns once begun)
    lead_ = 0;
    RunDueBeats(false);
    GiveBeatSync();
    return;
  }
  if (holdBeats_) return;
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
  if (catchingUp && back_.to && back_.aheadFrom < 0) back_.aheadFrom = BackSeconds(), back_.aheadTick = sim_->tick();
  if (catchingUp && rewinding_ && window) {
    // A jump back held in a window (Rewind): nothing is drawn meanwhile, so each frame runs as many beats as fit in
    // kHeldChunkMs (in steps of up to 10, each beat's sync given to the interface's scripts as it comes), the window
    // still answering the system and the note's progress moving on between them. (Without a window, 50 a frame as
    // below: the tests' frames stay as they were.)
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeldChunkMs);
    for (;;) {
      due_ = std::min<uint32_t>(std::max(fastForwardTo, seekTo_) - sim_->tick(), 10) * 0.1 + 0.01;
      RunDueBeats(true);
      GiveBeatSync();
      if (!CatchingUp() || (play_ && play_->AtEnd()) || std::chrono::steady_clock::now() >= until) break;
    }
    SeekReached();
    return;
  }
  if (catchingUp) {  // (--fast-forward, for measurements, or a jump of the replay timeline: up to 50 beats a frame until that beat)
    due_ = std::min<uint32_t>(std::max(fastForwardTo, seekTo_) - sim_->tick(), 50) * 0.1 + 0.01;
  } else {
    AdvanceClock(seconds);  // (after a long stall, catch up by at most a second of beats)
  }
  const bool begun = RunDueBeats(catchingUp);
  GiveBeatSync();  // (the last beat's sync, now that the next may be running: TakeBeatSync)
  if (begun) return;
  SeekReached();
}

void GameSession::DeliverBeatSync() {
  TakeBeatSync();
  GiveBeatSync();
}

// (the simulation's part: before the next beat may start; after any syncs not given yet, in order)
void GameSession::TakeBeatSync() {
  const auto t0 = std::chrono::steady_clock::now();
  for (std::string& sync : sim_->TakeSync()) syncsToGive_.push_back(std::move(sync));
  PostPrints(*sim_);
  LongFrames().Note("(beat: the simulation's sync taken)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// (the interface's scripts' part: the next beat may run meanwhile)
void GameSession::GiveBeatSync() {
  if (syncsToGive_.empty()) return;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> syncs;
  syncs.swap(syncsToGive_);
  for (std::string& sync : syncs) {
    if (onSync) onSync(sync);
    else pendingSync_.push_back(std::move(sync));
  }
  LongFrames().Note("(beat: the sync given to the interface's scripts)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// The tick and the picture's copy of it on the beats' thread (the renderer's viewer does the same:
// render/live_sim.cpp); nothing here touches the simulation until FinishBeat.
void GameSession::StartBeatOnThread() {
  beatFrom_ = sim_->tick();
  if (!worker_) worker_ = std::make_unique<BeatWorker>();
  beatToFinish_ = true;
  beatBegunAt_ = std::chrono::steady_clock::now();
  // (what the beats' thread needs of this one's for the beat's sounds: see DecideBeatSounds)
  const bool decideSounds = sounds != nullptr;
  if (decideSounds) loopEntities_ = sounds->LoopEntities(), seekingAtBeat_ = seekTo_ != 0;
  worker_->Start([this, decideSounds] {
    using Clock = std::chrono::steady_clock;
    const auto ts = Clock::now();
    sim_->Tick();
    NoteBeatTime();
    NetDigest();  // (a network game's 50th beat: its digest here, right after the beat)
    const auto tp = Clock::now();
    NotePictureBeat();
    const auto tf = Clock::now();
    // (the recorded game's fingerprint here too: it only reads the simulation, ~10 ms a beat in a big game, and so
    // stays off the frames (the engine, Chris's item 20))
    FingerprintBeat();
    workerSimSeconds_ = std::chrono::duration<double>(tp - ts).count();
    workerPictureSeconds_ = std::chrono::duration<double>(tf - tp).count();
    const auto ts2 = Clock::now();
    workerFingerprintSeconds_ = std::chrono::duration<double>(ts2 - tf).count();
    // (and the beat's sounds decided: they read the simulation, ~6-7 ms a beat late in a big game, so they too stay
    // off the frames; played here at FinishBeat)
    if (decideSounds) DecideBeatSounds(*sim_, &beatSounds_), beatSoundsDecided_ = true;
    workerSoundSeconds_ = std::chrono::duration<double>(Clock::now() - ts2).count();
  });
}

// After a beat on the thread: its times, its sounds and shakes, and its sync for the interface.
void GameSession::FingerprintBeat() {  // (after each beat of a recorded live game: its line in <replayOut>.fp)
  if (Fingerprinting()) fp_->Beat(*sim_, ++fpBeats_);
  if (watching_) WatchBeat();
}

// (tests: the state's line after a tick watched, Timeline::Watch; the "lua" part, the sim's Lua heap, left out: it is
// kept out of the fingerprint's total too)
void GameSession::WatchBeat() {
  const uint32_t tick = sim_->tick();
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    if (!watchTicks_.count(tick)) return;
  }
  std::string line = Fingerprint::StateLine(*sim_, tick);
  const size_t lua = line.find(" lua=");
  if (lua != std::string::npos) {
    const size_t end = line.find(' ', lua + 1);
    line.erase(lua, end == std::string::npos ? std::string::npos : end - lua);
  }
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  std::lock_guard<std::mutex> lock(watchMutex_);
  watched_[tick] = std::move(line);
}

void GameSession::FinishBeat() {
  beatToFinish_ = false;
  NetAfterBeat();
  MaxSimRate();
  beatFinishedAt_ = std::chrono::steady_clock::now();
  FrameTimes& times = Times();
  times.simBeats += workerSimSeconds_;
  times.pictureBeats += workerPictureSeconds_;
  ++times.beats;
  // The next beat starts this much early: the longest of the last beats, with some room (beats
  // vary; the old estimate fades slowly), at most a whole beat (2026-10-28: was 80 ms; the late game's chain of a beat,
  // 76 ms on the beats' thread at 95% and its share here, held the world a frame or two a beat at that; the picture
  // keeps six beats, render/sim_frames.cpp, so it may show two behind the newest).
  const double speed = std::pow(10.0, pace.rate / 10.0);
  const double took = (workerSimSeconds_ + workerPictureSeconds_ + workerFingerprintSeconds_ + workerSoundSeconds_) * speed;
  leadBeats_ = std::max(took * 1.3, leadBeats_ * 0.9);
  // (A beat starts only in a frame: one frame's time more, or it starts up to a frame late.)
  // MOHO64UI_LEAD_BEATS=2 (on trial, 2026-10-28): up to two beats, so a heavy beat may begin as soon as the last is
  // through, though the picture has not reached it (in the late game at 4K the frames take 30-40 ms and a beat
  // began a frame late about once a beat); the picture then shows up to two beats behind the newest, and orders wait
  // up to a beat longer, in a heavy game only (the lead is learnt as before).
  static const double maxLead = std::getenv("MOHO64UI_LEAD_BEATS") && std::atoi(std::getenv("MOHO64UI_LEAD_BEATS")) >= 2 ? 0.2 : 0.1;
  lead_ = std::min(maxLead, leadBeats_ + lastFrameSeconds_ * speed);
  static const bool beatLog = std::getenv("MOHO64UI_BEAT_LOG") != nullptr;
  const auto tHere = std::chrono::steady_clock::now();
  if (beatLog)
    Logf(LogLevel::Info, "moho64ui: beat %u on its thread: %.1f ms (sim %.1f, picture %.1f); next starts %.1f ms early; held so far %d",
         sim_->tick(), (workerSimSeconds_ + workerPictureSeconds_) * 1000.0, workerSimSeconds_ * 1000.0,
         workerPictureSeconds_ * 1000.0, lead_ / speed * 1000.0, times.heldPictures);
  NoteBeatHere();
  LongFrames().Note("(beat: after it, here: heights, allies, sounds, shakes)",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tHere).count());
  TakeBeatSync();
  // The sync goes to the interface's scripts here, before the next beat may begin: given while that beat ran, the
  // scripts' sync handlers waited on it (they ask the simulation: UserState::LiveSim), 3.7 -> 66.9 ms a beat at
  // 4,000 units (measured 2026-10-28, docs/ui-benchmarks.md). MOHO64UI_SYNC_AFTER=1: after it is begun (Advance,
  // GiveBeatSync), to measure that again.
  static const bool syncAfter = std::getenv("MOHO64UI_SYNC_AFTER") != nullptr;
  if (!syncAfter) GiveBeatSync();
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
    // The viewer's EndGame (SessionEndGame: FAF's End Replay, then the score screen, which waits for the score the
    // simulation's game-over listeners send, Sync.ScoreAccum): the replay's game ends as a game does on the stream's
    // EndGame (command_stream.cpp case 23): one more beat runs, in which FAF's GameOverListenerThread sees IsGameOver()
    // and the score goes out, then the game stands. Before, the order was dropped here and the score screen waited
    // for ever (Chris's third test, 2026-11-05: "click end replay doesn't work, I'm stuck in game"). INFERRED: the
    // original's replay viewer ends the same way (its EndGame is the session's, not the recorded stream's).
    else if (type == 23) replayEndAsked_ = true, ReplayHeld() = false;
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
  if (beatSoundsDecided_) {  // (decided on the beats' thread)
    ApplyBeatSounds(beatSounds_);
    beatSounds_.clear(), beatSoundsDecided_ = false;
  } else {
    PlayBeatSounds();
  }
  CheckLoopEntities();
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
  loopEntities_ = sounds->LoopEntities();
  seekingAtBeat_ = seekTo_ != 0;
  std::vector<SoundAction> actions;
  DecideBeatSounds(*sim_, &actions);
  ApplyBeatSounds(actions);
}

// The check for running sounds left behind (the review of 2026-11-09, row C): every tenth beat, once the beat's
// sounds are done, each running sound the sound system keeps is looked up in the simulation by its entity's id. One
// whose entity is gone was left by nobody ending it: it would start again whenever the camera comes near its last
// place, for the rest of the game. The count goes to the sound system (moho64ui_SoundInfo().goneLoops, for the
// tests) and, whenever it has grown, to the log with the first one's name. Every tenth beat is often enough to see
// each (an id is given out again only 99 beats after its entity went: Sim::ReleaseId) and costs one look-up a
// running sound a second.
void GameSession::CheckLoopEntities() {
  if (!sounds || !sim_ || sim_->tick() % 10 != 0) return;
  size_t gone = 0;
  uint32_t first = 0;
  for (uint32_t id : sounds->LoopEntities()) {
    if (sim_->FindEntityById(id)) continue;
    if (!gone++) first = id;
  }
  sounds->NoteGoneLoops(static_cast<int>(gone));
  if (gone > goneLoopsSaid_)
    Logf(LogLevel::Warning, "moho64ui: sound: %zu running sound(s) whose entity is gone, at beat %u (the first: entity %u, %s)", gone,
         static_cast<unsigned>(sim_->tick()), static_cast<unsigned>(first), sounds->LoopName(first).c_str());
  goneLoopsSaid_ = gone;
}

// (the simulation is read, the sound system is not: on the beats' thread right after the beat, or here)
void GameSession::DecideBeatSounds(const Sim& sim, std::vector<SoundAction>* out) {
  out->clear();
  std::vector<SoundEvent> events;
  SoundEvents(sim, &events);
  if (events.empty() && foreignLoops_.empty() && loopEntities_.empty()) return;
  auto act = [out](SoundAction::Kind kind, uint32_t entity, const std::string& bank, const std::string& cue, const float place[3]) {
    SoundAction a;
    a.kind = kind, a.entity = entity, a.bank = bank, a.cue = cue;
    for (int i = 0; i < 3; ++i) a.position[i] = place[i];
    out->push_back(std::move(a));
  };
  static const std::string none;
  const int focus = sim.focusArmy + 1;
  std::vector<bool> friendly;  // by army - 1
  for (const ArmyView& a : ArmyViews(sim)) {
    if (a.index < 1) continue;
    if (friendly.size() < static_cast<size_t>(a.index)) friendly.resize(static_cast<size_t>(a.index), false);
    bool ally = focus >= 1 && focus <= static_cast<int>(a.alliance.size()) && a.alliance[static_cast<size_t>(focus - 1)] == 2;
    friendly[static_cast<size_t>(a.index - 1)] = a.index == focus || ally;
  }
  auto isFriendly = [&](int army) { return army >= 1 && army <= static_cast<int>(friendly.size()) && friendly[static_cast<size_t>(army - 1)]; };
  std::vector<UnitView> units;  // in entity-id order
  UnitViews(sim, &units);
  auto isUnit = [&](uint32_t id) {
    auto it = std::lower_bound(units.begin(), units.end(), id, [](const UnitView& u, uint32_t v) { return u.id < v; });
    return it != units.end() && it->id == id;
  };
  auto seen = [&](uint32_t entity, const float place[3]) {
    if (focus < 1) return true;  // no army to ask for: an observer hears all
    uint32_t recon = isUnit(entity) ? ReconOf(sim, focus, entity) : ReconAt(sim, focus, place);
    return (recon & kReconSeenNow) != 0;
  };
  const bool log = std::getenv("MOHO64UI_SOUND_LOG") != nullptr;
  std::vector<uint32_t> startedHere;  // (our running sounds started in this beat: moved with their units below too)
  for (const SoundEvent& e : events) {
    bool ours = isFriendly(e.army);
    if (log)
      Logf(LogLevel::Info, "moho64ui: sound: tick %u %s entity %u army %d at %.0f %.0f %.0f: %s/%s (%s)", static_cast<unsigned>(sim.tick()),
           e.kind == SoundEvent::Play ? "play" : "loop", static_cast<unsigned>(e.entity), e.army, e.position[0], e.position[1],
           e.position[2], e.bank.c_str(), e.cue.c_str(), ours ? "ours" : seen(e.entity, e.position) ? "seen" : "not seen");
    if (e.kind == SoundEvent::Play) {
      // (not while the replay timeline jumps: up to 50 beats a frame; the running sounds below are still followed)
      if (!seekingAtBeat_ && (ours || seen(e.entity, e.position))) act(SoundAction::Play, e.entity, e.bank, e.cue, e.position);
    } else if (ours) {
      act(SoundAction::Loop, e.entity, e.bank, e.cue, e.position);
      if (!e.bank.empty() && !e.cue.empty()) startedHere.push_back(e.entity);
    } else if (e.bank.empty() || e.cue.empty()) {  // another army's running sound ends
      foreignLoops_.erase(e.entity);
      if (heardLoops_.erase(e.entity)) act(SoundAction::Loop, e.entity, none, none, e.position);
      // Or one of ours, ended by the simulation because its entity went without its script ending it (a unit given
      // away, reclaimed, destroyed by a script: Unit.OnDestroy ends no sound). That event names no army (the engine's
      // SoundEvents, sim/effects.cpp: "a loop ends with its entity", army 0), so it comes here and not to the branch
      // above; until 2026-11-10 it was dropped and the sound stayed for the rest of the game (the review's row C;
      // CheckLoopEntities counts such). Ended whenever the sound system has that entity's, or it began in this beat.
      else if (std::binary_search(loopEntities_.begin(), loopEntities_.end(), e.entity) ||
               std::find(startedHere.begin(), startedHere.end(), e.entity) != startedHere.end())
        act(SoundAction::Loop, e.entity, none, none, e.position);
    } else {
      ForeignLoop& loop = foreignLoops_[e.entity];
      if (heardLoops_.erase(e.entity)) act(SoundAction::Loop, e.entity, none, none, e.position);  // it changes: start again below
      loop.bank = e.bank, loop.cue = e.cue;
      for (int i = 0; i < 3; ++i) loop.position[i] = e.position[i];
    }
  }
  // Running sounds follow their units (those the sound system had when the beat began, and ours started in it);
  // another army's start and end as it comes into sight and leaves it.
  std::sort(startedHere.begin(), startedHere.end());
  for (const UnitView& u : units) {
    auto it = foreignLoops_.find(u.id);
    if (it != foreignLoops_.end())
      for (int i = 0; i < 3; ++i) it->second.position[i] = u.position[i];
    if (std::binary_search(loopEntities_.begin(), loopEntities_.end(), u.id) || std::binary_search(startedHere.begin(), startedHere.end(), u.id))
      act(SoundAction::Move, u.id, none, none, u.position);
  }
  for (auto& entry : foreignLoops_) {
    bool hear = seen(entry.first, entry.second.position), heard = heardLoops_.count(entry.first) != 0;
    if (hear && !heard) {
      act(SoundAction::Loop, entry.first, entry.second.bank, entry.second.cue, entry.second.position);
      heardLoops_.insert(entry.first);
    } else if (!hear && heard) {
      act(SoundAction::Loop, entry.first, none, none, entry.second.position);
      heardLoops_.erase(entry.first);
    }
  }
}

// (the sound system, on this thread, in the order decided)
void GameSession::ApplyBeatSounds(const std::vector<SoundAction>& actions) {
  if (!sounds) return;
  for (const SoundAction& a : actions) {
    if (a.kind == SoundAction::Play) sounds->PlayAt(a.bank, a.cue, a.position);
    else if (a.kind == SoundAction::Loop) sounds->SetLoop(a.entity, a.bank, a.cue, a.position);
    else sounds->MoveLoop(a.entity, a.position);
  }
}

void GameSession::RunFirstBeat() {
  if (worker_) worker_->Wait();
  if (!sim_ || sim_->tick() != 0) return;
  if (net_) return;  // (a network game's beat 1 waits for every client's READY, after the game's screen: RunDueBeats)
  // (A replay's first beat is its stream's first: what came before the first Advance, then it.)
  if (play_ && !play_->AtEnd()) play_->ApplyUntilAdvance(*sim_);
  if (Fingerprinting()) fp_->Beat(*sim_, 0);  // (beat 0: the state Start left)
  sim_->Tick();
  NetDigest();
  Record(CommandWriter::AdvanceMessage(1));
  FingerprintBeat();
  NetAfterBeat();
  NoteBeat();
  for (std::string& sync : sim_->TakeSync()) {
    if (onSync) onSync(sync);
    else pendingSync_.push_back(std::move(sync));
  }
  PostPrints(*sim_);
}

void GameSession::StartFirstBeat() {
  if (!beatsOnThread) return RunFirstBeat();
  if (worker_) worker_->Wait();
  if (!sim_ || sim_->tick() != 0 || net_) return;  // (as RunFirstBeat)
  if (play_ && !play_->AtEnd()) play_->ApplyUntilAdvance(*sim_);  // (as RunFirstBeat)
  if (Fingerprinting()) fp_->Beat(*sim_, 0);
  if (!worker_) worker_ = std::make_unique<BeatWorker>();
  firstBeatToFinish_ = true;
  worker_->Start([this] {
    sim_->Tick();
    NetDigest();
    NotePictureBeat();
    FingerprintBeat();  // (as StartBeatOnThread)
  });
}

void GameSession::HoldBeats(bool on) {
  const bool was = holdBeats_;
  holdBeats_ = on;
#ifdef MOHO64UI_WORLD_PICTURE
  if (was && !on && windowPicture_ && windowPicture_->shows == WindowPicture::Kind::Game) windowPicture_->picture.SetWorldShown(true);
#else
  (void)was;
#endif
}

void GameSession::FinishFirstBeat() {
  if (!firstBeatToFinish_) return;
  worker_->Wait();
  firstBeatToFinish_ = false;
  Record(CommandWriter::AdvanceMessage(1));  // (the rest of RunFirstBeat, in its order; its fingerprint ran on the thread)
  NetAfterBeat();
  NoteBeatHere();
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
