// moho64view: the renderer's own program. It shows a map from the player's game files with the game
// camera and plays a tick recording on it; without a map, a test scene.
//
//   moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]
//              [--rec <recording>] [--display <engine log>] [--tick N] [--paused]
//              [--size 1600x900] [--novsync] [--debug] [--lod <pixels>]
//              [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]
//
// --fa      the game's installation folder: its gamedata\*.scd and its maps are used
// --data    a folder of .nx2 / .scd archives used before the game's own (a patched game's files);
//           may be given several times, the first folder wins
// --rec     a tick recording (the engine's MOHO64_RECORD file) of a replay on that map
// --createlog  only for recordings that do not name their units (older ones): the engine's log of the
//           same run made with MOHO64_DEBUG_CREATE=1
// --init <init file> --sim <replay> [--mod-uids <file>] [--simlog <file>]
// --init <init file> --session <game configuration> [--seed N]
//           the same with a new game: the configuration a lobby hands over at Launch (a Lua file with
//           GameConfig = { ... }); its AI armies play, nobody gives orders yet
//           the simulation itself plays the replay inside the viewer (no recording needed): the game's
//           files are mounted by running its init file, the map is the replay's; --tick runs ahead to a tick
// --display the log written by the engine's tools/dump_display.lua: models take their mesh and texture
//           names from the simulation's blueprints (mods' changes included) instead of from .bp files
// --tick, --paused  where playback starts, and whether it stands still
// --mods    a folder of mods (each in its own folder): their units and textures are found too
// --maps    a folder of map folders (maps being worked on), found before the game's own maps
// --at x,z, --heading, --pitch  where --place stands its units, and from which side they are framed
// --crowd <unit id>=<count>  a square block of that unit, to measure what armies cost
// --emit <emitter>  plays that particle emitter of the game over and over where placed units would stand
//           (a name such as destruction_explosion_fire_01, or a blueprint's path); may be given several times
// --emit-scale <n>  the size those emitters are played at
// --map-decals normals|all|albedo|off|corner  which of the map's decals are drawn: normals (the default: only
//           the sculpting of cliffs and erosion), all (also the painted ground details, which as drawn
//           today make the ground too busy), albedo (those alone), off; corner: all, with a decal's position
//           taken as its corner (for finding out what the map file means)
// --decal-turn 1|-1, --decal-range <n>  which way a map decal's heading turns; a factor on the distance up
//           to which the map's decals are drawn
// --lights <n>  n small coloured lights circling the spot placed units stand at (to see and to measure them)
// --picture <file.png>  with --sim or --session: checks the pixel interface for other hosts (world_picture.h):
//           once the game is where --tick asks, the world is drawn through it into that file, its
//           answers about the screen are printed, and the viewer ends
// --hide <kinds>  leaves kinds of effect out, to find what draws what or what costs what: any of
//           particles, bands, decals, with commas between
// --beam <blueprint>, --trail <blueprint>  shows that beam (eight units long, above the same spot) or that
//           trail (behind a point circling it); a name such as phalanx_munition_beam_01, or a path
// --turn <bone>  placed units turn that bone (a name such as Turret, or its number) with all that hangs on
//           it, round and round: to look at a model's moving parts without a game (--bones lists the names)
// --noicons, --icon-scale <n>  strategic icons: off, or n screen pixels for each of a symbol's (key I switches them)
// --ao      ambient occlusion in the modern look: 0 off to 1 full (key O switches it)
// --msaa    anti-aliasing: samples per pixel of the scene, 1 (off), 2, 4 (the default) or 8
// --detail  the models' quality setting: 1 full, 0.5 for a slower card, a huge value forces the finest level
// --preview <unit id>=<folder>  a remastered model's meshes and LabPBR textures, in place of that unit's
// --place <unit id>  stand that unit at the camera's starting point (to look at a model without a recording)
// --look    modern (the default: physically based lighting) or original (the game's own formulas)
// --noshadows  start without the sun's shadows
// --sun, --sky, --exposure, --reflection  the modern look's strengths (for tuning it by eye)
// --time-of-day <hours>  the computed sky of that hour (0..24) lights the modern look instead of the map's
//           own light; --day-length <seconds> lets the day pass, a whole one in that long. Key T: an hour on
//           (from noon when the map's light was on), Y: the map's own light again.
// --border <mesh blueprint>  what the map lies in, as the game's MapBorderAdd names it (may be given twice),
//           for example /meshes/game/map-border_squ_uef_mesh
// --focus <army>  with --sim or --session: the picture is for that army (from 1): other armies' units show
//           only as it knows them (seen, a blip on radar, or not at all)
// --ranges  test range rings round where the camera starts: a red group that glows (three rings that join, one
//           of them with a hole, and one apart) and a blue one
// --unexplored  with --focus: ground that army has never seen is all but black (the game's option
//           FogOfWar "unexplored")
// Key B: life bars over units on and off.
// --clouds <0..1>  with a time of day: how much of the sky clouds cover (0.3 unless given); key C steps through
// --weather <clear|rain|storm|snow|fog>  the weather (it turns a time of day on, noon unless given); key R
//           steps through them. Given as an option, surfaces are wet or snowy from the start.
// --snow-lying <0..1>  how much snow lies at the start (to look at how it builds up and melts)
// --weather auto  the weather changes by itself, by --seed and the game's clock (or the viewer's, without a
//           game); --weather-spell <seconds> how long one kind lasts (300), --climate temperate|cold|dry
// --cam     start the camera there (pitch and heading in degrees; pitch 90 looks straight down)
// --frames N stops after N frames; with --screenshot the last frame is saved (for automatic checks).
//
// Mouse: wheel zooms at the pointer, middle button drags the ground, right button turns and tilts.
// Keys: W A S D or the arrows move, L switches the look, H the shadows, I the strategic icons, F12 saves a screenshot,
// V switches vsync, Escape quits;
// Space pauses, + and - change the speed, Page Up / Page Down jump ten seconds, Home starts over.
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ambient_occlusion.h"
#include "camera.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/mount.h"
#include "debug_draw.h"
#include "decals.h"
#include "effects.h"
#include "frame.h"
#include "gfx.h"
#include "ground.h"
#include "icons.h"
#include "lights.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "models.h"
#include "particles.h"
#include "png.h"
#include "post.h"
#include "live_sim.h"
#include "recording.h"
#include "ribbons.h"
#include "scmap.h"
#include "shadows.h"
#include "terrain.h"
#include "textures.h"
#include "view.h"
#include "water.h"
#include "window.h"
#include "world.h"
#include "world_picture.h"

using namespace render;

