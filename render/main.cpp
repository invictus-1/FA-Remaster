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
// --window-cycle  with --map and --screenshot <name>.png: one window shown by a menu's picture (the interface
//           alone), then a game's (the map), then a menu's again, each closed before the next opens on it, as
//           the interface does; what each showed is written as <name>_menu1.png, _game.png, _menu2.png
// --picture-window  with --picture: the interface's way without a read-back: the picture drawn into a window of
//           its own with a made-up interface layer over it (UseWindow, SetInterfaceLayer, DrawToWindow), read
//           back once for the file
// --picture <file.png>  with --sim or --session: checks the pixel interface for other hosts (world_picture.h):
//           once the game is where --tick asks, the world is drawn through it into that file, its
//           answers about the screen are printed (and the life bars' anchors checked and marked in
//           magenta, where custom names go), and the viewer ends
// --settings <bundles>  with --picture: the game's lighting settings, as the interface sets them (WorldPicture's
//           bundles): any of atmosphere, soft-shadows, ray-tracing, weather-effects, ground-detail, with commas between
// --hide <kinds>  leaves kinds of effect out, to find what draws what or what costs what: any of
//           particles, bands, decals, with commas between
// --beam <blueprint>, --trail <blueprint>  shows that beam (eight units long, above the same spot) or that
//           trail (behind a point circling it); a name such as phalanx_munition_beam_01, or a path
// --turn <bone>  placed units turn that bone (a name such as Turret, or its number) with all that hangs on
//           it, round and round: to look at a model's moving parts without a game (--bones lists the names)
// --timings  at the end of the run, what a frame cost on average, stage by stage: the graphics card's
//           time for each (its own clock) and the processor's for preparing, recording and handing over
// --noicons, --icon-scale <n>  strategic icons: off, or n screen pixels for each of a symbol's (key I switches them)
// --ao      ambient occlusion in the modern look: 0 off to 1 full (key O switches it)
// --msaa    anti-aliasing: samples per pixel of the scene, 1 (off), 2, 4 (the default) or 8
// --detail  the models' quality setting: 1 full, 0.5 for a slower card, a huge value forces the finest level
// --remasters <folder>  the remastered units' files for this engine, one folder per unit as the visuals track
//           lays them out (<folder>\<ID>\export\moho64): each is used in place of that unit's own model, in
//           games too; may be given several times
// --preview <unit id>=<folder>  a remastered model's meshes and LabPBR textures, in place of that unit's
// --place <unit id>  stand that unit at the camera's starting point (to look at a model without a recording)
// --look    modern (the default: physically based lighting) or original (the game's own formulas)
// --inspect <what>  a view for checking art (modern look): every lit surface shows one of its own numbers in
//           place of its lit colour: albedo, normals, roughness, metal, occlusion, shadow, lights (the small
//           lights alone). Key N steps through them.
// --noshadows  start without the sun's shadows
// --motion-view <1|2|3|5>  motion (motion.h), for checking; needs --msaa 1 (5: the temporal upscaler's reactive
//           mask in red, how much a pixel follows its frame alone): 1 shows it as colours, 2 shows the
//           frame before carried to this frame by it (with a still scene it is this frame, but where something
//           comes out from behind another), 3 what that misses of this frame (black where the motion is right)
// --pan <dx,dz>  the camera's point moves that far every frame (world units): a moving camera that repeats
// --clouds-3d  volumetric clouds (clouds.h, World::volumetricClouds; modern look, a time of day): the clouds as
//           real volumes lit by the sun, in place of the sky's own layer (--clouds sets how much of the sky)
// --soft-shadows [size]  soft shadows (World::softShadows; modern look): the sun's shadows blurred the more the
//           farther from what casts them; `size` how much wider per world unit (0.02 at first; a number is optional)
// --ray-reflections  reflections by ray tracing (World::rayReflections; the shaders built for it, gRayShaders; where the
//           device has ray tracing): smooth surfaces mirror what their rays meet
// --lit-rain  rain and snow lit where they fall (World::litRain; modern look): by the sun through its shadow and by the
//           lamps and their beams through their shadows, sent on mostly forward
// --rain-splashes  rain's splashes (World::rainSplashes; modern look): small crowns where rain meets what faces up
// --terrain-despite-hold  the remastered ground (<remasters>/_terrain/export/moho64) mounted even with its HOLD file
//           (to look at it before games may); --ground-ultra  its 8k materials in place of the 4k
// --ground-cover  grass near the camera (World::groundCover; modern look)
// --water-detail  wet shorelines and stronger caustics (World::waterDetail; modern look)
// --bounce  bounce light (bounce.h, World::bounceLight; modern look, where the device has ray tracing): light thrown
//           back by the ground, the hills and what stands on them, from probes the rays update
// --ray-view  ray tracing's check view (ray_scene.h, World::rayView; where the device can): a ray from the camera
//           through each pixel into the ray tracing scene, the sun's shadow by another, in place of the picture
// --sky-tables  the sky from tables of the atmosphere (sky.h, World::skyTables; modern look, a time of day): light
//           scattered many times over and the planet's shadow
// --contact-shadows  contact shadows (World::contactShadows; modern look): small shadows the sun's maps cannot hold
// --air     light in the air (air.h, World::volumetricFog; modern look): the air's own light in a volume over
//           the view, the sun's shafts and the lamps' beams in it. Tuning: --air-dust <thickness per world unit>,
//           --air-sun <strength>, --air-lamps <strength>, --air-cells <across>,<deep>
// --no-lamp-shadows  lamps' beams cast no shadows (they do unless this is given: the eight nearest)
// --no-tracers  plain white shell trails are drawn as the game's files give them, not as hot tracers
// --plain-ground  a remastered map's ground without its layers' third maps (roughness, creases, height)
// --map-ends  the map ends at its edge (with --border: in its frame), as the original; unless given, land
//           and sea go on beyond the edge in the modern look
// --no-frame  the map's frame (--border) is not drawn
// --vulkan  the Vulkan backend (gfx_vulkan.cpp) in place of Direct3D 12 (also for --picture, --window-cycle)
// --fxc  Direct3D 12's shaders compiled by Direct3D's own compiler, not DXC (DeviceDesc::oldShaderCompiler)
// --gfx-check  the graphics layer's compute shaders, storage targets and volumes tried, their results read
//           back and checked (gfx_check.cpp); with --vulkan, the Vulkan backend's
// --fsr3-check  AMD's FSR 3 upscaler (fsr3.h) tried on a made-up scene and checked against the scene drawn at
//           the display's size and against a plain stretch, then timed (fsr3_check.cpp); with --vulkan, on the
//           Vulkan backend; with --screenshot <name>.png, the pictures compared are written as <name>_*.png
// --no-shader-cache  every shader compiled anew, none kept (DeviceDesc::shaderCache "-"), to measure compiling
// --shader-cache <dir>  the compiled shaders kept in that folder (an empty one: to measure a first start)
// --no-prepass  the large models' depth is not laid first (MeshRenderer::depthPrepass, on by default), to measure it
// --no-ground-changes  the ground stays as the map file has it where the game lays it flat under structures (to compare)
// --no-bloom  no glow (World::bloom off: the modern look's glow and the original look's bloom, the game's Bloom Render)
// --fsr3    AMD's FSR 3 (fsr3.h, World::temporalUpscale) in place of FSR 1: the scene drawn a little shifted every
//           frame and brought up to the window's size from the frames before, at --fsr's share (without --fsr:
//           the edges smoothed at the window's own size); sets --msaa 1. --fsr3-bias <n>: the textures' level of
//           detail beyond the share's (World::temporalMipBias; 0 at first; AMD advises -1)
// --fsr off|ultra|quality|balanced|performance  the scene drawn at 1/1.3, 1/1.5, 1/1.7 or 1/2 of the window's
//           width and height and brought up to it by AMD's FSR 1 (upscale.h); or the share itself (0.25..1)
// --effects-quality <share>  the share of their particles effects throw out (1: all; 0.5 for a slower card)
// --shadow-size <texels>  the sun's shadow maps' width and height (2048; 4096 sharper, 1024 and 512 cheaper)
// --texture-detail 0|1|2  textures larger than 256 without their largest size (1) or two (2): for cards
//           with less memory (0: all, as at first)
// --fx <template or emitters>  the effects test stand (fx_stand.h): an effect template of the game's
//           /lua/EffectTemplates.lua (TGaussCannonFlash) or emitters with commas between, played at --fx-at
//           x,z[,height] (else where the camera looks) every --fx-repeat seconds (2)
// --fx-shot <projectile>  fires that projectile (an id such as TDFGauss01, or its blueprint) from --fx-from
//           x,z,height to --fx-to x,z[,height] (else across where the camera looks, away from it), with the
//           trails and the hit its script class gives it; --fx-hit land|unit|water|underwater|prop|shield|air|none
// --fx-compare  the remastered effects on the left, the original's on the right (--fx-spacing apart, 8)
// --fx-entity <effect entity>  an effect entity (UEFNukeEffectController01, or its blueprint's path) made at
//           --fx-at as a nuke's impact makes it, its own scripts run (fx_entities.h), every --fx-repeat seconds
//           (25 unless given); --fx-seed n: where its Random starts (1). A --fx-shot whose impact makes one
//           (a nuke) makes it where it lands.
// --fx-shield <unit>  that unit (UEB4301) at --fx-at with its shield up, hit from random directions above the
//           ground every --fx-hit-every seconds (0.8) with its own flash and hit effects, as the game's shield
//           script makes them; made again every --fx-repeat seconds (20 unless given)
//           (with --frames n --shots k --screenshot <name>.png: a numbered picture every k frames)
// --fx-death <unit>  that unit (UEL0201) made at --fx-at by its own script class and killed by its own death
//           path --fx-kill-at seconds (1) into each play, every --fx-repeat seconds (12 unless given):
//           its explosion, debris and wreck as the game's scripts make them, an aircraft falling (it
//           flies in at its elevation and comes down at the spot), a ship sinking on water; --fx-overkill
//           r: the overkill ratio it dies with (0.2; above 1 it leaves no wreck)
// --fx-faction uef|aeon|cybran|seraphim  the stand's effects are that faction's own in the remastered
//           look (else the faction of what they are made on: a shot's by its id, a unit's)
// --fx-move <unit>  that unit (UEL0203) made at --fx-at by its own script class and driven round a loop there
//           at its blueprint's speed (--fx-laps n, 1), its movement effects as its scripts make them: over
//           the map's own terrain types, from land onto water for a hover unit (put --fx-at on a shore
//           with the land toward the camera: the loop begins at its end nearest the camera),
//           an aircraft hovering at its elevation first; the play's length is worked out (else --fx-repeat);
//           the camera follows it (unless --cam)
// --fx-build <builder> <unit>  (or <builder>,<unit>) the builder building that unit at --fx-at as the game's
//           scripts make it: a factory (UEB0101 UEL0201) on its build bone, an engineer (UEL0105 UEB1101) from
//           beside it; --fx-build-seconds s: the build takes that long (else the game's own time)
// --fx-fire <unit> [label ...]  that unit (UEL0201; a mod's too, with --mods) made at --fx-at by its own script class
//           with its weapons as the engine makes them, firing at the point --fx-to x,z[,height] (else one in its
//           weapons' reach, to the right of the spot as the camera sees it; with --fx-compare across it, away from
//           the camera): --fx-fire-at s (1) into each play the weapons named (their blueprints' Label, with commas
//           or as the words after the unit; none: every weapon that shoots at the ground and is no death weapon)
//           get the point as their target, their turrets turn to it and they fire as their own scripts and the
//           engine's fire clock let them (shots with their trails and hits, beams with their emitters);
//           --fx-hold-at s (two seconds before the play's end) they, and the --fx-manual ones, lose it again (a gun
//           that unpacked to fire packs up then, as when the unit is given another order); made again every --fx-repeat
//           seconds (8 unless given). An aircraft hovers at its elevation. It faces the point when it is an
//           aircraft or one of those weapons cannot turn all round, else its back is to the camera (side by side:
//           its side), so that its turrets are seen turning.
// --fx-manual <label>  with --fx-fire: that manual-fire weapon (a tactical or strategic missile, a commander's
//           overcharge, a scripted main gun) fired at the point once each play as a player's order fires it,
//           through the weapon's own script (a play is 14 s unless --fx-repeat); may be given several times
// --effects-look original|remastered  the game's effects as the original has them, or the visuals track's
//           remastered files where there are some (--remasters <folder>/_effects/export/moho64; the default)
// --no-beyond-haze  the land beyond the map's edge is not lost in haze but at the far end
// --sun, --sky, --exposure, --reflection  the modern look's strengths (for tuning it by eye)
// --time-of-day <hours>  the computed sky of that hour (0..24) lights the modern look instead of the map's
//           own light; --day-length <seconds> lets the day pass, a whole one in that long. Key T: an hour on
//           (from noon when the map's light was on), Y: the map's own light again.
// --border <mesh blueprint>  what the map lies in, as the game's MapBorderAdd names it (may be given twice),
//           for example /meshes/game/map-border_squ_uef_mesh
// --focus <army>  with --sim or --session: the picture is for that army (from 1): other armies' units show
//           only as it knows them (seen, a blip on radar, or not at all)
// --playable x0,z0,x1,z1  only that part of the map can be played (World::playable, the campaign's
//           SetPlayableArea): outside it black (the original's), or dimmed in the modern look with the world
//           beyond the map's edge; nothing of the game shown there
// --ranges  test range rings round where the camera starts: a red group that glows (three rings that join, one
//           of them with a hole, and one apart) and a blue one
// --unexplored  with --focus: ground that army has never seen is all but black (the game's option
//           FogOfWar "unexplored")
// Key B: life bars over units on and off.
// --clouds <0..1>  with a time of day: how much of the sky clouds cover (0.3 unless given); key C steps through
// --mist <0..1>  with a time of day: how much mist lies on low ground and over water (unless given: by the
//           hour and the weather, most at daybreak)
// --wet <0..1>  the ground starts that wet (World::wet; then it follows the weather): puddles at once
// --weather <clear|rain|storm|snow|fog>  the weather (it turns a time of day on, noon unless given); key R
//           steps through them. Given as an option, surfaces are wet or snowy from the start.
// --find shield|wreck|<unit id>  with a game: once it is where --tick asks, the camera goes to the first shield's bubble,
//           the first wreck or the first unit of that kind there is (to look at them without knowing where they are)
// --hide-bone <bone>  placed units are drawn with that bone and all that hangs on it hidden (as a script's
//           HideBone does; a name or a number, as for --turn)
// --soften <n>  over how many world units particles fade where they meet something solid (0.6; next to 0: a hard cut)
// --shielded  placed units are drawn with a personal shield up
// --built <0..1>  placed units are that far under construction (to look at the factions' build looks)
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
#include <algorithm>
#include <map>
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
#include "cpu_times.h"
#include "fx_stand.h"
#include "camera.h"
#include "core/hostfs.h"
#include "remasters.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/mount.h"
#include "debug_draw.h"
#include "decals.h"
#include "effects.h"
#include "frame.h"
#include "gfx.h"
#include "gfx_check.h"
#include "fsr3_check.h"
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
  bool lampShadows = true;              // --no-lamp-shadows
  bool tracers = true;                  // --no-tracers
  bool richGround = true;               // --plain-ground
  bool worldBeyond = true;              // --map-ends
  bool timings = false;                 // --timings
  FxStandSetup fx;                      // --fx, --fx-shot and their places
  bool fxAt = false, fxFrom = false, fxTo = false, fxRepeat = false;
  bool remasteredEffects = true;        // --effects-look original|remastered
  bool prepass = true;                  // --no-prepass
  bool groundChanges = true;            // --no-ground-changes
  bool bloom = true;                    // --no-bloom
  bool vulkan = false;                  // --vulkan
  bool fxc = false;                     // --fxc
  bool shaderCache = true;              // --no-shader-cache
  std::string shaderCacheDir;           // --shader-cache
  bool vulkanClear = false;             // --vulkan-clear
  bool gfxCheck = false;                // --gfx-check
  bool fsr3Check = false;               // --fsr3-check
  bool air = false;                     // --air
  bool softShadows = false;             // --soft-shadows
  bool contactShadows = false;          // --contact-shadows
  bool skyTables = false;               // --sky-tables
  bool rayView = false;                 // --ray-view
  bool bounce = false;                  // --bounce
  bool rayReflections = false;          // --ray-reflections
  bool litRain = false;                 // --lit-rain
  bool rainSplashes = false;            // --rain-splashes
  bool waterDetail = false;             // --water-detail
  bool groundCover = false;             // --ground-cover
  bool terrainDespiteHold = false;      // --terrain-despite-hold
  bool groundUltra = false;             // --ground-ultra
  float sunSize = -1;                   // (its size, when given)
  bool clouds3d = false;                // --clouds-3d
  bool cloudsFull = false;              // --clouds-full (the same, marched at the picture's own size)
  int motionView = 0;                   // --motion-view
  float pan[2] = {0, 0};                // --pan
  float airDust = -1, airSun = -1, airLamps = -1;  // --air-dust, --air-sun, --air-lamps (negative: the default)
  int airAcross = 0, airDeep = 0;       // --air-cells
  bool pictureWindow = false;           // --picture-window
  bool windowCycle = false;             // --window-cycle
  float effectsQuality = 1;             // --effects-quality
  float upscale = 1;                    // --fsr
  float wet = 0;                        // --wet
  bool fsr3 = false;                    // --fsr3
  float fsr3Bias = 0;                   // --fsr3-bias
  int shadowSize = 2048;                // --shadow-size
  int textureDetail = 0;                // --texture-detail
  bool frame = true;                    // --no-frame
  bool beyondHaze = true;               // --no-beyond-haze
  std::string inspect;                  // --inspect
  float iconScale = 0;  // screen pixels for each pixel of a strategic icon; 0: by the picture's height
  Look look = Look::Modern;
  int frames = 0;
  float lodPixels = 0;
  // the modern look's strengths, found by eye on the game's maps (the maps' own light values were
  // chosen for the original's formula)
  float sun = 1.4f, sky = 1.1f, exposure = 0.55f, reflection = 0.7f;
  float timeOfDay = -1, dayLength = 0;  // --time-of-day, --day-length
  float clouds = 0.3f;                  // --clouds
  float mist = -1;                      // --mist
  int focus = 0;                        // --focus
  float playable[4] = {0, 0, 0, 0};     // --playable
  bool unexplored = false;              // --unexplored
  bool ranges = false;                  // --ranges
  bool shielded = false;                // --shielded
  bool hideTurned = false;              // --hide-bone
  float snowLying = -1;                 // --snow-lying
  float built = 1;                      // --built
  std::string weather, climate;         // --weather, --climate
  std::string find;                     // --find
  bool show = false;                    // --show
  bool bars = false;                    // --bars
  float shotLight = 1;                  // --shot-light
  int shots = 0;                        // --shots
  std::string animate;                  // --animate: an .sca file of the game's
  float animateAt = -1;                 // ... and where in it (0..1); below 0: it plays
  int selfDestructAt = 0, selfDestructArmy = 1;  // --self-destruct
  int enhanceAt = 0, enhanceArmy = 1;            // --enhance
  int pauseAt = 0;                               // --pause-at
  std::string enhancement;
  float soften = -1;                    // --soften
  std::vector<std::string> border;      // --border
  float weatherSpell = 300;             // --weather-spell
  float heading = 25, pitch = 32;  // degrees: from where placed units are looked at when no --cam is given
  float at[2] = {};                // --at x,z: where placed units stand (default: the camera's starting target)
  bool hasAt = false;
  double tick = -1;
  std::string screenshot, fa, map, recording, createLog, display;
  std::string init, sim, session, modUids, simLog;  // the simulation run in the viewer (--init, --sim or --session, ...)
  unsigned seed = 0;
  std::vector<std::string> data, mods, maps, previews, remasters, place, crowd;
  std::string turn;    // --turn: the bone placed units turn
  std::vector<std::string> emit;  // --emit: emitters played over and over
  float emitScale = 1;
  std::vector<std::string> beam, trail;  // --beam, --trail: shown for a look at them
  std::string hide;    // --hide: kinds of effect left out
  std::string picture; // --picture
  std::string settings; // --settings
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
    else if (a == "--show") o->show = true;
    else if (a == "--bars") o->bars = true;  // life bars on from the start (key B switches them)
    else if (a == "--paused") o->paused = true;
    else if (a == "--noshadows") o->shadows = false;
    else if (a == "--no-lamp-shadows") o->lampShadows = false;
    else if (a == "--no-tracers") o->tracers = false;
    else if (a == "--plain-ground") o->richGround = false;
    else if (a == "--map-ends") o->worldBeyond = false;
    else if (a == "--timings") o->timings = true;
    else if (a == "--fx-compare") o->fx.compare = true;
    else if (a == "--vulkan") o->vulkan = true;
    else if (a == "--fxc") o->fxc = true;
    else if (a == "--no-shader-cache") o->shaderCache = false;
    else if (a == "--vulkan-clear") o->vulkan = o->vulkanClear = true;
    else if (a == "--gfx-check") o->gfxCheck = true;
    else if (a == "--fsr3-check") o->fsr3Check = true;
    else if (a == "--fsr3") o->fsr3 = true;
    else if (a == "--air") o->air = true;
    else if (a == "--contact-shadows") o->contactShadows = true;
    else if (a == "--sky-tables") o->skyTables = true;
    else if (a == "--ray-view") o->rayView = true;
    else if (a == "--bounce") o->bounce = true;
    else if (a == "--ray-reflections") o->rayReflections = true;
    else if (a == "--lit-rain") o->litRain = true;
    else if (a == "--rain-splashes") o->rainSplashes = true;
    else if (a == "--water-detail") o->waterDetail = true;
    else if (a == "--ground-cover") o->groundCover = true;
    else if (a == "--terrain-despite-hold") o->terrainDespiteHold = true;
    else if (a == "--ground-ultra") o->groundUltra = true;
    else if (a == "--soft-shadows") {
      o->softShadows = true;
      if (i + 1 < argc && (std::isdigit(static_cast<unsigned char>(argv[i + 1][0])) || argv[i + 1][0] == '.')) o->sunSize = static_cast<float>(atof(argv[++i]));
    }
    else if (a == "--clouds-3d") o->clouds3d = true;
    else if (a == "--clouds-full") o->clouds3d = o->cloudsFull = true;
    else if (a == "--picture-window") o->pictureWindow = true;
    else if (a == "--window-cycle") o->windowCycle = true;
    else if (a == "--prepass") o->prepass = true;
    else if (a == "--no-prepass") o->prepass = false;
    else if (a == "--no-ground-changes") o->groundChanges = false;
    else if (a == "--no-bloom") o->bloom = false;
    else if (a == "--no-frame") o->frame = false;
    else if (a == "--no-beyond-haze") o->beyondHaze = false;
    else if (a == "--noicons") o->icons = false;
    else if (a == "--unexplored") o->unexplored = true;
    else if (a == "--ranges") o->ranges = true;
    else if (a == "--shielded") o->shielded = true;
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
    else if (a == "--remasters") o->remasters.push_back(v);
    else if (a == "--place") o->place.push_back(v);
    else if (a == "--crowd") o->crowd.push_back(v);
    else if (a == "--turn") o->turn = v;
    else if (a == "--hide-bone") {
      o->turn = v;
      o->hideTurned = true;
    }
    else if (a == "--emit") o->emit.push_back(v);
    else if (a == "--beam") o->beam.push_back(v);
    else if (a == "--hide") o->hide = v;
    else if (a == "--picture") o->picture = v;
    else if (a == "--settings") o->settings = v;
    else if (a == "--motion-view") o->motionView = atoi(v);
    else if (a == "--pan") {
      if (sscanf(v, "%f,%f", &o->pan[0], &o->pan[1]) != 2) return false;
    }
    else if (a == "--air-dust") o->airDust = static_cast<float>(atof(v));
    else if (a == "--air-sun") o->airSun = static_cast<float>(atof(v));
    else if (a == "--air-lamps") o->airLamps = static_cast<float>(atof(v));
    else if (a == "--air-cells") {
      if (sscanf(v, "%d,%d", &o->airAcross, &o->airDeep) != 2) return false;
    }
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
    else if (a == "--effects-look") o->remasteredEffects = std::string(v) != "original";
    else if (a == "--fx") o->fx.fx = v;
    else if (a == "--effects-quality") o->effectsQuality = static_cast<float>(atof(v));
    else if (a == "--shadow-size") o->shadowSize = atoi(v);
    else if (a == "--texture-detail") o->textureDetail = std::clamp(atoi(v), 0, 2);
    else if (a == "--fsr3-bias") o->fsr3Bias = static_cast<float>(atof(v));
    else if (a == "--fsr") {  // FSR 1's modes, or the share itself
      const std::string m = v;
      o->upscale = m == "off" ? 1.0f : m == "ultra" ? 1 / 1.3f : m == "quality" ? 1 / 1.5f : m == "balanced" ? 1 / 1.7f
                 : m == "performance" ? 0.5f : static_cast<float>(atof(v));
      if (!(o->upscale > 0)) o->upscale = 1;
    }
    else if (a == "--fx-shot") o->fx.shot = v;
    else if (a == "--fx-hit") o->fx.hit = v;
    else if (a == "--fx-repeat") {
      o->fx.repeat = std::max(0.2f, static_cast<float>(atof(v)));
      o->fxRepeat = o->fx.repeatGiven = true;
    } else if (a == "--fx-entity") o->fx.entity = v;
    else if (a == "--fx-shield") o->fx.shield = v;
    else if (a == "--fx-death") o->fx.death = v;
    else if (a == "--fx-kill-at") o->fx.killAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-overkill") o->fx.overkill = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-faction") o->fx.faction = v;
    else if (a == "--fx-move") o->fx.move = v;
    else if (a == "--fx-laps") o->fx.laps = std::max(1, atoi(v));
    else if (a == "--fx-build") {  // <builder> <unit>, or <builder>,<unit>
      std::string both = v;
      const size_t comma = both.find(',');
      if (comma != std::string::npos) {
        o->fx.build = both.substr(0, comma);
        o->fx.buildUnit = both.substr(comma + 1);
      } else {
        const char* w = value();
        if (!w) return false;
        o->fx.build = both;
        o->fx.buildUnit = w;
      }
    } else if (a == "--fx-build-seconds") o->fx.buildSeconds = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-fire" || a == "--fx-manual") {  // --fx-fire <unit>[,label...] [label ...]; --fx-manual <label>[,label...]
      // (the labels: given with commas, and for --fx-fire also as the words that follow it, up to the next option)
      std::vector<std::string> words;
      auto split = [&words](const std::string& all) {
        for (size_t at = 0; at <= all.size();) {
          const size_t comma = all.find(',', at);
          const std::string w = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
          if (!w.empty()) words.push_back(w);
          if (comma == std::string::npos) break;
          at = comma + 1;
        }
      };
      split(v);
      if (a == "--fx-fire") {
        while (i + 1 < argc && argv[i + 1][0] != '-') split(argv[++i]);
        if (words.empty()) return false;
        o->fx.fire = words[0];
        o->fx.fireWeapons.insert(o->fx.fireWeapons.end(), words.begin() + 1, words.end());
      } else {
        o->fx.manual.insert(o->fx.manual.end(), words.begin(), words.end());
      }
    }
    else if (a == "--fx-fire-at") o->fx.fireAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-hold-at") o->fx.holdAt = std::max(0.0f, static_cast<float>(atof(v)));
    else if (a == "--fx-hit-every") o->fx.hitEvery = std::max(0.05f, static_cast<float>(atof(v)));
    else if (a == "--fx-seed") o->fx.seed = static_cast<uint32_t>(std::max(1, atoi(v)));
    else if (a == "--fx-spacing") {
      o->fx.spacing = static_cast<float>(atof(v));
      o->fx.spacingGiven = true;
    }
    else if (a == "--fx-at" || a == "--fx-from" || a == "--fx-to") {  // x,z[,height above the ground]
      float x = 0, z = 0, h = 0;
      if (sscanf(v, "%f,%f,%f", &x, &z, &h) < 2) return false;
      (a == "--fx-at" ? o->fx.at : a == "--fx-from" ? o->fx.from : o->fx.to) = Vec3{x, h, z};
      (a == "--fx-at" ? o->fxAt : a == "--fx-from" ? o->fxFrom : o->fxTo) = true;
    }
    else if (a == "--inspect") o->inspect = v;
    else if (a == "--mist") o->mist = static_cast<float>(atof(v));
    else if (a == "--focus") o->focus = atoi(v);
    else if (a == "--weather") o->weather = v;
    else if (a == "--wet") o->wet = static_cast<float>(atof(v));
    else if (a == "--shader-cache") o->shaderCacheDir = v;
    else if (a == "--find") o->find = v;
    else if (a == "--shots") o->shots = atoi(v);
    else if (a == "--shot-light") o->shotLight = static_cast<float>(atof(v));  // how strong the light of shots in flight is (0: none; below 0: on and off by turns of 30 frames, to compare)
    else if (a == "--animate") {  // <file.sca>[,<fraction>]
      o->animate = v;
      const size_t comma = o->animate.rfind(',');
      if (comma != std::string::npos) {
        o->animateAt = static_cast<float>(atof(o->animate.c_str() + comma + 1));
        o->animate.resize(comma);
      }
    }
    else if (a == "--pause-at") o->pauseAt = atoi(v);  // <beat>: the commander of --enhance's army is paused then
    else if (a == "--enhance") {  // <beat>,<army>,<enhancement>: that army's commander is ordered to build it then
      char name[128] = "";
      if (sscanf(v, "%d,%d,%127s", &o->enhanceAt, &o->enhanceArmy, name) != 3) return false;
      o->enhancement = name;
    } else if (a == "--self-destruct") {  // <beat>[,<army>]: that army's commander destroys itself then (a new game)
      if (sscanf(v, "%d,%d", &o->selfDestructAt, &o->selfDestructArmy) < 1) return false;
    }
    else if (a == "--soften") o->soften = static_cast<float>(atof(v));
    else if (a == "--snow-lying") o->snowLying = static_cast<float>(atof(v));
    else if (a == "--built") o->built = static_cast<float>(atof(v));
    else if (a == "--border") o->border.push_back(v);
    else if (a == "--climate") o->climate = v;
    else if (a == "--weather-spell") o->weatherSpell = static_cast<float>(atof(v));
    else if (a == "--reflection") o->reflection = static_cast<float>(atof(v));
    else if (a == "--heading") o->heading = static_cast<float>(atof(v));
    else if (a == "--pitch") o->pitch = static_cast<float>(atof(v));
    else if (a == "--playable") {
      if (sscanf(v, "%f,%f,%f,%f", &o->playable[0], &o->playable[1], &o->playable[2], &o->playable[3]) != 4) return false;
    } else if (a == "--at") {
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
  // --remasters <folder>: every remastered unit's files for this engine found there (remasters.h)
  for (const std::string& root : o.remasters) printf("render: %d remastered units from %s" "%c", MountRemasters(vfs, root, !o.fx.fx.empty() || !o.fx.shot.empty() || !o.fx.entity.empty() || !o.fx.shield.empty() || !o.fx.death.empty() || !o.fx.move.empty() || !o.fx.build.empty() || !o.fx.fire.empty(), o.terrainDespiteHold), root.c_str(), 10);
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
  gSceneSamples = opt.fsr3 ? 1 : opt.msaa == 2 || opt.msaa == 4 || opt.msaa == 8 ? opt.msaa : 1;  // (FSR 3: one sample a pixel)
  std::string error;
  // an automatic run (--frames) must give the same picture whatever the person at the machine is
  // doing: its window stays in the background and what is typed or clicked is not acted on
  const bool automatic = opt.frames > 0;
  // (and a run that only writes a picture shows no window at all: it would lie over whatever the
  // person is doing, a game of their own included; --show brings it back)
  auto window = Window::Create("FA Remastered", opt.width, opt.height, &error, automatic, automatic && !opt.screenshot.empty() && !opt.show);
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
  dd.backend = opt.vulkan ? gfx::DeviceDesc::Backend::Vulkan : gfx::DeviceDesc::Backend::Direct3D12;
  dd.oldShaderCompiler = opt.fxc;
  if (!opt.shaderCache) dd.shaderCache = "-";
  else if (!opt.shaderCacheDir.empty()) dd.shaderCache = opt.shaderCacheDir;
  auto device = gfx::Device::Create(dd, &error);
  if (device && opt.vulkanClear) {  // --vulkan-clear: the backend's first step alone: frames cleared, shown and read back
    printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());
    std::vector<uint8_t> shot;
    for (int i = 0; i < std::max(opt.frames, 3); ++i) {
      window->Pump(nullptr);
      const float c[4] = {0.2f + 0.1f * static_cast<float>(i % 3), 0.4f, 0.7f, 1};
      device->BeginFrame(c);
      device->EndFrame(i + 1 == std::max(opt.frames, 3) ? &shot : nullptr);
    }
    if (!opt.screenshot.empty() && !shot.empty())
      printf("render: --vulkan-clear: %s\n", WritePng(opt.screenshot, shot.data(), device->Width(), device->Height()) ? opt.screenshot.c_str() : "could not be written");
    return 0;
  }
  if (!device) {
    fprintf(stderr, "render: no graphics device: %s\n", error.c_str());
    return 1;
  }
  printf("render: %s on %s, %dx%d\n", device->BackendName(), device->AdapterName().c_str(), device->Width(), device->Height());
  if (opt.gfxCheck) return GfxCheck(device.get(), [&] { window->Pump(nullptr); }) ? 0 : 1;
  if (opt.fsr3Check) return Fsr3Check(device.get(), [&] { window->Pump(nullptr); }, opt.screenshot) ? 0 : 1;

  moho::LogSetEcho(false);  // the engine's file layer reports every archive it opens
  moho::Vfs vfs;
  MountGameFiles(opt, &vfs);
  if (vfs.MountCount()) printf("render: %zu archives and folders, %zu files\n", vfs.MountCount(), vfs.FileCount());
  // --sim: the simulation itself plays the replay, on its own thread; it starts loading at once
  if (opt.windowCycle) {  // --window-cycle
    std::string error;
    auto window = Window::Create("FA Remastered (window cycle)", opt.width, opt.height, &error, true, false);
    if (!window) {
      fprintf(stderr, "render: --window-cycle: %s\n", error.c_str());
      return 1;
    }
    const int w = window->Width(), h = window->Height();
    std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    for (int y = 40; y < 140; ++y)
      for (int x = 40; x < 340; ++x) layer[static_cast<size_t>(y) * w + x] = 0xffff2020u;  // (a red box: the menu's "interface")
    const std::string base = opt.screenshot.empty() ? std::string("window_cycle") : opt.screenshot.substr(0, opt.screenshot.rfind('.'));
    Camera cam;
    cam.target = {opt.hasAt ? opt.at[0] : 512.0f, 0, opt.hasAt ? opt.at[1] : 512.0f};
    cam.distance = 120;
    cam.pitch = 0.9f;
    auto shoot = [&](const char* what, const std::string& map) {
      WorldPicture p;
      p.SetVulkan(opt.vulkan);
      p.UseWindow(window->Handle());
      std::string why;
      if (!p.Open(&vfs, map, &why)) {
        printf("render: --window-cycle: %s could not open: %s\n", what, why.c_str());
        return;
      }
      std::vector<uint8_t> shown;
      for (int i = 0; i < 3; ++i) {
        window->Pump(nullptr);
        p.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, i == 0 ? w : 0, i == 0 ? h : 0);
        if (i == 2) p.CaptureNextWindowFrame(&shown);
        p.DrawToWindow(cam, w, h, 0);
      }
      const std::string file = base + "_" + what + ".png";
      printf("render: --window-cycle: %s %s\n", what, shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(file, shown.data(), w, h) ? file.c_str() : "showed nothing");
    };
    shoot("menu1", "");
    shoot("game", opt.map);
    shoot("menu2", "");
    return 0;
  }
  WorldPicture picture;
  std::unique_ptr<Window> pictureWindow;  // --picture-window
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
    setup.selfDestructAt = static_cast<uint32_t>(std::max(opt.selfDestructAt, 0));
    setup.selfDestructArmy = opt.selfDestructArmy;
    setup.enhanceAt = static_cast<uint32_t>(std::max(opt.enhanceAt, 0));
    setup.enhanceArmy = opt.enhanceArmy;
    setup.enhancement = opt.enhancement;
    setup.pauseAt = static_cast<uint32_t>(std::max(opt.pauseAt, 0));
    live.SetFocusArmy(opt.focus);
    picture.SetVulkan(opt.vulkan);
    picture.SetFocusArmy(opt.focus);
    picture.SetUpscale(opt.upscale);        // (--fsr)
    picture.SetTemporalUpscale(opt.fsr3);   // (--fsr3)
    picture.SetRayReflections(opt.rayReflections);  // (--ray-reflections)
    if (opt.timeOfDay >= 0) picture.SetDayAndWeather(opt.timeOfDay, opt.dayLength, opt.weather == "auto");  // (--picture: the hour and the weather through the pixel interface)
    setup.modUids = opt.modUids;
    if (!live.Open(setup, &error)) {
      fprintf(stderr, "render: simulation: %s\n", error.c_str());
      return 1;
    }
    if (opt.map.empty()) opt.map = live.MapPath();
    if (!opt.picture.empty()) {  // the pixel interface, fed by the same simulation
      if (opt.pictureWindow) {  // (into a window of its own, as the interface's would be)
        pictureWindow = Window::Create("FA Remastered (picture into a window)", opt.width, opt.height, &error, true, false);
        if (pictureWindow) picture.UseWindow(pictureWindow->Handle());
      }
      if (!opt.settings.empty()) {  // (the game's lighting settings, before Open as the interface sets them)
        const std::string s = "," + opt.settings + ",";
        picture.SetAtmosphere(s.find(",atmosphere,") != std::string::npos);
        picture.SetSoftShadows(s.find(",soft-shadows,") != std::string::npos);
        picture.SetRayTracing(s.find(",ray-tracing,") != std::string::npos);
        picture.SetWeatherEffects(s.find(",weather-effects,") != std::string::npos);
        picture.SetGroundDetail(s.find(",ground-detail,") != std::string::npos);
      }
      if (!picture.Open(&vfs, opt.map, &error)) {
        fprintf(stderr, "render: --picture: %s\n", error.c_str());
        return 1;
      }
      printf("render: --picture: ray tracing %s\n", picture.RayTracingSupported() ? "supported" : "not supported");
      live.onStarted = [&](const moho::Sim& sim) { picture.GameStarted(sim); };
      live.onBeat = [&](const moho::Sim& sim) { picture.Beat(sim); };
    }
    printf("render: simulation: %s %s on %s, loading ...\n", opt.session.empty() ? "the replay" : "a new game from",
           (opt.session.empty() ? opt.sim : opt.session).c_str(), live.MapPath().c_str());
    live.Start();
  }

  // the world's picture: the map and everything drawn on it (with --ray-reflections, its shaders built to trace rays)
  gRayShaders = opt.rayReflections && device->RayTracing();
  World world(device.get(), &vfs);
  world.terrain.ultraGround = opt.groundUltra;  // (the remastered ground's set: before the map opens)
  {
    World::Options wo;
    wo.displayTable = opt.display;
    wo.lodPixels = opt.lodPixels;
    wo.mapDecals = opt.mapDecals;
    wo.decalFilter = opt.decalFilter;
    wo.decalTurn = opt.decalTurn;
    wo.decalRange = opt.decalRange;
    wo.verbose = opt.debug;
    world.textures.detail = opt.textureDetail;
    world.bloom = opt.bloom;
    if (!world.Open(opt.map, wo, &error)) {
      fprintf(stderr, "render: %s (is --fa or --data given?)\n", error.c_str());
      return 1;
    }
  }
  world.look = opt.look;
  world.shadowsOn = opt.shadows;
  world.lampShadows = opt.lampShadows;
  world.volumetricFog = opt.air;
  world.softShadows = opt.softShadows;
  world.contactShadows = opt.contactShadows;
  world.skyTables = opt.skyTables;
  world.rayView = opt.rayView;
  world.bounceLight = opt.bounce;
  world.rayReflections = opt.rayReflections;
  world.litRain = opt.litRain;
  world.rainSplashes = opt.rainSplashes;
  world.waterDetail = opt.waterDetail;
  world.groundCover = opt.groundCover;
  if (opt.sunSize > 0) world.sunSize = opt.sunSize;
  world.volumetricClouds = opt.clouds3d;
  if (opt.cloudsFull) world.clouds.shrink = 1;
  world.motionView = opt.motionView;
  if (opt.airDust >= 0) world.air.dust = opt.airDust;
  if (opt.airSun >= 0) world.air.sunInAir = opt.airSun;
  if (opt.airLamps >= 0) world.air.lampsInAir = opt.airLamps;
  if (opt.airAcross > 0) world.air.across = opt.airAcross;
  if (opt.airDeep > 0) world.air.deep = opt.airDeep;
  world.ribbons.tracers = opt.tracers;
  world.terrain.richGround = opt.richGround;
  world.worldBeyond = opt.worldBeyond;
  world.effectsLook = opt.remasteredEffects ? World::EffectsLook::Remastered : World::EffectsLook::Original;
  world.effectsQuality = opt.effectsQuality;
  world.upscale = opt.upscale;
  world.temporalUpscale = opt.fsr3;
  world.temporalMipBias = opt.fsr3Bias;
  world.shadowSize = opt.shadowSize;
  world.meshes.depthPrepass = opt.prepass;
  world.frame = opt.frame;
  world.beyondHaze = opt.beyondHaze;
  static const char* const kInspect[] = {"", "albedo", "normals", "roughness", "metal", "occlusion", "shadow", "lights", "cascades", "detail", "ambient"};
  for (int i = 1; i < 11; ++i)
    if (opt.inspect == kInspect[i]) world.inspect = i;
  if (!opt.inspect.empty() && !world.inspect) fprintf(stderr, "render: --inspect %s: expected albedo, normals, roughness, metal, occlusion, shadow or lights\n", opt.inspect.c_str());
  world.iconsOn = opt.icons;
  std::copy(opt.playable, opt.playable + 4, world.playable);
  world.sun = opt.sun;
  world.sky = opt.sky;
  world.exposure = opt.exposure;
  world.timeOfDay = opt.timeOfDay;
  world.wet = opt.wet;
  world.cloudCover = opt.clouds;
  world.mist = opt.mist;
  world.borders = opt.border;
  world.unexplored = opt.unexplored;
  if (opt.soften >= 0) world.particles.soften = opt.soften;
  if (opt.ranges) {  // range rings to look at, round where the camera starts
    const Vec3 at{opt.cam[0], 0, opt.cam[1]};
    RangeGroup fire, radar;
    fire.color = 0xffff3020u;
    fire.glow = 160;
    fire.rings = {{at + Vec3{-14, 0, 0}, 22, 0}, {at + Vec3{12, 0, 6}, 22, 8}, {at + Vec3{2, 0, -4}, 9, 0}, {at + Vec3{40, 0, -30}, 15, 0}};
    radar.color = 0xff20e0ffu;
    radar.rings = {{at + Vec3{0, 0, 10}, 60, 0}, {at + Vec3{70, 0, 20}, 45, 0}};
    // (and two more groups lying exactly on one of the fire rings, drawn before it: the later line
    // must take their place whole, with no second colour beside it)
    RangeGroup yellow, cyan;
    yellow.color = 0xffffff00u;
    yellow.rings = {{at + Vec3{40, 0, -30}, 15, 0}};
    cyan.color = 0xff00ffffu;
    cyan.rings = {{at + Vec3{40, 0, -30}, 15, 0}};
    world.ranges.groups = {yellow, cyan, fire, radar};
    // ... and the meshes the game's script places: a rally point's flag, and an order's mark half way through its life
    PlacedMesh flag, mark;
    flag.mesh = "/meshes/game/flag02d_lod0.scm";
    flag.texture = "/meshes/game/flag02d_albedo.dds";
    flag.shader = "RallyPoint";
    flag.uniformScale = 0.5f;
    flag.position = at + Vec3{6, world.HaveMap() ? world.GroundHeight(at.x + 6, at.z + 10) : 0, 10};
    mark.mesh = "/meshes/game/crosshair02d_lod0.scm";
    mark.texture = "/meshes/game/crosshair02d_albedo.dds";
    mark.shader = "CommandFeedback";
    mark.uniformScale = 0.5f;
    mark.position = at + Vec3{-8, world.HaveMap() ? world.GroundHeight(at.x - 8, at.z + 14) : 0, 14};
    mark.lifetime = 0.7f;
    mark.age = 0.3f;
    world.placed = {flag, mark};
    // ... the UEF's box of light round what it builds, and the dots the cursor's script stands in a column
    PlacedMesh cube;
    cube.mesh = "/meshes/generic/cube01_lod0.scm";
    cube.texture = "/meshes/generic/cube03_albedo.dds";
    cube.shader = "UEFBuildCube";
    cube.position = at + Vec3{14, (world.HaveMap() ? world.GroundHeight(at.x + 14, at.z + 12) : 0) + 1, 12};
    cube.scale = {3, 2, 3};
    world.placed.push_back(cube);
    for (int k = 0; k < 4; ++k) {
      PlacedMesh dot;
      dot.mesh = "/env/common/props/sphere_lod0.scm";
      dot.texture = "/meshes/game/Assist_albedo.dds";
      dot.shader = "FakeRings";
      dot.uniformScale = 0.15f;
      dot.position = at + Vec3{0, (world.HaveMap() ? world.GroundHeight(at.x, at.z + 14) : 0) + 0.5f + static_cast<float>(k), 14};
      dot.fractionComplete = 0.8f;
      world.placed.push_back(dot);
    }
    // ... a pane of glass (the dome of the Aeon's torpedo launcher), with a cube behind it to see through to
    PlacedMesh dome, behind;
    dome.mesh = "/effects/entities/uab2205_dome/uab2205_dome_lod0.scm";
    dome.texture = "/effects/entities/uab2205_dome/uab2205_dome_albedo.dds";
    dome.shader = "GlassAlpha";
    dome.uniformScale = 1.0f;
    dome.position = at + Vec3{-2, (world.HaveMap() ? world.GroundHeight(at.x - 2, at.z + 6) : 0) + 0.3f, 6};
    behind = cube;
    behind.shader = "Unit";
    behind.position = dome.position + Vec3{0, 0.2f, -1.5f};
    behind.scale = {0.6f, 0.6f, 0.6f};
    world.placed.push_back(dome);
    world.placed.push_back(behind);
    // ... and the guides round building sites: outlines four pixels wide in pieces of one map unit
    // that follow the ground, a hairline one beside them, and an order's line through them
    auto outline = [&](Vec3 middle, float half, uint32_t color, float width) {
      const Vec3 corners[4] = {{-half, 0, -half}, {half, 0, -half}, {half, 0, half}, {-half, 0, half}};
      for (int side = 0; side < 4; ++side) {
        const Vec3 from = middle + corners[side], to = middle + corners[(side + 1) % 4];
        const int pieces = std::max(static_cast<int>(half * 2), 1);
        for (int k = 0; k < pieces; ++k) {
          Vec3 a = from + (to - from) * (static_cast<float>(k) / static_cast<float>(pieces));
          Vec3 b = from + (to - from) * (static_cast<float>(k + 1) / static_cast<float>(pieces));
          if (world.HaveMap()) {
            a.y = world.GroundHeight(a.x, a.z);
            b.y = world.GroundHeight(b.x, b.z);
          }
          world.lines.push_back({a, b, color, width});
        }
      }
    };
    outline(at + Vec3{-20, 0, 16}, 2, 0xff40ff40u, 4);
    outline(at + Vec3{-16, 0, 16}, 2, 0xff40ff40u, 4);
    outline(at + Vec3{-12, 0, 16}, 2, 0xff4080ffu, 4);
    outline(at + Vec3{-8, 0, 20}, 2, 0xff40ff40u, 1);
    world.lines.push_back({at + Vec3{-26, world.HaveMap() ? world.GroundHeight(at.x - 26, at.z + 16) : 0, 16},
                           at + Vec3{-12, world.HaveMap() ? world.GroundHeight(at.x - 12, at.z + 16) : 0, 16}, 0xffffff40u, 4});
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
  world.focusArmy = opt.focus > 0 ? opt.focus - 1 : -1;  // (whose work bars and pause marks show)
  world.lifeBars = opt.bars;
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
  // --animate: one of the game's animations (.sca: per frame every bone's place and turn against
  // its parent, the turn as w x y z; the layout as the engine reads it, src/sim/anim.cpp), for the
  // placed units whose bones it names
  struct Animation {
    int frames = 0, bones = 0;
    float duration = 0;
    std::vector<std::string> names;  // lower case
    std::vector<float> data;         // frames x bones x 7
  } animation;
  if (!opt.animate.empty()) {
    const auto file = vfs.ReadFile(opt.animate);
    int32_t head[9] = {};
    if (file && file->size() >= 36 && !file->compare(0, 4, "ANIM")) std::memcpy(head, file->data(), 36);
    const int32_t frames = head[2], bones = head[4], namesAt = head[5], dataAt = head[7];
    const size_t stride = static_cast<size_t>(std::max(bones, 0)) * 28 + 8, first = static_cast<size_t>(std::max(dataAt, 0)) + 28;
    if (frames > 0 && bones > 0 && namesAt > 0 && dataAt > 0 && first + static_cast<size_t>(frames) * stride <= file->size()) {
      animation.frames = frames;
      animation.bones = bones;
      std::memcpy(&animation.duration, file->data() + 12, 4);
      size_t at = static_cast<size_t>(namesAt);
      for (int i = 0; i < bones && at < file->size(); ++i) {
        size_t end = file->find('\0', at);
        if (end == std::string::npos) end = file->size();
        std::string name(*file, at, end - at);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        animation.names.push_back(name);
        at = end + 1;
      }
      animation.names.resize(static_cast<size_t>(bones));
      animation.data.resize(static_cast<size_t>(frames) * static_cast<size_t>(bones) * 7);
      for (int f = 0; f < frames; ++f)
        std::memcpy(animation.data.data() + static_cast<size_t>(f) * static_cast<size_t>(bones) * 7, file->data() + first + static_cast<size_t>(f) * stride + 8,
                    static_cast<size_t>(bones) * 28);
      printf("render: animation %s: %d frames of %d bones, %.2f s\n", opt.animate.c_str(), frames, bones, animation.duration);
    } else {
      fprintf(stderr, "render: animation %s: not found, or not an .sca file\n", opt.animate.c_str());
    }
  }
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
  // --fx, --fx-shot: the effects test stand
  std::unique_ptr<FxStand> fxStand;
  if (!opt.fx.fx.empty() || !opt.fx.shot.empty() || !opt.fx.entity.empty() || !opt.fx.shield.empty() || !opt.fx.death.empty() ||
      !opt.fx.move.empty() || !opt.fx.build.empty() || !opt.fx.fire.empty()) {
    if (!opt.fxAt) opt.fx.at = {emitAt.x, 0, emitAt.z};
    if (!opt.fxRepeat && !opt.fx.shield.empty()) opt.fx.repeat = 20;
    if (!opt.fxRepeat && !opt.fx.death.empty()) opt.fx.repeat = 12;  // (a death's smoke and debris, a ship's sinking, take their time)
    // (an effect entity's play is long: a nuke's lasts some twenty seconds)
    if (!opt.fxRepeat && (!opt.fx.entity.empty() || opt.fx.shot.find("Nuke") != std::string::npos)) opt.fx.repeat = 25;
    if (!opt.hasCam) {  // close to it, a little from above
      camera.distance = !opt.fx.entity.empty() ? 160.0f : !opt.fx.shield.empty() ? 90.0f : !opt.fx.death.empty() || !opt.fx.move.empty() ? 40.0f : !opt.fx.build.empty() ? 30.0f : opt.fx.shot.empty() ? 14.0f : 40.0f;  // (a flash is a pace or two across; a shot's flight is long; a nuke is huge)
      camera.pitch = 0.55f;
    }
    {  // (a shot not placed: across the spot, away from the camera)
      Vec3 right, up, back;
      camera.Axes(&right, &up, &back);
      Vec3 away{-back.x, 0, -back.z};
      away = Length(away) > 1e-4f ? Normalize(away) : Vec3{0, 0, -1};
      if (!opt.fxFrom) opt.fx.from = opt.fx.at - away * 14 + Vec3{0, 1.5f, 0};
      if (!opt.fxTo) opt.fx.to = opt.fx.at + away * 14;
      opt.fx.toGiven = opt.fxTo;  // (--fx-fire: else the stand puts it in its weapons' reach, that way)
    }
    fxStand = std::make_unique<FxStand>();
    // (the map under a unit's death: the ground, and under water the bottom, and the water's level)
    if (haveMap)
      fxStand->SetGround([&map](float x, float z) { return map.Height(x, z); }, map.water.present, map.water.elevation,
                         [&map](float x, float z) -> int {  // (the map's terrain type code of the cell)
                           if (map.terrainTypes.empty() || map.width <= 0 || map.height <= 0) return -1;
                           const int cx = std::clamp(static_cast<int>(x), 0, map.width - 1), cz = std::clamp(static_cast<int>(z), 0, map.height - 1);
                           return map.terrainTypes[static_cast<size_t>(cz) * static_cast<size_t>(map.width) + static_cast<size_t>(cx)];
                         },
                         static_cast<float>(map.width), static_cast<float>(map.height));
    std::string fxError;
    if (fxStand->Init(&vfs, opt.fx, &fxError)) {
      printf("render: --fx: %s%s\n", fxStand->Describe().c_str(), opt.fx.compare ? "; remastered left, original right" : "");
      if (!opt.hasCam) {
        const Vec3 c = fxStand->Middle();
        camera.target = {c.x, groundHeight(c.x, c.z) + 1, c.z};
        if (!opt.fx.fire.empty()) targetLift = c.y;  // (a firing aircraft: the camera looks halfway up to it)
        // (a build: all of it in view; a move: the two sides' units, which the camera then follows;
        // near enough to see their effects)
        if (fxStand->Extent() > 0) camera.distance = std::max(12.0f, fxStand->Extent() * 0.6f / std::tan(camera.fovY * 0.5f));
      }
    } else {
      fprintf(stderr, "render: --fx: %s\n", fxError.c_str());
      fxStand.reset();
    }
  }
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
  CpuTimes::on = opt.timings;
  struct {  // --timings: each stage's time added up over the frames, in the order first met
    std::vector<std::pair<std::string, double>> sums;
    int frames = 0;
    double worst = 0;  // the longest whole frame (a hitch)
    // the card's stages and its whole frame, frame by frame: their middle and their lowest are what a
    // frame costs when other programs use the card too (they only ever add to it)
    std::vector<std::pair<std::string, std::vector<double>>> gpuFrames;
    void AddGpu(const std::string& name, double ms) {
      for (auto& s : gpuFrames)
        if (s.first == name) {
          s.second.push_back(ms);
          return;
        }
      gpuFrames.push_back({name, {ms}});
    }
    // the slowest frame of all, the first ones too (where loading on first sight goes), part by part
    double slowest = 0;
    int slowestFrame = 0;
    std::vector<std::pair<const char*, double>> slowestParts;
    void Add(const std::string& name, double ms) {
      for (auto& s : sums)
        if (s.first == name) {
          s.second += ms;
          return;
        }
      sums.emplace_back(name, ms);
    }
  } timing;

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
          if (e.key == 'N') {  // the views for checking art, one after another, then the picture again
            world.inspect = (world.inspect + 1) % 11;
            printf("render: inspect: %s\n", world.inspect ? kInspect[world.inspect] : "off");
          }
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
    camera.target.x += opt.pan[0];  // (--pan)
    camera.target.z += opt.pan[1];
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
        CpuTimes::Part("blueprints' looks taken from the simulation");
        world.SetLooks(looks);
        CpuTimes::Part(nullptr);
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
      CpuTimes::Part("the game's beats blended");
      frameAt(playTick, &frame);
      CpuTimes::Part(nullptr);
      if (haveLive && !framedLive && !opt.hasCam && !frame.units.empty()) {  // the first look at the game
        framedLive = true;
        lookAtUnits();
      }
    }

    if (!opt.picture.empty() && opt.pictureWindow && pictureWindow && playing && !liveBusy && frameCount >= 3) {
      // --picture-window: the world into the window, a made-up interface over it: a dark see-through
      // bar along the bottom, a solid white square at the top left, the rest open
      const int w = pictureWindow->Width(), h = pictureWindow->Height();
      std::vector<uint32_t> layer(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
      for (int y = h - h / 6; y < h; ++y)
        for (int x = 0; x < w; ++x) layer[static_cast<size_t>(y) * w + x] = 0xb0101010u;  // (premultiplied: 0x10 is the colour times 0xb0)
      for (int y = 20; y < 120; ++y)
        for (int x = 20; x < 220; ++x) layer[static_cast<size_t>(y) * w + x] = 0xffffffffu;
      picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, w, h);
      picture.DrawToWindow(camera, w, h, playTick + 1);
      std::vector<uint8_t> shown;
      picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, 0, 0);
      picture.CaptureNextWindowFrame(&shown);
      picture.DrawToWindow(camera, w, h, playTick + 1);
      printf("render: --picture-window: %dx%d %s\n", w, h, shown.size() >= static_cast<size_t>(w) * h * 4 && WritePng(opt.picture, shown.data(), w, h) ? opt.picture.c_str() : "could not be read back");
      {  // what a frame costs this way, the whole layer handed over each time (the worst case)
        picture.SetVsync(false);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) {
          picture.SetInterfaceLayer(layer.data(), w, h, w, 0, 0, w, h);
          picture.DrawToWindow(camera, w, h, playTick + 1);
        }
        printf("render: --picture-window: %.2f ms a frame (vertical sync off), the whole layer handed over each time\n",
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
      }
      break;
    }
    if (!opt.picture.empty() && playing && !liveBusy && frameCount >= 3) {
      // --picture: the world through the pixel interface, and what it says about the screen
      const int w = device->Width(), h = device->Height();
      {  // what the interface shows in the world: some units selected, a building's ghost, a line and a ring
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < frame.units.size(); i += 3) ids.push_back(frame.units[i].id);
        picture.SetSelection(ids);
        picture.SetLifeBars(true);  // (and their anchors, for the custom names)
        picture.SetIconColors({0xffff00ffu, 0xff00ffffu});  // (the team colour button: the first two armies' icons magenta and cyan)
        if (opt.playable[2] > opt.playable[0]) picture.SetPlayableRect(opt.playable[0], opt.playable[1], opt.playable[2], opt.playable[3]);
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
          // and the interface's pictures on the ground: a nuke's outer ring round the camera's target, a
          // weapon's splash ring at the ghost's spot
          WorldPicture::UserDecal outer{"/textures/ui/common/game/AreaTargetDecal/nuke_icon_outer.dds", camera.target.x, camera.target.z, 40, 40};
          WorldPicture::UserDecal splash{"/textures/ui/common/game/AreaTargetDecal/weapon_icon_small.dds", spot.x, spot.z, 10, 10};
          picture.SetUserDecals({outer, splash});
        }
      }
      // where the interface would write the units' custom names: asked first, for the frame then drawn
      std::vector<WorldPicture::LifeBarAnchor> anchors;
      picture.LifeBarAnchors(camera, w, h, playTick + 1, &anchors);
      const std::vector<uint32_t>& pixels = picture.Draw(camera, w, h, playTick + 1);
      std::vector<uint8_t> bytes(pixels.size() * 4);
      for (size_t i = 0; i < pixels.size(); ++i) {
        bytes[i * 4] = static_cast<uint8_t>(pixels[i] >> 16);
        bytes[i * 4 + 1] = static_cast<uint8_t>(pixels[i] >> 8);
        bytes[i * 4 + 2] = static_cast<uint8_t>(pixels[i]);
        bytes[i * 4 + 3] = 255;
      }
      {  // (checked: the pixel just above each anchor is its lowest bar's; each marked with a magenta dot)
        int under = 0;
        for (const WorldPicture::LifeBarAnchor& a : anchors) {
          const int x = static_cast<int>(a.x), y = static_cast<int>(a.y);
          if (x < 0 || x >= w || y < 1 || y >= h) continue;
          const uint32_t above = pixels[static_cast<size_t>(y - 1) * static_cast<size_t>(w) + static_cast<size_t>(x)];
          const uint32_t r = (above >> 16) & 0xff, g = (above >> 8) & 0xff, b = above & 0xff;
          // (a bar is black, or green, yellow, red, orange or pale blue: no grey ground)
          if ((r < 24 && g < 24 && b < 24) || std::max({r, g, b}) - std::min({r, g, b}) > 120) ++under;
          for (int dy = 0; dy < 3; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int px = x + dx, py = y + dy;
              if (px < 0 || px >= w || py >= h) continue;
              uint8_t* p = bytes.data() + (static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 4;
              p[0] = 255, p[1] = 0, p[2] = 255;
            }
        }
        printf("render: --picture: %zu life bar anchors, %d of them just under a bar\n", anchors.size(), under);
      }
      printf("render: --picture: %dx%d %s\n", w, h, WritePng(opt.picture, bytes.data(), w, h) ? opt.picture.c_str() : "could not be written");
      {  // what a call costs, read-back included
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) picture.Draw(camera, w, h, playTick + 1);
        printf("render: --picture: %.2f ms a call\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 30);
      }
      {  // what the overlay of numbers would show of it, after those calls (WorldPicture::LastStats)
        const WorldPicture::Stats& s = picture.LastStats();
        printf("render: --picture: stats: processor %.2f ms waiting, %.2f making ready, %.2f drawing, %.2f ending; card %.2f ms in %zu stages; "
               "%d draw calls, %d models, %lld + %lld shadow + %lld ground triangles, %d particles of %d emitters, textures %lld MB, card memory %lld MB\n",
               s.waitMs, s.prepareMs, s.drawMs, s.endMs, s.gpuMs, s.gpuStages.size(), s.drawCalls, s.models, s.triangles, s.shadowTriangles,
               s.groundTriangles, s.particles, s.emitters, s.textureBytes >> 20, s.cardBytes >> 20);
      }
      {  // the minimap, in the map's proportion
        float mw, mh;
        picture.MapSize(&mw, &mh);
        if (opt.playable[2] > opt.playable[0]) {  // (--playable: that part's proportion)
          mw = opt.playable[2] - opt.playable[0];
          mh = opt.playable[3] - opt.playable[1];
        }
        const int mapH = 400, mapW = mh > 0 ? static_cast<int>(400 * mw / mh) : 400;
        // (the ground under the picture's middle must be the same place before and after the minimap's
        // first drawing of a size: that pass has a camera of its own, which must not stay)
        Vec3 before, after;
        const bool hadGround = picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f, &before);
        const std::vector<uint32_t>& mini = picture.DrawMinimap(mapW, mapH);
        const bool hasGround = picture.GroundAt(static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f, &after);
        printf("render: --picture: the ground under the middle before the minimap %.1f %.1f, after it %.1f %.1f%s\n", before.x, before.z, after.x, after.z,
               hadGround && hasGround && Length(before - after) < 0.01f ? "" : " (NOT the same)");
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
        if (shown == 0) {  // (and where the picture would draw it at this moment, for a camera that follows it)
          float p[3], q[4];
          if (picture.UnitTransform(u.id, playTick + 1, p, q))
            printf("render: --picture: unit %x as drawn now: %.2f %.2f %.2f, turned %.3f %.3f %.3f %.3f (x y z w); %.2f from the viewer's own frame\n", u.id, p[0], p[1], p[2],
                   q[0], q[1], q[2], q[3], Length(Vec3{p[0], p[1], p[2]} - Vec3{u.position[0], u.position[1], u.position[2]}));
          else
            printf("render: --picture: unit %x: UnitTransform has it not (FAILED)\n", u.id);
        }
        if (++shown == 6) break;
      }
      {  // the campaign's: the world off (black under the interface), a playable part that leaves a unit
         // outside (then not found at its pixel), and a cinematic's icons and selection marks off
        picture.SetWorldShown(false);
        const std::vector<uint32_t>& dark = picture.Draw(camera, w, h, playTick + 1);
        size_t lit = 0;
        for (uint32_t p : dark) lit += (p & 0x00ffffffu) != 0;
        picture.SetWorldShown(true);
        printf("render: --picture: the world off: %zu of %zu pixels not black%s\n", lit, dark.size(), lit ? " (NOT black)" : "");
        float mw, mh;
        picture.MapSize(&mw, &mh);
        picture.Draw(camera, w, h, playTick + 1);
        for (const FrameUnit& u : frame.units) {
          float x, y;
          if (u.dead || !picture.Project({u.position[0], u.position[1], u.position[2]}, &x, &y) || x < 0 || y < 0 || x >= static_cast<float>(w) ||
              y >= static_cast<float>(h) || picture.UnitAt(x, y) != u.id)
            continue;
          const bool west = u.position[0] > mw * 0.5f;  // (the half of the map it is not in)
          picture.SetPlayableRect(west ? 0 : mw * 0.5f, 0, west ? mw * 0.5f : mw, mh);
          picture.Draw(camera, w, h, playTick + 1);
          const uint32_t outside = picture.UnitAt(x, y);
          picture.SetPlayableRect(0, 0, 0, 0);
          picture.Draw(camera, w, h, playTick + 1);
          const uint32_t again = picture.UnitAt(x, y);
          printf("render: --picture: unit %x outside a playable half: the unit there %x%s; the whole map again: %x%s\n", u.id, outside,
                 outside == 0xffffffffu ? " (none, right)" : " (WRONG)", again, again == u.id ? " (itself)" : " (WRONG)");
          break;
        }
        picture.SetStrategicIcons(false);
        picture.SetSelectionMarks(false);
        picture.Draw(camera, w, h, playTick + 1);
        picture.SetStrategicIcons(true);
        picture.SetSelectionMarks(true);
        printf("render: --picture: icons and selection marks off, then on again: drawn\n");
      }
      break;
    }
    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    const auto frameStart = std::chrono::steady_clock::now();
    device->BeginFrame(clear);
    const auto frameBegun = std::chrono::steady_clock::now();
    // effects run on the game's clock while a game plays (so they stand still when it is paused)
    {  // the time of day: where it started, what T added, and what has passed of a running day
      float hours = (opt.timeOfDay >= 0 ? opt.timeOfDay : 12) + dayShift;
      if (opt.dayLength > 0) hours += time / opt.dayLength * 24;
      world.timeOfDay = dayOn ? std::fmod(hours, 24.0f) : -1;
    }
    {  // --fx-move: the camera follows the driven units (where the stand had them last frame)
      Vec3 at;
      if (fxStand && !opt.hasCam && fxStand->Focus(&at)) camera.target = at;
    }
    world.Begin(camera, device->Width(), device->Height(), time, havePlay ? playTick * 0.1 : static_cast<double>(time));

    if (fxStand) {  // the effects test stand
      Vec3 right, up, back;
      camera.Axes(&right, &up, &back);
      fxStand->Update(world, time, right, groundHeight);
    }
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
    if (opt.ranges) {  // (... and with the test rings: a quantum gate's field and the Cybran zapper's three balls)
      const Vec3 base{opt.cam[0] + 6, (world.HaveMap() ? world.GroundHeight(opt.cam[0] + 6, opt.cam[1] + 6) : 0) + 1.2f, opt.cam[1] + 6};
      static const char* const kTest[] = {"/effects/entities/forcefield01/forcefield01_mesh.bp", "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere01_mesh.bp",
                                          "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere02_mesh.bp", "/effects/entities/cybranphalanxsphere01/cybranphalanxsphere03_mesh.bp"};
      for (int k = 0; k < 4; ++k) {
        const Model* model = models.Blueprint(kTest[k]);
        if (model && model->ok) meshes.Add(model, Mat4::FromTRS(base + Vec3{static_cast<float>(k) * 3.5f, 0, 0}, Quat{}, model->scale * (k ? 1.2f : 1.0f)), {1, 1, 1});
      }
      // ... and a shield's flash where it is hit, the plain one and the Cybran's, each at four ages
      // of its two seconds (it is a patch of a bubble; here of one twelve across, turned to face upward)
      for (int k = 0; k < 8; ++k) {
        const Model* model = models.Blueprint(k < 4 ? "/effects/entities/shieldsection01/shieldsection01_mesh.bp" : "/effects/entities/shieldsection01/shieldsection01b_mesh.bp");
        const Vec3 where = base + Vec3{static_cast<float>(k % 4) * 7.0f - 8, -1.0f, k < 4 ? -9.0f : -13.5f};
        if (model && model->ok) meshes.Add(model, Mat4::FromTRS(where, Quat{-0.7071f, 0, 0, 0.7071f}, model->scale * 12), {0.15f + 0.55f * static_cast<float>(k % 4), -0.37f, 0});
        // (two of each on a bubble of that size, as in a game; the others bare, to see the flash alone)
        const Model* bubble = k % 4 == 1 || k % 4 == 2 ? models.Blueprint("/effects/entities/shield01/shield01_mesh.bp") : nullptr;
        if (bubble && bubble->ok) meshes.Add(bubble, Mat4::FromTRS(where, Quat{}, bubble->scale * 12), {1, 1, 1});
      }
    }
    for (PlacedUnit& u : placedUnits) {
      Pose pose;
      if (u.turned < 0 && animation.frames > 0) {
        // the pose a simulation would hand over at that moment of the animation: every bone's place
        // and turn down the chain from the unit (a bone the file does not name keeps its rest
        // against its parent)
        const std::vector<ModelBone>& bones = u.model->bones;
        const float scale = u.model->scale;
        float where = opt.animateAt >= 0 ? std::min(opt.animateAt, 1.0f) : std::fmod(time * 0.25f, 1.0f);
        where *= static_cast<float>(animation.frames - 1);
        const int frame = std::min(static_cast<int>(where), animation.frames - 1), after = std::min(frame + 1, animation.frames - 1);
        const float part = where - static_cast<float>(frame);
        std::vector<Vec3> place(bones.size());
        std::vector<Quat> turn(bones.size());
        u.bones.clear();
        bool named = false;
        for (size_t i = 0; i < bones.size(); ++i) {
          const int up = bones[i].parent;
          const bool hasUp = up >= 0 && static_cast<size_t>(up) < i;
          // (its rest against its parent, from the rests the model holds against the unit)
          Vec3 p = hasUp ? Rotate(Conjugate(bones[static_cast<size_t>(up)].rotation), bones[i].position - bones[static_cast<size_t>(up)].position) : bones[i].position;
          Quat q = hasUp ? Mul(Conjugate(bones[static_cast<size_t>(up)].rotation), bones[i].rotation) : bones[i].rotation;
          for (int b = 0; b < animation.bones; ++b)
            if (animation.names[static_cast<size_t>(b)] == bones[i].name) {
              const float* a = animation.data.data() + (static_cast<size_t>(frame) * static_cast<size_t>(animation.bones) + static_cast<size_t>(b)) * 7;
              const float* c = animation.data.data() + (static_cast<size_t>(after) * static_cast<size_t>(animation.bones) + static_cast<size_t>(b)) * 7;
              p = Lerp(Vec3{a[0], a[1], a[2]}, Vec3{c[0], c[1], c[2]}, part);
              q = Nlerp(Quat{a[4], a[5], a[6], a[3]}, Quat{c[4], c[5], c[6], c[3]}, part);
              named = true;
              break;
            }
          place[i] = hasUp ? place[static_cast<size_t>(up)] + Rotate(turn[static_cast<size_t>(up)], p) : p;
          turn[i] = hasUp ? Mul(turn[static_cast<size_t>(up)], q) : q;
          const Vec3 w = place[i] * scale;
          u.bones.push_back({static_cast<uint32_t>(i), {w.x, w.y, w.z}, {turn[i].x, turn[i].y, turn[i].z, turn[i].w}, false});
        }
        if (!named) u.bones.clear();  // (not this unit's animation)
        pose.now = pose.next = u.bones.data();
        pose.nowCount = pose.nextCount = u.bones.size();
      }
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
          u.bones.push_back({static_cast<uint32_t>(i), {p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}, opt.hideTurned});
        }
        pose.now = pose.next = u.bones.data();
        pose.nowCount = pose.nextCount = u.bones.size();
      }
      const bool posed = u.turned >= 0 || !u.bones.empty();
      meshes.Add(u.model, Mat4::FromTRS(u.position, Quat{}, u.model->scale), kArmyColors[0], opt.shielded ? 3.0f : opt.built, posed ? &pose : nullptr);
      world.AddLamps(u.model, u.position, Quat{}, posed ? &pose : nullptr, &kArmyColors[0]);
      icons.Add(u.icon, u.position, kArmyColors[0]);
    }
    if (haveLive && playing) {  // what the scripts did with effects in the beats since the last frame
      live.TakeEffectEvents(&effectEvents);
      {  // how the scenery changed
        std::vector<FramePropEvent> changed;
        live.TakePropEvents(&changed);
        if (!changed.empty()) world.AddPropEvents(changed);
      }
      {  // how the simulation changed the ground (laid flat under a structure)
        std::vector<FrameGroundChange> ground;
        live.TakeGroundChanges(&ground);
        if (!ground.empty() && opt.groundChanges) world.ChangeGround(ground);
      }
      CpuTimes::Part("effects");
      effects.Queue(std::move(effectEvents));
      effects.Update(playTick, frame);
      CpuTimes::Part(nullptr);
    }
    effects.shotLight = opt.shotLight >= 0 ? opt.shotLight : ((frameCount / 30) % 2 ? 0.0f : -opt.shotLight);
    if (playing) world.AddFrame(frame, armyColors);
    // --find: once the game is where it was asked to be, the camera goes to the first such thing
    // (a shield's flash lasts two seconds and may not be there at that moment: that search goes on,
    // frame after frame, until one shows)
    static bool impactFound = false;
    if (!opt.find.empty() && haveLive && !(automatic && liveBusy) && (frameCount == 1 || (opt.find == "impact" && !impactFound && frameCount > 1))) {
      if (opt.find == "shield") {
        for (const FrameEntity& f : frame.entities)
          if (f.shield) {
            camera.target = {f.position[0], f.position[1], f.position[2]};
            camera.distance = f.scale[0] * 2.4f;
            break;
          }
      } else if (opt.find == "impact" && frame.blueprints) {
        // a shield being hit: one with a flash on it now, or else the one with the most shots flying near it
        bool flashing = false;
        int most = frameCount == 1 ? -1 : 1 << 30;  // (after the first frame only a flash moves the camera)
        for (const FrameEntity& f : frame.entities) {
          if (f.mesh < frame.blueprints->size()) {
            std::string name = (*frame.blueprints)[f.mesh];
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find("shieldsection") != std::string::npos) {
              camera.target = {f.position[0], f.position[1], f.position[2]};
              camera.distance = f.scale[0] * 2.2f;
              flashing = true;
              break;
            }
          }
          if (!f.shield) continue;
          int near = 0;
          for (const FrameProjectile& q : frame.projectiles) {
            const float dx = q.position[0] - f.position[0], dz = q.position[2] - f.position[2];
            const float speed2 = q.velocity[0] * q.velocity[0] + q.velocity[1] * q.velocity[1] + q.velocity[2] * q.velocity[2];
            if (dx * dx + dz * dz < f.scale[0] * f.scale[0] * 1.5f && speed2 > 25) ++near;
          }
          if (near > most) {
            most = near;
            camera.target = {f.position[0], f.position[1], f.position[2]};
            camera.distance = f.scale[0] * 2.2f;
          }
        }
        impactFound = flashing;
        if (flashing) printf("render: --find impact: a flash on a shield at %.0f, %.0f (frame %d)\n", camera.target.x, camera.target.z, frameCount);
        else if (frameCount == 1) printf("render: --find impact: no flash yet; the shield with the most shots near it is at %.0f, %.0f\n", camera.target.x, camera.target.z);
      } else if (opt.find == "collider" && frame.blueprints) {
        // the helper shell FAF hangs on a crashing plane so that it can hit a shield (a box ten across
        // that its script hides from everybody): where one is now, to see that it is not drawn
        for (const FrameProjectile& q : frame.projectiles) {
          if (q.blueprint >= frame.blueprints->size()) continue;
          std::string name = (*frame.blueprints)[q.blueprint];
          for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (name.find("shieldcollider") == std::string::npos) continue;
          camera.target = {q.position[0], q.position[1], q.position[2]};
          camera.distance = 40;
          printf("render: --find collider: one at %.0f, %.0f, %.1f above the ground, %s\n", q.position[0], q.position[2],
                 q.position[1] - (world.HaveMap() ? world.GroundHeight(q.position[0], q.position[2]) : 0), q.seen ? "to be seen" : "hidden");
          break;
        }
      } else if (opt.find == "dying") {  // a unit in its death: the one highest above the ground (a plane falling)
        float best = -1e9f;
        for (const FrameUnit& u : frame.units) {
          if (!u.dead) continue;
          const float above = u.position[1] - (world.HaveMap() ? world.GroundHeight(u.position[0], u.position[2]) : 0);
          if (above > best) {
            best = above;
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 22;
          }
        }
        if (best > -1e8f) printf("render: --find dying: one %.1f above the ground at %.0f, %.0f\n", best, camera.target.x, camera.target.z);
      } else if (opt.find == "fight") {  // where the most shots are in flight: the shot with the most others near it
        size_t best = 0;
        int most = -1;
        for (size_t a = 0; a < frame.projectiles.size(); a += std::max<size_t>(frame.projectiles.size() / 200, 1)) {
          int near = 0;
          for (const FrameProjectile& b : frame.projectiles) {
            const float dx = b.position[0] - frame.projectiles[a].position[0], dz = b.position[2] - frame.projectiles[a].position[2];
            const float speed2 = b.velocity[0] * b.velocity[0] + b.velocity[1] * b.velocity[1] + b.velocity[2] * b.velocity[2];
            if (dx * dx + dz * dz < 30 * 30 && speed2 > 25) ++near;  // (what flies: the UEF's build slices stand still)
          }
          if (near > most) {
            most = near;
            best = a;
          }
        }
        if (most > 0) {
          const FrameProjectile& p = frame.projectiles[best];
          camera.target = {p.position[0], world.HaveMap() ? world.GroundHeight(p.position[0], p.position[2]) : p.position[1], p.position[2]};
          camera.distance = 45;
          printf("render: --find fight: %d shots within 30 of %.0f, %.0f\n", most, camera.target.x, camera.target.z);
        }
      } else if (opt.find == "wreck") {
        const std::vector<Vec3> wrecks = world.WreckPlaces(1);
        if (!wrecks.empty()) {
          camera.target = wrecks[0];
          camera.distance = 14;
        }
      } else if (opt.find == "uefbuild" && frame.blueprints) {  // a UEF building a third to two thirds built
        for (const FrameUnit& u : frame.units) {
          if (u.blueprint >= frame.blueprints->size() || u.fractionComplete < 0.3f || u.fractionComplete > 0.7f) continue;
          const std::string& id = (*frame.blueprints)[u.blueprint];
          if (id.size() > 2 && id[0] == 'u' && id[1] == 'e' && id[2] == 'b') {
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 16;
            break;
          }
        }
      } else if (frame.blueprints) {  // a unit's blueprint id: the first unit of that kind (with --enhance: of that army)
        for (const FrameUnit& u : frame.units)
          if (u.blueprint < frame.blueprints->size() && (*frame.blueprints)[u.blueprint] == opt.find &&
              (opt.enhancement.empty() || static_cast<int>(u.Army()) + 1 == opt.enhanceArmy)) {
            camera.target = {u.position[0], u.position[1], u.position[2]};
            camera.distance = 9;
            break;
          }
      }
    }
    if (!haveMap) DrawTestScene(draw);
    const auto renderStart = std::chrono::steady_clock::now();
    world.Render();
    const auto renderEnd = std::chrono::steady_clock::now();

    if (!(automatic && liveBusy)) {  // an automatic run counts its frames from where it was asked to look
      if (frameCount == 0) countedFrom = std::chrono::steady_clock::now();
      ++frameCount;
    }
    const bool lastFrame = opt.frames > 0 && frameCount >= opt.frames;
    // (--shots <n>: every n-th counted frame is written too, as <screenshot>_0001.png ...: a thing
    // that moves or flickers, looked at over time in one and the same game)
    const bool series = opt.shots > 0 && automatic && !liveBusy && !opt.screenshot.empty() && !lastFrame && frameCount % opt.shots == 0;
    const bool shoot = wantScreenshot || series || (lastFrame && !opt.screenshot.empty());
    device->EndFrame(shoot ? &capture : nullptr);
    if (opt.timings) {  // (every frame, the first ones too: the slowest, part by part)
      const double whole = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
      if (whole > timing.slowest) {
        timing.slowest = whole;
        timing.slowestFrame = frameCount;
        timing.slowestParts = CpuTimes::parts;
        timing.slowestParts.emplace_back("(drawing recorded, all of it)", std::chrono::duration<double, std::milli>(renderEnd - renderStart).count());
      }
    }
    if (opt.timings && !(automatic && liveBusy) && frameCount > 2) {  // (the first frames load what they need)
      const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      const auto frameEnd = std::chrono::steady_clock::now();
      timing.Add("cpu: waiting for a free frame", ms(frameStart, frameBegun));
      timing.Add("cpu: the game's frame, units, effects listed", ms(frameBegun, renderStart));
      timing.Add("cpu: drawing recorded", ms(renderStart, renderEnd));
      timing.Add("cpu: handed over and shown", ms(renderEnd, frameEnd));
      timing.Add("cpu: whole frame", ms(frameStart, frameEnd));
      timing.worst = std::max(timing.worst, ms(frameStart, frameEnd));
      std::vector<std::pair<const char*, double>> stages;
      device->TakeTimes(&stages);
      double gpu = 0;
      for (const auto& s : stages) {
        timing.Add(std::string("gpu: ") + s.first, s.second);
        timing.AddGpu(s.first, s.second);
        gpu += s.second;
      }
      if (!stages.empty()) {
        timing.Add("gpu: whole frame", gpu);
        timing.AddGpu("whole frame", gpu);
      }
      for (const auto& part : CpuTimes::parts) timing.Add(std::string("cpu part: ") + part.first, part.second);
      ++timing.frames;
    }
    CpuTimes::parts.clear();
    if (shoot && !capture.empty()) {
      std::string path = wantScreenshot ? "screenshot" + std::to_string(++shotNumber) + ".png" : opt.screenshot;
      if (series && !wantScreenshot) {
        char number[16];
        snprintf(number, sizeof number, "_%04d", frameCount / opt.shots);
        const size_t dot = path.rfind('.');
        path = dot == std::string::npos ? path + number : path.substr(0, dot) + number + path.substr(dot);
      }
      if (WritePng(path, capture.data(), device->Width(), device->Height())) printf("render: wrote %s\n", path.c_str());
      else fprintf(stderr, "render: cannot write %s\n", path.c_str());
      wantScreenshot = false;
    }
    if (lastFrame) {
      // (with a simulation: the time from the first counted frame, so loading and running ahead stay out)
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - (haveLive ? countedFrom : start)).count();
      const int timed = haveLive ? std::max(1, frameCount - 1) : frameCount;
      printf("render: %d frames in %.2f s (%.2f ms each)", frameCount, seconds, 1000.0 * seconds / timed);
      if (haveMap) printf("; ground: %d patches, %d triangles, %d changes by the game", terrain->drawnPatches, terrain->drawnTriangles, world.groundChanges);
      printf("; models: %d drawn, %d triangles, %d more in shadow maps", meshes.drawnInstances, meshes.drawnTriangles, meshes.shadowTriangles);
      if (iconsOn) printf("; %d icons", icons.drawn);
      if (particles.liveParticles || particles.liveEmitters)
        printf("; %d particles of %d emitters (%d quiet), %.2f ms", particles.liveParticles, particles.liveEmitters, particles.quietEmitters, particles.cpuMs);
      if (havePlay) printf("; %d posed, %zu projectiles, %zu beams", meshes.posedInstances, frame.projectiles.size(), frame.beams.size());
      if (meshes.posedInstances) printf("; bones %.2f ms", meshes.prepareMs);
      if (lights.count) printf("; %d lights", lights.count);
      printf("; textures %.0f MB", static_cast<double>(world.textures.bytes) / (1 << 20));
      if (haveLive) printf("; scenery: %d changes, %d gone, %d made in the game standing, %d props of %d squares gone through; %zu script entities", world.propsChanged, world.propsGone, world.MadeProps(), world.propsAdded, world.propCellsSeen, frame.entities.size());
      if (opt.focus > 0) printf("; %d buildings remembered, %zu of them gone", world.RememberedCount(), world.rememberedGone.size());
      if (opt.debug) printf("; %d lamps with shadows", world.lampShadowsDrawn);
      if (haveLive && opt.debug) {  // (where to look for them)
        printf("; %d emitter curves set by scripts", effects.curvesSet);
        for (const Vec3& p : world.WreckPlaces(6)) printf("; wreck at %.0f, %.0f", p.x, p.z);
        int shields = 0;
        for (const FrameEntity& f : frame.entities)
          if (f.shield && shields++ < 4) printf("; shield at %.0f, %.0f (size %.1f)", f.position[0], f.position[2], f.scale[0]);
        int big = 0;  // (shells drawn large: a ball of fire, a box of light, or a size gone wrong)
        for (const FrameProjectile& q : frame.projectiles)
          if (q.sized && std::max(q.size[0], std::max(q.size[1], q.size[2])) > 6 && frame.blueprints && q.blueprint < frame.blueprints->size() && big++ < 6)
            printf("; shell %s of size %.1f, %.1f, %.1f at %.0f, %.0f", (*frame.blueprints)[q.blueprint].c_str(), q.size[0], q.size[1], q.size[2], q.position[0], q.position[2]);
        printf("; camera at %.0f, %.0f", camera.target.x, camera.target.z);
        printf("; %d units in their death drawn", world.dyingDrawn);
        printf("; %d lights from shots and beams", effects.shotLights);
        {  // (what the scripts have made that has a mesh, by mesh: to tell what a shape in the picture is)
          std::map<std::string, int> made;
          for (const FrameEntity& f : frame.entities)
            if (frame.blueprints && f.mesh < frame.blueprints->size()) ++made[(*frame.blueprints)[f.mesh]];
          for (const auto& m : made) printf("; %d x %s", m.second, m.first.c_str());
        }
      }
      if (haveLive)
        printf("; effects: %d live of %d started, %d events of kinds not drawn yet, %d bands, %.2f ms; %d decals drawn of %d", effects.live,
               effects.started, effects.notShown, ribbons.drawnBands, effects.cpuMs, decals.drawn, decals.Count());
      if (haveLive) printf("; simulation at tick %u, %.2f ms a tick", live.LastTick(), live.MsPerTick());
      printf("\n");
      if (opt.timings && timing.frames > 0) {
        printf("render: --timings: %d frames at %dx%d on %s%s, average ms a frame:\n", timing.frames, device->Width(), device->Height(),
               device->AdapterName().c_str(), vsync ? " (vsync on: the waits include it)" : "");
        for (const auto& s : timing.sums) printf("render: --timings:   %-48s %7.3f\n", s.first.c_str(), s.second / timing.frames);
        printf("render: --timings:   %-48s %7.3f\n", "cpu: the longest frame", timing.worst);
        for (const auto& s : timing.gpuFrames) {
          std::vector<double> g = s.second;
          std::sort(g.begin(), g.end());
          printf("render: --timings:   %-48s %7.3f\n", ("gpu: " + s.first + ", the middle one").c_str(), g[g.size() / 2]);
          printf("render: --timings:   %-48s %7.3f\n", ("gpu: " + s.first + ", the lowest").c_str(), g.front());
        }
      }
      if (opt.timings && timing.slowest > 0) {
        printf("render: --timings: the slowest frame of all (frame %d, the first ones counted too): %.1f ms; its parts over 1 ms:\n", timing.slowestFrame,
               timing.slowest);
        for (const auto& p : timing.slowestParts)
          if (p.second >= 1) printf("render: --timings:   %-48s %7.1f\n", p.first, p.second);
      }
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