namespace {

constexpr float kPi = 3.14159265f;

// Army colours until they are read from the replay: the first of the game's own list.
const Vec3 kArmyColors[8] = {{0.26f, 0.43f, 0.93f}, {0.91f, 0.04f, 0.04f}, {0.38f, 0.43f, 0.49f}, {0.98f, 0.98f, 0.0f},
                             {1.0f, 0.53f, 0.22f},  {0.62f, 0.21f, 0.74f}, {1.0f, 0.53f, 0.93f},  {0.18f, 0.55f, 0.22f}};

struct Options {
  int width = 1600, height = 900;
  bool vsync = true, debug = false, paused = false, shadows = true, icons = true;
  float iconScale = 0;  // screen pixels for each pixel of a strategic icon; 0: by the picture's height
  Look look = Look::Modern;
  int frames = 0;
  float lodPixels = 0;
  // the modern look's strengths, found by eye on the game's maps (the maps' own light values were
  // chosen for the original's formula)
  float sun = 1.4f, sky = 1.1f, exposure = 0.55f, reflection = 0.7f;
  float timeOfDay = -1, dayLength = 0;  // --time-of-day, --day-length
  float clouds = 0.3f;                  // --clouds
  int focus = 0;                        // --focus
  bool unexplored = false;              // --unexplored
  bool ranges = false;                  // --ranges
  float snowLying = -1;                 // --snow-lying
  std::string weather, climate;         // --weather, --climate
  std::vector<std::string> border;      // --border
  float weatherSpell = 300;             // --weather-spell
  float heading = 25, pitch = 32;  // degrees: from where placed units are looked at when no --cam is given
  float at[2] = {};                // --at x,z: where placed units stand (default: the camera's starting target)
  bool hasAt = false;
  double tick = -1;
  std::string screenshot, fa, map, recording, createLog, display;
  std::string init, sim, session, modUids, simLog;  // the simulation run in the viewer (--init, --sim or --session, ...)
  unsigned seed = 0;
  std::vector<std::string> data, mods, maps, previews, place, crowd;
  std::string turn;    // --turn: the bone placed units turn
  std::vector<std::string> emit;  // --emit: emitters played over and over
  float emitScale = 1;
  std::vector<std::string> beam, trail;  // --beam, --trail: shown for a look at them
  std::string hide;    // --hide: kinds of effect left out
  std::string picture; // --picture
  int testLights = 0;  // --lights
  std::string mapDecals = "normals";  // --map-decals
  float decalTurn = 1, decalRange = 1;
  std::string decalFilter;  // --decal-filter: only the map's decals whose texture's name holds this
  bool listBones = false;
  int msaa = 4;        // samples per pixel of the scene (anti-aliasing): 1 off, 2, 4, 8
  float ao = 1;        // ambient occlusion in the modern look: 0 off, 1 full
  float detail = 1;    // the models' quality setting (mesh_renderer.h): 1 full, 0.5 for a slower card, huge = finest always
  bool hasCam = false;
  float cam[5] = {};
};

bool ParseOptions(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--novsync") o->vsync = false;
    else if (a == "--debug") o->debug = true;
    else if (a == "--paused") o->paused = true;
    else if (a == "--noshadows") o->shadows = false;
    else if (a == "--noicons") o->icons = false;
    else if (a == "--unexplored") o->unexplored = true;
    else if (a == "--ranges") o->ranges = true;
    else if (a == "--bones") o->listBones = true;
    else if (!(v = value())) return false;
    else if (a == "--look") {
      if (!strcmp(v, "original")) o->look = Look::Original;
      else if (!strcmp(v, "modern")) o->look = Look::Modern;
      else return false;
    }
    else if (a == "--rec") o->recording = v;
    else if (a == "--createlog") o->createLog = v;
    else if (a == "--display") o->display = v;
    else if (a == "--init") o->init = v;
    else if (a == "--sim") o->sim = v;
    else if (a == "--session") o->session = v;
    else if (a == "--seed") o->seed = static_cast<unsigned>(strtoul(v, nullptr, 0));
    else if (a == "--mod-uids") o->modUids = v;
    else if (a == "--simlog") o->simLog = v;
    else if (a == "--tick") o->tick = atof(v);
    else if (a == "--size") {
      if (sscanf(v, "%dx%d", &o->width, &o->height) != 2) return false;
    } else if (a == "--frames") o->frames = atoi(v);
    else if (a == "--lod") o->lodPixels = static_cast<float>(atof(v));
    else if (a == "--screenshot") o->screenshot = v;
    else if (a == "--fa") o->fa = v;
    else if (a == "--data") o->data.push_back(v);
    else if (a == "--mods") o->mods.push_back(v);
    else if (a == "--maps") o->maps.push_back(v);
    else if (a == "--preview") o->previews.push_back(v);
    else if (a == "--place") o->place.push_back(v);
    else if (a == "--crowd") o->crowd.push_back(v);
    else if (a == "--turn") o->turn = v;
    else if (a == "--emit") o->emit.push_back(v);
    else if (a == "--beam") o->beam.push_back(v);
    else if (a == "--hide") o->hide = v;
    else if (a == "--picture") o->picture = v;
    else if (a == "--lights") o->testLights = atoi(v);
    else if (a == "--map-decals") o->mapDecals = v;
    else if (a == "--decal-filter") o->decalFilter = v;
    else if (a == "--decal-turn") o->decalTurn = static_cast<float>(atof(v));
    else if (a == "--decal-range") o->decalRange = static_cast<float>(atof(v));
    else if (a == "--trail") o->trail.push_back(v);
    else if (a == "--emit-scale") o->emitScale = static_cast<float>(atof(v));
    else if (a == "--detail") o->detail = static_cast<float>(atof(v));
    else if (a == "--msaa") o->msaa = atoi(v);
    else if (a == "--ao") o->ao = static_cast<float>(atof(v));
    else if (a == "--icon-scale") o->iconScale = static_cast<float>(atof(v));
    else if (a == "--sun") o->sun = static_cast<float>(atof(v));
    else if (a == "--sky") o->sky = static_cast<float>(atof(v));
    else if (a == "--exposure") o->exposure = static_cast<float>(atof(v));
    else if (a == "--time-of-day") o->timeOfDay = static_cast<float>(atof(v));
    else if (a == "--day-length") o->dayLength = static_cast<float>(atof(v));
    else if (a == "--clouds") o->clouds = static_cast<float>(atof(v));
    else if (a == "--focus") o->focus = atoi(v);
    else if (a == "--weather") o->weather = v;
    else if (a == "--snow-lying") o->snowLying = static_cast<float>(atof(v));
    else if (a == "--border") o->border.push_back(v);
    else if (a == "--climate") o->climate = v;
    else if (a == "--weather-spell") o->weatherSpell = static_cast<float>(atof(v));
    else if (a == "--reflection") o->reflection = static_cast<float>(atof(v));
    else if (a == "--heading") o->heading = static_cast<float>(atof(v));
    else if (a == "--pitch") o->pitch = static_cast<float>(atof(v));
    else if (a == "--at") {
      if (sscanf(v, "%f,%f", &o->at[0], &o->at[1]) != 2) return false;
      o->hasAt = true;
    }
    else if (a == "--map") o->map = v;
    else if (a == "--cam") {
      if (sscanf(v, "%f,%f,%f,%f,%f", &o->cam[0], &o->cam[1], &o->cam[2], &o->cam[3], &o->cam[4]) != 5) return false;
      o->hasCam = true;
    } else return false;
  }
  return true;
}

// The game's files: every archive of the --data folders, then the game's own, then its maps.
void MountGameFiles(const Options& o, moho::Vfs* vfs) {
  auto archives = [&](const std::string& folder) {
    int n = 0;
    for (const char* pattern : {"/*.nx2", "/*.scd"})
      for (const std::string& name : moho::hostfs::Glob(folder + pattern))
        if (vfs->Mount(folder + "/" + name, "/")) ++n;
    if (!n) fprintf(stderr, "render: no .nx2 or .scd archives in %s\n", folder.c_str());
  };
  // --init: what the game itself mounts (its archives, maps and mods), by running its init file
  if (!o.init.empty()) {
    std::vector<std::string> hooks;
    if (!moho::MountFromInitFile(o.init, {}, *vfs, hooks)) fprintf(stderr, "render: --init %s: the init file could not be run\n", o.init.c_str());
  }
  for (const std::string& folder : o.data) archives(folder);
  // --maps: a folder of map folders (maps being worked on); mounted before the game's own maps, so
  // a map of the same name there is the one that is found
  for (const std::string& folder : o.maps)
    if (!vfs->Mount(folder, "/maps")) fprintf(stderr, "render: --maps %s: no such folder\n", folder.c_str());
  if (!o.fa.empty()) {
    archives(o.fa + "/gamedata");
    if (!vfs->Mount(o.fa + "/maps", "/maps")) fprintf(stderr, "render: no maps folder in %s\n", o.fa.c_str());
  }
  // every mod of a mods folder, each under /mods/<its folder's name>, as the game mounts them
  for (const std::string& folder : o.mods)
    for (const std::string& name : moho::hostfs::ListDirectory(folder))
      if (moho::hostfs::IsDirectory(folder + "/" + name)) vfs->Mount(folder + "/" + name, "/mods/" + name);
  // --preview <unit id>=<folder>: a remastered model's files, tried out in place of that unit's
  for (const std::string& p : o.previews) {
    const size_t eq = p.find('=');
    if (eq == std::string::npos || !vfs->Mount(p.substr(eq + 1), "/preview/" + p.substr(0, eq)))
      fprintf(stderr, "render: --preview %s: expected <unit id>=<existing folder>\n", p.c_str());
  }
}

void DrawTestScene(DebugDraw& draw) {
  for (int i = -128; i <= 128; i += 8) {
    uint32_t c = i == 0 ? 0xff808080u : (i % 64 == 0 ? 0xff585858u : 0xff383838u);
    draw.Line({static_cast<float>(i), 0, -128}, {static_cast<float>(i), 0, 128}, c);
    draw.Line({-128, 0, static_cast<float>(i)}, {128, 0, static_cast<float>(i)}, c);
  }
  draw.Box({0, 2, 0}, {2, 2, 2}, 0xffc0c0c0u);
  draw.Box({12, 1, 0}, {1, 1, 1}, 0xff4040e0u);      // red: +x, east
  draw.Box({0, 1, 12}, {1, 1, 1}, 0xffe06040u);      // blue: +z, south
  draw.Box({0, 9, 0}, {0.5f, 3, 0.5f}, 0xff40c040u); // green: up
  draw.Box({-40, 4, -60}, {6, 4, 10}, 0xff60a0c0u);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseOptions(argc, argv, &opt)) {
    fprintf(stderr, "usage: moho64view [--fa <game folder>] [--data <folder>]... [--map /maps/NAME/NAME.scmap]\n"
                    "                  [--rec <recording>] [--display <engine log>] [--tick N] [--paused]\n"
                    "                  [--size WxH] [--novsync] [--debug] [--lod <pixels>]\n"
                    "                  [--cam x,z,distance,pitch,heading] [--frames N] [--screenshot file.png]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);  // messages appear in order with stderr's, and survive a crash
  gSceneSamples = opt.msaa == 2 || opt.msaa == 4 || opt.msaa == 8 ? opt.msaa : 1;
  std::string error;
  // an automatic run (--frames) must give the same picture whatever the person at the machine is
  // doing: its window stays in the background and what is typed or clicked is not acted on
  const bool automatic = opt.frames > 0;
  auto window = Window::Create("FA Remastered", opt.width, opt.height, &error, automatic);
  if (!window) {
    fprintf(stderr, "render: no window: %s\n", error.c_str());
    return 1;
  }
  gfx::DeviceDesc dd;
  dd.window = window->Handle();
  dd.width = window->Width();
  dd.height = window->Height();
  dd.vsync = opt.vsync;
  dd.debug = opt.debug;
  auto device = gfx::Device::Create(dd, &error);
  if (!device) {
    fprintf(stderr, "render: no graphics device: %s\n", error.c_str());
    return 1;
  }
  printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());

  moho::LogSetEcho(false);  // the engine's file layer reports every archive it opens
  moho::Vfs vfs;
  MountGameFiles(opt, &vfs);
  if (vfs.MountCount()) printf("render: %zu archives and folders, %zu files\n", vfs.MountCount(), vfs.FileCount());
  // --sim: the simulation itself plays the replay, on its own thread; it starts loading at once
  WorldPicture picture;
  LiveSim live;
  const bool haveLive = !opt.sim.empty() || !opt.session.empty();
  if (haveLive) {
    if (opt.init.empty()) {
      fprintf(stderr, "render: --sim and --session need --init <the game's init file>\n");
      return 1;
    }
    if (!opt.simLog.empty()) moho::LogOpenFile(opt.simLog);
    LiveSim::Setup setup;
    setup.initFile = opt.init;
    setup.replay = opt.sim;
    setup.session = opt.session;
    setup.seed = opt.seed;
    setup.maps = opt.maps;
    live.SetFocusArmy(opt.focus);
    picture.SetFocusArmy(opt.focus);
    setup.modUids = opt.modUids;
    if (!live.Open(setup, &error)) {
      fprintf(stderr, "render: simulation: %s\n", error.c_str());
      return 1;
    }
    if (opt.map.empty()) opt.map = live.MapPath();
    if (!opt.picture.empty()) {  // the pixel interface, fed by the same simulation
      if (!picture.Open(&vfs, opt.map, &error)) {
        fprintf(stderr, "render: --picture: %s\n", error.c_str());
        return 1;
      }
      live.onStarted = [&](const moho::Sim& sim) { picture.GameStarted(sim); };
      live.onBeat = [&](const moho::Sim& sim) { picture.Beat(sim); };
    }
    printf("render: simulation: %s %s on %s, loading ...\n", opt.session.empty() ? "the replay" : "a new game from",
           (opt.session.empty() ? opt.sim : opt.session).c_str(), live.MapPath().c_str());
    live.Start();
  }

  // the world's picture: the map and everything drawn on it
  World world(device.get(), &vfs);
  {
    World::Options wo;
    wo.displayTable = opt.display;
    wo.lodPixels = opt.lodPixels;
    wo.mapDecals = opt.mapDecals;
    wo.decalFilter = opt.decalFilter;
    wo.decalTurn = opt.decalTurn;
    wo.decalRange = opt.decalRange;
    wo.verbose = opt.debug;
    if (!world.Open(opt.map, wo, &error)) {
      fprintf(stderr, "render: %s (is --fa or --data given?)\n", error.c_str());
      return 1;
    }
  }
  world.look = opt.look;
  world.shadowsOn = opt.shadows;
  world.iconsOn = opt.icons;
  world.sun = opt.sun;
  world.sky = opt.sky;
  world.exposure = opt.exposure;
  world.timeOfDay = opt.timeOfDay;
  world.cloudCover = opt.clouds;
  world.borders = opt.border;
  world.unexplored = opt.unexplored;
  if (opt.ranges) {  // range rings to look at, round where the camera starts
    const Vec3 at{opt.cam[0], 0, opt.cam[1]};
    RangeGroup fire, radar;
    fire.color = 0xffff3020u;
    fire.glow = 160;
    fire.rings = {{at + Vec3{-14, 0, 0}, 22, 0}, {at + Vec3{12, 0, 6}, 22, 8}, {at + Vec3{2, 0, -4}, 9, 0}, {at + Vec3{40, 0, -30}, 15, 0}};
    radar.color = 0xff20e0ffu;
    radar.rings = {{at + Vec3{0, 0, 10}, 60, 0}, {at + Vec3{70, 0, 20}, 45, 0}};
    world.ranges.groups = {fire, radar};
  }
  int weatherNow = 0;  // which of kWeathers
  static const char* const kWeathers[] = {"clear", "rain", "storm", "snow", "fog"};
  auto setWeather = [&](int which) {
    weatherNow = which;
    world.rain = which == 1 ? 0.5f : which == 2 ? 1.0f : 0.0f;
    world.snow = which == 3 ? 0.8f : 0.0f;
    world.fog = which == 4 ? 1.0f : which == 2 ? 0.25f : 0.0f;
  };
  world.weatherAuto = opt.weather == "auto";
  world.weatherSeed = opt.seed ? opt.seed : 1;
  world.weatherSpell = opt.weatherSpell;
  world.climate = opt.climate == "cold" ? Climate::Cold : opt.climate == "dry" ? Climate::Dry : Climate::Temperate;
  for (int i = 0; i < 5; ++i)
    if (opt.weather == kWeathers[i]) {
      setWeather(i);
      world.wet = world.rain > 0 ? 1.0f : 0.0f;
      world.snowLying = world.snow > 0 ? 1.0f : 0.0f;
    }
  if (opt.snowLying >= 0) world.snowLying = opt.snowLying;
  if (opt.dayLength > 0 && world.timeOfDay < 0) world.timeOfDay = 12;
  world.reflection = opt.reflection;
  world.hide = opt.hide;
  world.meshes.detail = opt.detail;
  world.icons.scale = opt.iconScale;
  world.ao.strength = opt.ao;
  // the world's parts that the viewer's own switches add to, or whose numbers it prints
  const MapData& map = world.map;
  const bool haveMap = world.HaveMap();
  const Terrain* terrain = &world.terrain;
  ModelCache& models = world.models;
  MeshRenderer& meshes = world.meshes;
  Particles& particles = world.particles;
  Ribbons& ribbons = world.ribbons;
  Decals& decals = world.decals;
  Lights& lights = world.lights;
  Effects& effects = world.effects;
  StrategicIcons& icons = world.icons;
  DebugDraw& draw = world.draw;
  AmbientOcclusion& ao = world.ao;
  Look& look = world.look;
  float dayShift = 0;  // hours added with T
  bool dayOn = opt.timeOfDay >= 0 || opt.dayLength > 0 || weatherNow != 0 || world.weatherAuto;  // a time of day lights (T), or the map's own light (Y)
  bool& shadowsOn = world.shadowsOn;
  bool& iconsOn = world.iconsOn;
  std::vector<FrameEffectEvent> effectEvents;
  Recording recording;
  bool haveRecording = false;
  if (!opt.recording.empty()) {
    if (!recording.Load(opt.recording, &error)) {
      fprintf(stderr, "render: recording %s: %s\n", opt.recording.c_str(), error.c_str());
      return 1;
    }
    haveRecording = true;
    const int fromLog = opt.createLog.empty() ? 0 : recording.LoadCreateLog(opt.createLog);
    printf("render: recording %s: ticks %u to %u, %d units named by %s\n", opt.recording.c_str(), recording.FirstTick(),
           recording.LastTick(), recording.NamedUnits() + fromLog, recording.NamedUnits() ? "the recording" : "the log");
    if (!recording.NamedUnits() && !fromLog)
      fprintf(stderr, "render: this recording does not say what its units are (an older one, or the original game's) and no\n"
                      "        --createlog (an engine log made with MOHO64_DEBUG_CREATE=1) was given: units are drawn as boxes\n");
  }
  Frame frame;
  // where the frames come from: a recording, or the running simulation
  const bool havePlay = haveRecording || haveLive;
  auto firstTick = [&]() { return haveLive ? live.FirstTick() : recording.FirstTick(); };
  auto lastTick = [&]() { return haveLive ? live.LastTick() : recording.LastTick(); };
  auto frameAt = [&](double tick, Frame* out) {
    if (haveLive) live.FrameAt(tick, out);
    else recording.FrameAt(tick, out);
  };
  std::vector<Vec3> armyColors(std::begin(kArmyColors), std::end(kArmyColors));  // replaced by the game's own
  bool liveReported = false, framedLive = false, liveBusy = false;  // busy: loading, or running ahead to --tick
  double playTick = haveRecording ? (opt.tick >= 0 ? opt.tick : recording.FirstTick()) : 0;
  double playSpeed = 1;  // 1: ten ticks a second, as the game runs at normal speed
  bool paused = opt.paused;

  Camera camera;
  float maxDistance = 6000;
  if (haveMap) {  // the whole map from straight above
    const float w = static_cast<float>(map.width), h = static_cast<float>(map.height);
    camera.target = {w * 0.5f, 0, h * 0.5f};
    float aspect = static_cast<float>(window->Width()) / static_cast<float>(std::max(1, window->Height()));
    float fit = std::max(h, w / aspect) * 0.5f / std::tan(camera.fovY * 0.5f);
    camera.distance = fit * 1.05f;
    camera.pitch = kPi / 2;
    maxDistance = fit * 1.6f;
  } else {
    camera.heading = 0.5f;
    camera.pitch = 0.75f;
    camera.distance = 70;
  }
  if (opt.hasCam) {
    camera.target = {opt.cam[0], 0, opt.cam[1]};
    camera.distance = opt.cam[2];
    camera.pitch = opt.cam[3] * kPi / 180;
    camera.heading = opt.cam[4] * kPi / 180;
  }
  // points the camera at the recording's units as they stand at the current moment (key F)
  auto lookAtUnits = [&]() {
    frameAt(playTick, &frame);
    Vec3 lo{1e30f, 0, 1e30f}, hi{-1e30f, 0, -1e30f};
    for (const FrameUnit& u : frame.units) {
      if (u.dead) continue;
      lo = {std::min(lo.x, u.position[0]), 0, std::min(lo.z, u.position[2])};
      hi = {std::max(hi.x, u.position[0]), 0, std::max(hi.z, u.position[2])};
    }
    if (lo.x > hi.x) return;
    camera.target = (lo + hi) * 0.5f;
    camera.distance = Clamp(std::max(hi.x - lo.x, hi.z - lo.z) * 1.2f, 25.0f, maxDistance);
    camera.pitch = 50 * kPi / 180;
    camera.heading = 0;
  };
  if (haveRecording && !opt.hasCam) lookAtUnits();
  // --place <unit id>: stands that unit at the camera's starting target (several: side by side),
  // to look at a model without a recording
  struct PlacedUnit {
    const Model* model;
    Vec3 position;
    const StrategicIcons::Kind* icon;
    int turned = -1;               // --turn: the bone it turns
    std::vector<FrameBone> bones;  // this frame's pose
  };
  std::vector<PlacedUnit> placedUnits;
  float targetLift = 0;  // how far above the ground the camera's target is held
  {
    auto stand = [&](const Model* model, Vec3 p, const std::string& id) {
      p.y = haveMap ? map.Height(p.x, p.z) : 0;
      if (haveMap && map.water.present) p.y = std::max(p.y, map.water.elevation);
      PlacedUnit placed{model, p, icons.ForBlueprint(models.UnitBlueprintPath(id)), -1, {}};
      if (opt.listBones && (placedUnits.empty() || placedUnits.back().model != model)) {
        printf("render: %s: %zu bones:", id.c_str(), model->bones.size());
        for (size_t i = 0; i < model->bones.size(); ++i) printf(" %zu %s", i, model->bones[i].name.c_str());
        printf("\n");
      }
      if (!opt.turn.empty()) {  // by name (as the file spells it, in any case), or by number
        std::string wanted = opt.turn;
        for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (size_t i = 0; i < model->bones.size() && placed.turned < 0; ++i)
          if (model->bones[i].name == wanted) placed.turned = static_cast<int>(i);
        if (placed.turned < 0 && !wanted.empty() && std::isdigit(static_cast<unsigned char>(wanted[0])) &&
            static_cast<size_t>(atoi(wanted.c_str())) < model->bones.size())
          placed.turned = atoi(wanted.c_str());
      }
      placedUnits.push_back(std::move(placed));
    };
    Vec3 at = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
    for (const std::string& id : opt.place) {
      const Model* model = models.Unit(id);
      if (!model->ok) continue;
      const float width = (model->boundsMax.x - model->boundsMin.x) * model->scale;
      stand(model, at, id);
      // a remastered unit gets the game's own model beside it, in the same light, to compare
      const Model* original = models.Remastered(id) ? models.Unit(id, true) : nullptr;
      if (original && original->ok) {
        const float originalWidth = (original->boundsMax.x - original->boundsMin.x) * original->scale;
        at.x += (width + originalWidth) * 0.5f + std::max(width, originalWidth) * 0.25f;
        stand(original, at, id);
        at.x += originalWidth * 1.5f;
      } else {
        at.x += width * 1.5f;
      }
    }
    // --crowd <unit id>=<count>: that many copies in a square block, to measure what many units cost
    for (const std::string& c : opt.crowd) {
      const size_t eq = c.find('=');
      const std::string id = c.substr(0, eq);
      const Model* model = models.Unit(id);
      const int count = eq == std::string::npos ? 100 : atoi(c.c_str() + eq + 1);
      if (!model->ok || count <= 0) continue;
      const Vec3 size = (model->boundsMax - model->boundsMin) * model->scale;
      const float spacing = std::max(size.x, size.z) * 1.3f;
      const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
      for (int i = 0; i < count; ++i)
        stand(model, at + Vec3{(static_cast<float>(i % side) - static_cast<float>(side - 1) * 0.5f) * spacing, 0,
                               (static_cast<float>(i / side) - static_cast<float>(side - 1) * 0.5f) * spacing}, id);
    }
    // without --cam the camera frames what was placed; --heading and --pitch choose the side
    if (!opt.hasCam && !placedUnits.empty()) {
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (const PlacedUnit& u : placedUnits) {
        const Vec3 a = u.position + u.model->boundsMin * u.model->scale, b = u.position + u.model->boundsMax * u.model->scale;
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
      }
      camera.target = (lo + hi) * 0.5f;
      targetLift = (hi.y - lo.y) * 0.5f;  // look at the units' middle, not at their feet
      camera.distance = Length(hi - lo) * 0.5f / std::tan(camera.fovY * 0.5f) * 0.95f;
      camera.heading = opt.heading * kPi / 180;
      camera.pitch = opt.pitch * kPi / 180;
    }
  }
  // --emit: where the emitters play, and when each was last started
  const Vec3 emitAt = opt.hasAt ? Vec3{opt.at[0], 0, opt.at[1]} : camera.target;
  std::vector<uint32_t> emitHandles(opt.emit.size(), 0);
  float emitStarted = -1e9f;
  auto effectPath = [](const std::string& name) { return name.find('/') != std::string::npos ? name : "/effects/emitters/" + name + "_emit.bp"; };
  std::vector<uint32_t> trailHandles(opt.trail.size(), 0);
  auto groundHeight = [&](float x, float z) {
    if (!haveMap) return 0.0f;
    float y = map.Height(x, z);
    return map.water.present ? std::max(y, map.water.elevation) : y;
  };
  camera.target.y = groundHeight(camera.target.x, camera.target.z) + targetLift;
  // what the pointer is on: the ground, or failing that the level plane through the camera's target
  auto pick = [&](int px, int py, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    if (h <= 0) return false;
    Ray ray = camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h);
    if (haveMap && HitGround(map, ray, true, hit)) return true;
    return HitPlaneY(ray, camera.target.y, hit);
  };
  auto pickAtHeight = [&](int px, int py, float y, Vec3* hit) {
    const float w = static_cast<float>(window->Width()), h = static_cast<float>(window->Height());
    return h > 0 && HitPlaneY(camera.PixelRay(static_cast<float>(px), static_cast<float>(py), w, h), y, hit);
  };

  std::vector<InputEvent> events;
  std::vector<uint8_t> capture;
  bool vsync = opt.vsync, wantScreenshot = false;
  int dragX = 0, dragY = 0;
  Vec3 grab;
  bool grabbing = false;
  const auto start = std::chrono::steady_clock::now();
  auto last = start;
  double titleTime = 0;
  int titleFrames = 0, frameCount = 0, shotNumber = 0;
  auto countedFrom = start;

  while (window->Pump(&events)) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    bool quit = false;
    if (automatic) events.clear();
    for (const InputEvent& e : events) {
      switch (e.type) {
        case InputEvent::KeyDown:
          if (e.key == kKeyEscape) quit = true;
          if (e.key == kKeyF12) wantScreenshot = true;
          if (e.key == 'V') device->SetVsync(vsync = !vsync);
          if (e.key == kKeySpace) paused = !paused;
          if (e.key == kKeyPlus) playSpeed = std::min(playSpeed * 2, 64.0);
          if (e.key == kKeyMinus) playSpeed = std::max(playSpeed / 2, 0.125);
          if (e.key == kKeyPageDown) playTick += 100;
          if (e.key == kKeyPageUp) playTick -= 100;
          if (e.key == kKeyHome && haveRecording) playTick = recording.FirstTick();
          if (e.key == 'F' && havePlay) lookAtUnits();
          if (e.key == 'L') look = look == Look::Modern ? Look::Original : Look::Modern;
          if (e.key == 'H') shadowsOn = !shadowsOn;
          if (e.key == 'B') world.lifeBars = !world.lifeBars;
          if (e.key == 'T') {
            if (dayOn) dayShift += 1;
            dayOn = true;
          }
          if (e.key == 'Y') dayOn = false;
          if (e.key == 'R') {
            world.weatherAuto = false;
            setWeather((weatherNow + 1) % 5);
            dayOn = true;
            printf("render: weather: %s\n", kWeathers[weatherNow]);
          }
          if (e.key == 'C') world.cloudCover = world.cloudCover >= 0.95f ? 0 : std::min(world.cloudCover + 0.25f, 1.0f);
          if (e.key == 'I') iconsOn = !iconsOn;
          if (e.key == 'O') ao.strength = ao.strength > 0 ? 0 : (opt.ao > 0 ? opt.ao : 1);
          break;
        case InputEvent::MouseDown:
          dragX = e.x;
          dragY = e.y;
          if (e.key == 2) grabbing = pick(e.x, e.y, &grab);
          break;
        case InputEvent::MouseUp:
          if (e.key == 2) grabbing = false;
          break;
        case InputEvent::MouseMove:
          if (window->MouseDown(1)) {
            camera.heading -= static_cast<float>(e.x - dragX) * 0.005f;
            camera.pitch = Clamp(camera.pitch + static_cast<float>(e.y - dragY) * 0.005f, 0.1f, kPi / 2);
          } else if (grabbing) {
            // keep the grabbed ground point under the pointer
            Vec3 hit;
            if (pickAtHeight(e.x, e.y, grab.y, &hit)) camera.target += Vec3{grab.x - hit.x, 0, grab.z - hit.z};
          }
          dragX = e.x;
          dragY = e.y;
          break;
        case InputEvent::Wheel: {
          // zoom at the pointer: the ground point under it stays where it is
          Vec3 before, after;
          bool hit = pick(e.x, e.y, &before);
          camera.distance = Clamp(camera.distance * std::pow(0.85f, e.wheel), 3.0f, maxDistance);
          if (hit && pickAtHeight(e.x, e.y, before.y, &after)) camera.target += Vec3{before.x - after.x, 0, before.z - after.z};
          break;
        }
        default:
          break;
      }
    }
    events.clear();
    if (quit) break;

    if (!automatic) {  // keyboard movement, along the ground in the camera's directions
      Vec3 r, u, b;
      camera.Axes(&r, &u, &b);
      Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
      float step = camera.distance * 1.2f * dt;
      if (window->KeyDown('W') || window->KeyDown(kKeyUp)) camera.target += forward * step;
      if (window->KeyDown('S') || window->KeyDown(kKeyDown)) camera.target -= forward * step;
      if (window->KeyDown('D') || window->KeyDown(kKeyRight)) camera.target += r * step;
      if (window->KeyDown('A') || window->KeyDown(kKeyLeft)) camera.target -= r * step;
    }
    if (haveMap) {
      camera.target.x = Clamp(camera.target.x, 0, static_cast<float>(map.width));
      camera.target.z = Clamp(camera.target.z, 0, static_cast<float>(map.height));
    }
    camera.target.y = groundHeight(camera.target.x, camera.target.z) + targetLift;

    if (window->Width() == 0 || window->Height() == 0) {  // minimized
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (window->Width() != device->Width() || window->Height() != device->Height()) device->Resize(window->Width(), window->Height());

    // an automatic run counts time in frames, so that its pictures can be compared
    const float time = automatic ? static_cast<float>(frameCount) / 60.0f : std::chrono::duration<float>(now - start).count();

    // the game: how far the simulation is, and the frame to show
    if (haveLive) {
      const LiveSim::State state = live.GetState();
      if (state != LiveSim::State::Loading && !liveReported) {
        liveReported = true;
        if (state == LiveSim::State::Failed) fprintf(stderr, "render: simulation: %s\n", live.Problem().c_str());
        else printf("render: simulation: started after %.1f s\n", live.LoadSeconds());
        std::vector<Vec3> own = live.ArmyColors();
        if (!own.empty()) armyColors = std::move(own);
        std::vector<BlueprintLook> looks;  // how the blueprints look as the simulation has them
        live.TakeLooks(&looks);
        world.SetLooks(looks);
        if (live.Seed()) world.weatherSeed = live.Seed();  // the weather every player of this game sees
        live.SetSightGrid(static_cast<float>(world.map.width), static_cast<float>(world.map.height));
      }
      {  // what the focus army sees, when it has been worked out anew
        std::vector<uint8_t> seen;
        int cells = 0;
        if (live.TakeSight(&seen, &cells)) world.sight.Set(seen, cells);
      }
      // the simulation is asked to stay two beats ahead of what is shown, so there are always two to blend
      if (state == LiveSim::State::Running) live.RunTo(static_cast<uint32_t>(playTick) + 2);
      // --tick: run ahead to that tick as fast as the simulation goes, showing its newest beat on the way
      liveBusy = state == LiveSim::State::Loading;
      if (state == LiveSim::State::Running && opt.tick > playTick) {
        live.RunTo(static_cast<uint32_t>(opt.tick) + 2);
        playTick = std::min(static_cast<double>(live.LastTick()), opt.tick);
        liveBusy = playTick < opt.tick;
      } else if (state != LiveSim::State::Loading) {
        opt.tick = -1;  // reached, or the game ended before it
      }
    }
    const bool playing = havePlay && (!haveLive || live.GetState() != LiveSim::State::Loading);
    if (playing) {
      if (!paused) playTick += static_cast<double>(dt) * 10 * playSpeed;
      playTick = std::clamp(playTick, static_cast<double>(firstTick()), static_cast<double>(lastTick()));
      frameAt(playTick, &frame);
      if (haveLive && !framedLive && !opt.hasCam && !frame.units.empty()) {  // the first look at the game
        framedLive = true;
        lookAtUnits();
      }
    }

    if (!opt.picture.empty() && playing && !liveBusy && frameCount >= 3) {
      // --picture: the world through the pixel interface, and what it says about the screen
      const int w = device->Width(), h = device->Height();
      {  // what the interface shows in the world: some units selected, a building's ghost, a line and a ring
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < frame.units.size(); i += 3) ids.push_back(frame.units[i].id);
        picture.SetSelection(ids);
        // and marks as under the mouse: the first three units not selected, one of each kind in turn
        if (!frame.units.empty()) {
          const size_t which = 1 + 3 * ((frameCount / 2) % 3);
          if (which < frame.units.size()) picture.SetHover(frame.units[which].id, 1 + static_cast<int>((frameCount / 2) % 3));
        }
        Vec3 spot;
        if (picture.Draw(camera, w, h, playTick + 1).size() && picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.75f, &spot)) {
          WorldPicture::BuildPreview free{"ueb0101", {spot.x, spot.y, spot.z}, 0, true}, blocked{"ueb1101", {spot.x + 8, spot.y, spot.z}, 0.6f, false};
          picture.SetBuildPreviews({free, blocked});
          WorldPicture::Line line{{spot.x, spot.y, spot.z}, {camera.target.x, camera.target.y, camera.target.z}, 0xff40ff40u};
          WorldPicture::Ring ring{{camera.target.x, camera.target.y, camera.target.z}, 6, 0xffff4040u};
          picture.SetShapes({line}, {ring});
        }
      }
      const std::vector<uint32_t>& pixels = picture.Draw(camera, w, h, playTick + 1);
      std::vector<uint8_t> bytes(pixels.size() * 4);
      for (size_t i = 0; i < pixels.size(); ++i) {
        bytes[i * 4] = static_cast<uint8_t>(pixels[i] >> 16);
        bytes[i * 4 + 1] = static_cast<uint8_t>(pixels[i] >> 8);
        bytes[i * 4 + 2] = static_cast<uint8_t>(pixels[i]);
        bytes[i * 4 + 3] = 255;
      }
      printf("render: --picture: %dx%d %s\n", w, h, WritePng(opt.picture, bytes.data(), w, h) ? opt.picture.c_str() : "could not be written");
      {  // what a call costs, read-back included
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) picture.Draw(camera, w, h, playTick + 1);
        printf("render: --picture: %.2f ms a call\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
      }
      {  // the minimap, in the map's proportion
        float mw, mh;
        picture.MapSize(&mw, &mh);
        const int mapH = 400, mapW = mh > 0 ? static_cast<int>(400 * mw / mh) : 400;
        const std::vector<uint32_t>& mini = picture.DrawMinimap(mapW, mapH);
        std::vector<uint8_t> miniBytes(mini.size() * 4);
        for (size_t i = 0; i < mini.size(); ++i) {
          miniBytes[i * 4] = static_cast<uint8_t>(mini[i] >> 16);
          miniBytes[i * 4 + 1] = static_cast<uint8_t>(mini[i] >> 8);
          miniBytes[i * 4 + 2] = static_cast<uint8_t>(mini[i]);
          miniBytes[i * 4 + 3] = 255;
        }
        const std::string miniPath = opt.picture.substr(0, opt.picture.rfind('.')) + "_minimap.png";
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) picture.DrawMinimap(mapW, mapH);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30;
        Vec3 back;
        float mx = 0, my = 0;
        const bool there = picture.MinimapProject(camera.target, &mx, &my) && picture.MinimapGroundAt(mx, my, &back);
        printf("render: --picture: minimap %dx%d %s, %.2f ms a call; the camera's target %.1f %.1f is at %.1f %.1f on it, and back %.1f %.1f%s\n", mapW, mapH,
               WritePng(miniPath, miniBytes.data(), mapW, mapH) ? miniPath.c_str() : "could not be written", each, camera.target.x, camera.target.z, mx, my,
               back.x, back.z, there ? "" : " (failed)");
        picture.Draw(camera, w, h, playTick + 1);  // (the device goes back to the picture's size)
      }
      std::vector<uint32_t> inside;
      picture.UnitsIn(0, 0, static_cast<float>(w), static_cast<float>(h), &inside);
      printf("render: --picture: %zu units in the picture of %zu in the game\n", inside.size(), frame.units.size());
      int shown = 0;
      for (const FrameUnit& u : frame.units) {
        float x, y;
        if (!picture.Project({u.position[0], u.position[1], u.position[2]}, &x, &y) || x < 0 || y < 0 || x >= static_cast<float>(w) || y >= static_cast<float>(h)) continue;
        Vec3 ground;
        const bool hit = picture.GroundAt(x, y, &ground);
        const uint32_t under = picture.UnitAt(x, y);
        printf("render: --picture: unit %x at %.1f %.1f %.1f -> pixel %.0f %.0f; the ground there %.1f %.1f %.1f%s; the unit there %x%s\n", u.id,
               u.position[0], u.position[1], u.position[2], x, y, ground.x, ground.y, ground.z, hit ? "" : " (none)", under,
               under == u.id ? " (itself)" : "");
        if (++shown == 6) break;
      }
      break;
    }
    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    device->BeginFrame(clear);
    // effects run on the game's clock while a game plays (so they stand still when it is paused)
    {  // the time of day: where it started, what T added, and what has passed of a running day
      float hours = (opt.timeOfDay >= 0 ? opt.timeOfDay : 12) + dayShift;
      if (opt.dayLength > 0) hours += time / opt.dayLength * 24;
      world.timeOfDay = dayOn ? std::fmod(hours, 24.0f) : -1;
    }
    world.Begin(camera, device->Width(), device->Height(), time, havePlay ? playTick * 0.1 : static_cast<double>(time));

    // --emit: every emitter is started again once all have ended and three seconds have passed
    if (!opt.emit.empty()) {
      bool running = false;
      for (uint32_t h : emitHandles) running = running || particles.Running(h);
      if (!running && time - emitStarted > 3) {
        emitStarted = time;
        for (size_t i = 0; i < opt.emit.size(); ++i) {
          const std::string& name = opt.emit[i];
          emitHandles[i] = particles.Start(effectPath(name), {emitAt.x, groundHeight(emitAt.x, emitAt.z) + 0.25f * opt.emitScale, emitAt.z}, Quat{}, opt.emitScale);
        }
      }
    }
    if (!opt.beam.empty() || !opt.trail.empty()) {  // --beam, --trail
      const Vec3 base{emitAt.x, groundHeight(emitAt.x, emitAt.z) + 1, emitAt.z};
      for (size_t i = 0; i < opt.beam.size(); ++i)
        ribbons.Beam(effectPath(opt.beam[i]), base + Vec3{0, static_cast<float>(i) * 0.6f, 0}, base + Vec3{8, static_cast<float>(i) * 0.6f, 0});
      for (size_t i = 0; i < opt.trail.size(); ++i) {
        const float angle = time * 2, radius = 3 + static_cast<float>(i);
        const Vec3 at = base + Vec3{std::cos(angle) * radius, 0, std::sin(angle) * radius};
        if (!trailHandles[i]) trailHandles[i] = ribbons.StartTrail(effectPath(opt.trail[i]), at);
        ribbons.MoveTrail(trailHandles[i], at);
      }
    }
    for (int i = 0; i < opt.testLights; ++i) {  // --lights: in rings around the spot, each its own colour
      const float ring = 3 + 2.5f * static_cast<float>(i / 12), angle = time * (0.5f + 0.07f * static_cast<float>(i % 5)) + static_cast<float>(i) * 0.83f;
      const Vec3 at{emitAt.x + std::cos(angle) * ring, 0, emitAt.z + std::sin(angle) * ring};
      const float hue = static_cast<float>(i) * 2.4f;
      lights.Add({at.x, groundHeight(at.x, at.z) + 1.2f, at.z}, 6,
                 Vec3{0.5f + 0.5f * std::cos(hue), 0.5f + 0.5f * std::cos(hue + 2.1f), 0.5f + 0.5f * std::cos(hue + 4.2f)} * 3.0f);
    }
    for (PlacedUnit& u : placedUnits) {
      Pose pose;
      if (u.turned >= 0) {
        // the pose a simulation would hand over for that bone turned about the unit's upright axis:
        // the bone and all that hangs on it, each where the turn about the bone's own place carries it
        const std::vector<ModelBone>& bones = u.model->bones;
        const float angle = time * 0.8f, scale = u.model->scale;
        const Quat yaw{0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f)};
        const Vec3 pivot = bones[static_cast<size_t>(u.turned)].position * scale;
        u.bones.clear();
        for (size_t i = static_cast<size_t>(u.turned); i < bones.size(); ++i) {
          int up = static_cast<int>(i);
          while (up >= 0 && up != u.turned) up = bones[static_cast<size_t>(up)].parent;
          if (up != u.turned) continue;
          const Vec3 p = pivot + Rotate(yaw, bones[i].position * scale - pivot);
          const Quat q = Mul(yaw, bones[i].rotation);
          u.bones.push_back({static_cast<uint32_t>(i), {p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}});
        }
        pose.now = pose.next = u.bones.data();
        pose.nowCount = pose.nextCount = u.bones.size();
      }
      meshes.Add(u.model, Mat4::FromTRS(u.position, Quat{}, u.model->scale), kArmyColors[0], 1, u.turned >= 0 ? &pose : nullptr);
      world.AddLamps(u.model, u.position, Quat{}, u.turned >= 0 ? &pose : nullptr, &kArmyColors[0]);
      icons.Add(u.icon, u.position, kArmyColors[0]);
    }
    if (haveLive && playing) {  // what the scripts did with effects in the beats since the last frame
      live.TakeEffectEvents(&effectEvents);
      effects.Queue(std::move(effectEvents));
      effects.Update(playTick, frame);
    }
    if (playing) world.AddFrame(frame, armyColors);
    if (!haveMap) DrawTestScene(draw);
    world.Render();

    if (!(automatic && liveBusy)) {  // an automatic run counts its frames from where it was asked to look
      if (frameCount == 0) countedFrom = std::chrono::steady_clock::now();
      ++frameCount;
    }
    const bool lastFrame = opt.frames > 0 && frameCount >= opt.frames;
    const bool shoot = wantScreenshot || (lastFrame && !opt.screenshot.empty());
    device->EndFrame(shoot ? &capture : nullptr);
    if (shoot && !capture.empty()) {
      std::string path = wantScreenshot ? "screenshot" + std::to_string(++shotNumber) + ".png" : opt.screenshot;
      if (WritePng(path, capture.data(), device->Width(), device->Height())) printf("render: wrote %s\n", path.c_str());
      else fprintf(stderr, "render: cannot write %s\n", path.c_str());
      wantScreenshot = false;
    }
    if (lastFrame) {
      // (with a simulation: the time from the first counted frame, so loading and running ahead stay out)
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - (haveLive ? countedFrom : start)).count();
      const int timed = haveLive ? std::max(1, frameCount - 1) : frameCount;
      printf("render: %d frames in %.2f s (%.2f ms each)", frameCount, seconds, 1000.0 * seconds / timed);
      if (haveMap) printf("; ground: %d patches, %d triangles", terrain->drawnPatches, terrain->drawnTriangles);
      printf("; models: %d drawn, %d triangles, %d more in shadow maps", meshes.drawnInstances, meshes.drawnTriangles, meshes.shadowTriangles);
      if (iconsOn) printf("; %d icons", icons.drawn);
      if (particles.liveParticles || particles.liveEmitters)
        printf("; %d particles of %d emitters (%d quiet), %.2f ms", particles.liveParticles, particles.liveEmitters, particles.quietEmitters, particles.cpuMs);
      if (havePlay) printf("; %d posed, %zu projectiles, %zu beams", meshes.posedInstances, frame.projectiles.size(), frame.beams.size());
      if (meshes.posedInstances) printf("; bones %.2f ms", meshes.prepareMs);
      if (lights.count) printf("; %d lights", lights.count);
      if (opt.focus > 0) printf("; %d buildings remembered, %zu of them gone", world.RememberedCount(), world.rememberedGone.size());      if (haveLive)
        printf("; effects: %d live of %d started, %d events of kinds not drawn yet, %d bands, %.2f ms; %d decals drawn of %d", effects.live,
               effects.started, effects.notShown, ribbons.drawnBands, effects.cpuMs, decals.drawn, decals.Count());
      if (haveLive) printf("; simulation at tick %u, %.2f ms a tick", live.LastTick(), live.MsPerTick());
      printf("\n");
      break;
    }

    titleTime += dt;
    ++titleFrames;
    if (titleTime >= 0.5) {
      char title[320];
      int n = snprintf(title, sizeof title, "FA Remastered - %s - %s - %.2f ms (%.0f fps)%s", device->BackendName(),
                       device->AdapterName().c_str(), 1000.0 * titleTime / titleFrames, titleFrames / titleTime, vsync ? "" : " - vsync off");
      if (haveMap && n > 0 && n < static_cast<int>(sizeof title))
        n += snprintf(title + n, sizeof title - static_cast<size_t>(n), " - ground: %d patches, %d triangles", terrain->drawnPatches, terrain->drawnTriangles);
      if (havePlay && n > 0 && n < static_cast<int>(sizeof title)) {
        const int seconds = static_cast<int>(playTick / 10);
        n += snprintf(title + n, sizeof title - static_cast<size_t>(n), " - %d:%02d (tick %d) x%g%s - %d units, %d triangles", seconds / 60, seconds % 60,
                      static_cast<int>(playTick), playSpeed, paused ? " paused" : "", meshes.drawnInstances, meshes.drawnTriangles);
        if (haveLive && n > 0 && n < static_cast<int>(sizeof title)) {
          const LiveSim::State state = live.GetState();
          snprintf(title + n, sizeof title - static_cast<size_t>(n), " - simulation %s, %.1f ms a tick",
                   state == LiveSim::State::Loading ? "loading" : state == LiveSim::State::Running ? "running" : state == LiveSim::State::Ended ? "ended" : "FAILED",
                   live.MsPerTick());
        }
      }
      window->SetTitle(title);
      titleTime = 0;
      titleFrames = 0;
    }
  }
  return 0;
}
