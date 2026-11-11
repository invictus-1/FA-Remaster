#include "world.h"
#include "marks.h"

#include "cpu_times.h"

#include <chrono>

#include "dds.h"

#include <algorithm>
#include <array>  // [snowhitch]
#include <cctype>  // [glitches]
#include <cmath>
#include <cstdio>
#include <limits>  // [nightlight]

#include "core/vfs.h"
#include "effect_files.h"
#include "remasters.h"
#include "ground.h"
#include "snow_ground.h"  // [snow ground]
#include "volfx_log.h"  // (MOHO64_DEBUG_VOLFX)
#include "bp_reader.h"  // [dents]

namespace render {

namespace {
// [glitches] (World::finishedSlabFade) The UEF's build glow slab: "/effects/Entities/UEFBuildEffect/UEFBuildEffect02_proj.bp"
// in any case (the effects' blueprints are paths as the scripts wrote them).
bool BuildGlowSlab(const std::string& blueprint) {
  static const char kName[] = "uefbuildeffect02_proj";
  const size_t n = sizeof kName - 1;
  if (blueprint.size() < n) return false;
  for (size_t at = 0; at + n <= blueprint.size(); ++at) {
    size_t k = 0;
    while (k < n && std::tolower(static_cast<unsigned char>(blueprint[at + k])) == kName[k]) ++k;
    if (k == n) return true;
  }
  return false;
}
}  // namespace

World::World(gfx::Device* device, const moho::Vfs* vfs)
    : textures(device, vfs),
      terrain(device, &textures),
      water(device, &textures),
      models(device, vfs, &textures),
      meshes(device, &textures),
      particles(device, vfs, &textures),
      ribbons(device, vfs, &textures),
      decals(device, vfs, &textures),
      lights(device, vfs),
      effects(&particles, &ribbons, &decals, &lights, &models),
      icons(device, vfs),
      draw(device),
      ao(device),
      post(device),
      sunMask(device),  // [shadowmask]
      air(device),
      rays(device),
      bounce(device),
      blasts(device),
      debris(device, &textures),
      outlines(device),
      dust(device),
      build(device),
      exhausts(device, vfs),  // (unitsfx)
      clouds(device),
      cloudZoomDraw(device),  // (cloudzoom)
      motion(device),
      upscaler(device),
      dof(device),
      fsr3(device),
      framegen(device),
      skyDraw(device),
      falling(device),
      grass(device),
      wildlifeDraw(device),
      sight(device),
      shafts(device),
      tracks(device),
      trackField(device, &textures),
      plantLife(device),  // [plants]
      wakes(device),
      hullWash(device),  // [hulls]
      fxMirror(device),  // [fxmirror]
      ranges(device),
      placedDraw(device, &models),
      orders(device, &textures),
      shadows(device),
      smokeMap(device),
      volumes(device, vfs, &textures),
      device_(device),
      vfs_(vfs),
      energy(device),
      photoFinish(device),  // (photolook)
      horizon(device),  // [horizon]
      leaves(device),  // [seasons]
      sunlight(device),  // [sunlight]
      streams(device, &textures) {  // [stream]
  particles.rampGlow = [this](const std::string& ramp) { return lights.RampGlow(ramp); };  // (what is hot: lit smoke's fires, heat shimmer)
  // (movement dust: how each kind of unit moves, from its blueprint)
  dust.readBlueprint = [this](const std::string& id, BpValue* out) {
    auto file = textures.ReadFile(models.UnitBlueprintPath(id));
    return file && ReadBlueprintFile(*file, out);
  };
  wear.readBlueprint = dust.readBlueprint;  // [unit dirt] (how each kind of unit moves: what takes mud)
  water.surf.readBlueprint = dust.readBlueprint;  // ([surf] which units are ships, and how fast they go)
  volumes.rampGlow = [this](const std::string& ramp) { return lights.RampGlow(ramp); };    // (volume explosions: a nuke's flash's hue)
}

World::~World() {
  if (surroundAhead_.valid()) surroundAhead_.wait();  // [surahead] (it reads the map and the surround: before they go)
  if (framegenAsked_) device_->SetFrameGeneration({});
}

bool World::Open(const std::string& mapPath, const Options& options, std::string* error) {
  // (how long each step of opening takes, for the log: loading is measured like the frames)
  auto stepStart = std::chrono::steady_clock::now();
  std::string steps;
  auto step = [&](const char* name) {
    const auto now = std::chrono::steady_clock::now();
    char b[96];
    snprintf(b, sizeof b, "%s%s %.2f s", steps.empty() ? "" : ", ", name, std::chrono::duration<double>(now - stepStart).count());
    steps += b;
    stepStart = now;
  };
  // the pipelines of everything opened made together at the end, their shaders compiled first on several cores
  // where the cache lacks them (gfx: a pipeline batch; ended on the way out too, should the opening fail)
  device_->BeginPipelineBatch();
  struct Batch {
    gfx::Device* device;
    bool open = true;
    ~Batch() {
      if (open) device->EndPipelineBatch(nullptr);
    }
  } batch{device_};
  if (!draw.Init(error) || !meshes.Init(error) || !particles.Init(error) || !ribbons.Init(error) || !decals.Init(error) || !icons.Init(error) ||
      !ao.Init(error) || !air.Init(error) || !clouds.Init(error) || !motion.Init(error) || !post.Init(error) || !upscaler.Init(error) || !skyDraw.Init(error) || !falling.Init(error) || !sight.Init(error) || !shafts.Init(error) || !tracks.Init(error) || !wakes.Init(error) || !ranges.Init(error) || !placedDraw.Init(error) || !orders.Init(error) || !outlines.Init(error)) {
    *error = "shader: " + *error;
    return false;
  }
  step("shaders");
  effects.verbose = options.verbose;
  effects.SetTracks(&tracks);
  effects.SetBlasts(&blasts);
  effects.isProp = [this](uint32_t id) { return IsProp(id); };  // [scars] (a flash made on a prop is no explosion)
  // [whitecaps] (what hangs on a ship's bones follows its hull riding the lifted waves: as the unit's drawing, RideWaves)
  effects.drawnAt = [this](const std::string& blueprint, const FrameUnit& u, const Model* model, Vec3* position, Quat* orientation) {
    const bool building = blueprint.size() > 2 && (blueprint[2] == 'b' || blueprint[2] == 'B');
    return !building && !u.dead && RideWaves(model, position, orientation);
  };
  effects.SetBuild(&build);
  effects.SetExhausts(&exhausts);  // (unitsfx)
  tracks.deep = &trackField;  // (deep tracks: the prints go there while they are on)
  trackField.picture = [this](const std::string& name) { return decals.Picture(name); };
  trackField.groundHeight = [this](float x, float z) { return map.Height(x, z); };
  if (!options.displayTable.empty()) {
    const int levels = models.LoadDisplayTable(options.displayTable);
    if (levels) printf("render: %d model levels named by the simulation's blueprints (%s)\n", levels, options.displayTable.c_str());
    else fprintf(stderr, "render: display table %s: no DISPLAY lines found\n", options.displayTable.c_str());
  }
  if (!mapPath.empty()) {
    auto file = vfs_->ReadFile(mapPath);
    if (!file) {
      *error = "map " + mapPath + ": file not found";
      return false;
    }
    std::string why;
    if (!LoadMap(*file, &map, &why)) {
      *error = "map " + mapPath + ": " + why;
      return false;
    }
    // [mapfx] the map's own looping effects, where it has them (World::MapEffect)
    mapEffects_.clear();
    if (mapPath.size() > 6 && mapPath.compare(mapPath.size() - 6, 6, ".scmap") == 0) {
      const std::string fxPath = mapPath.substr(0, mapPath.size() - 6) + "_fx.json";
      if (auto text = vfs_->ReadFile(fxPath)) {
        JsonValue list;
        std::string why;
        if (!ReadJson(*text, &list, &why)) {
          fprintf(stderr, "render: %s: %s (not played)\n", fxPath.c_str(), why.c_str());
        } else {
          for (const JsonValue& e : list.items) {
            MapEffect fx;
            fx.emitter = e["emitter"].StringOr("");
            const JsonValue& at = e["at"];
            if (fx.emitter.empty() || at.items.size() < 3) continue;
            fx.at = {static_cast<float>(at.items[0].NumberOr(0)), static_cast<float>(at.items[1].NumberOr(0)), static_cast<float>(at.items[2].NumberOr(0))};
            const float turn = static_cast<float>(e["turn"].NumberOr(0)) * 3.14159265f / 180;
            fx.turn = {0, std::sin(turn * 0.5f), 0, std::cos(turn * 0.5f)};
            fx.scale = static_cast<float>(e["scale"].NumberOr(1));
            mapEffects_.push_back(fx);
          }
          printf("render: %zu looping effects of the map from %s\n", mapEffects_.size(), fxPath.c_str());
        }
      }
    }
    // the map's render-only props, where it has them (scmap.h, LoadDecor): after its own, so the simulation's prop numbers
    // still name the same ones
    decorFrom_ = map.props.size();  // [underfoot] (those after the .scmap's own are the render-only ones)
    if (mapPath.size() > 6 && mapPath.compare(mapPath.size() - 6, 6, ".scmap") == 0) {
      const std::string decorPath = mapPath.substr(0, mapPath.size() - 6) + "_decor.bin";
      if (auto decor = vfs_->ReadFile(decorPath)) {
        const size_t before = map.props.size();
        if (LoadDecor(*decor, &map.props, &why))
          printf("render: %zu render-only props from %s\n", map.props.size() - before, decorPath.c_str());
        else
          fprintf(stderr, "render: %s: %s (not drawn)\n", decorPath.c_str(), why.c_str());
      }
    }
    step("map file");
    terrain.mapPath = mapPath;  // (by which the remastered ground's remap.json is looked up)
    terrain.season = season;    // [seasons] (its materials, cover and mud read for it; its props below)
    seasonOpen_ = season;
    if (!terrain.Load(&map, &why)) {
      *error = "terrain: " + why;
      return false;
    }
    if (options.lodPixels > 0) terrain.quadPixels = options.lodPixels;
    distantLand.Forget(map, mapPath);  // (beyondland: read from this map as opened; the land far off made when it is wanted)
    if (surroundAhead_.valid()) surroundAhead_.get();  // [surahead] (the map before's, never taken: let go of first)
    surround.Forget();                 // (surround: looked for when it is first wanted)
    surroundTried_ = false;
    ForgetSurroundScenery();
    // [surahead] the surround read ahead, on a thread of its own beside the rest of the load: the files' reading and
    // unpacking, the join to the map's edge, and the map's upper layer by its layers. It reads the map as loaded and
    // the ground's layers as Load left them, and writes the surround alone, which nothing else touches until it is taken.
    if (surroundAhead && mapSurround && mapPath.size() > 6 && mapPath.compare(mapPath.size() - 6, 6, ".scmap") == 0)
      surroundAhead_ = std::async(std::launch::async, [this, path = mapPath] {
        const auto started = std::chrono::steady_clock::now();
        SurroundAhead r;
        const std::string name = path.substr(0, path.size() - 6) + "_surround.json";
        if (auto json = vfs_->ReadFile(name)) {
          r.found = true;
          const std::string folder = path.substr(0, path.find_last_of('/') + 1);
          std::string why;
          if (!surround.Open(map, *json, [&](const std::string& file) { return vfs_->ReadFile(folder + file); }, &why)) {
            fprintf(stderr, "render: %s: %s (beyond the edge the map laid out again is drawn)\n", name.c_str(), why.c_str());
          } else if (surround.HasMasks() && !map.albedo[9].texture.empty()) {
            if (const auto upper = vfs_->ReadFile(moho::Vfs::Normalize(map.albedo[9].texture))) r.upper = terrain.UpperByLayer(r.upperOf, &*upper);
          }
        }
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return r;
      });
    step("ground");
    if (!water.Load(&map, &why)) {
      *error = "water: " + why;
      return false;
    }
    // [groundfix] the surf on as the map opens: its shore field begun now on a thread of its own (the rest of the map
    // loading meanwhile), not the first frame
    if (surf && look == Look::Modern && map.water.present) water.surf.StartField();
    haveMap_ = true;
    volumes.Clear();  // (volume explosions: none of the map before)
    temporalBefore_ = false;  // (the temporal upscaler: nothing of the map before is used)
    raysGround_ = false;  // (the ray tracing scene's ground: made again when it is next wanted)
    shadows.Forget();     // [cardcost] (no far cascade of the map before is kept)
    tracks.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));
    trackField.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));
    trackField.waterLevel = map.water.present ? map.water.elevation : -1e9f;
    plantLife.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));  // [plants]
    printf("render: map %s: %dx%d, look %s, %zu props, %zu decals\n", mapPath.c_str(), map.width, map.height, map.shader.c_str(), map.props.size(),
           map.decals.size());
    {  // the ground's lowest and highest (for what the map lies in)
      float low = 1e30f, high = -1e30f;
      for (int z = 0; z <= map.height; z += 2)
        for (int x = 0; x <= map.width; x += 2) {
          const float h = map.Height(static_cast<float>(x), static_cast<float>(z));
          low = std::min(low, h);
          high = std::max(high, h);
        }
      groundMiddle_ = (low + high) * 0.5f;
      ranges.low = low;
      ranges.high = high;
      // where mist lies: on the water, or on the lowest twentieth of a dry map's ground; it thins
      // over a fifth of the way up to the high ground (the height nine tenths of the map lie under)
      std::vector<float> heights;
      for (int z = 0; z <= map.height; z += 4)
        for (int x = 0; x <= map.width; x += 4) heights.push_back(map.Height(static_cast<float>(x), static_cast<float>(z)));
      std::sort(heights.begin(), heights.end());
      mistFloor_ = map.water.present ? map.water.elevation : heights[heights.size() / 20];
      mistDepth_ = std::clamp((heights[heights.size() * 9 / 10] - mistFloor_) * 0.2f, 1.5f, 12.0f);
    }
    if (map.water.present)
      particles.SetWater(map.water.elevation, [this](float x, float z) {
        return map.Height(Clamp(x, 0, static_cast<float>(map.width)), Clamp(z, 0, static_cast<float>(map.height)));
      });
    if (map.water.present) decals.SetWater(terrain.WaterMap(), terrain.WaterRamp(), static_cast<float>(map.width), static_cast<float>(map.height));
    decals.waterLevel = map.water.present ? map.water.elevation : -1e9f;
    decals.SetGround(terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width), static_cast<float>(map.height));  // [steep]
    decals.groundHeight = [this](float x, float z) { return map.Height(Clamp(x, 0, static_cast<float>(map.width)), Clamp(z, 0, static_cast<float>(map.height))); };
    // [iconzoom] (the symbols come in by the zoom of the ground or the water under a unit, as the original's)
    icons.groundHeight = [this](float x, float z) {
      const float h = map.Height(Clamp(x, 0, static_cast<float>(map.width)), Clamp(z, 0, static_cast<float>(map.height)));
      return map.water.present ? std::max(h, map.water.elevation) : h;
    };
    // (the blasts and the debris: on the ground and the water of this map)
    blasts.Clear();
    debris.Clear();
    rocks.Begin(0);  // [rockhits] (this map's rocks found the first time they are wanted: MakeRocks)
    blasts.waterLevel = debris.waterLevel = decals.waterLevel;
    blasts.groundHeight = debris.groundHeight = decals.groundHeight;
    debris.mapWidth = static_cast<float>(map.width);
    debris.mapHeight = static_cast<float>(map.height);
    // (movement dust: on this map's ground and water; where its ground is dusty worked out the first frame it is on)
    dust.waterLevel = decals.waterLevel;
    dust.groundHeight = decals.groundHeight;
    dust.SetMap(&map, mapPath, [this](const std::string& path) { return textures.ReadFile(path); });
    wear.ForgetGround();  // [unit dirt] (the ground's kinds under the units: worked out again the first frame it is on)
    // the map's own decals: albedo ones (pictures over the ground's colour) and normal ones (its sculpting)
    decals.hotMetal = false;  // (hot metal: the map's own scorches are no fresh marks; Begin sets it again)
    const std::string mapDecals = !options.mapDecals.empty() ? options.mapDecals
                                  : !terrain.MapDecalsGiven().empty() ? terrain.MapDecalsGiven() : std::string("normals");
    if (mapDecals != "off") {
      int laid = 0;
      for (const MapDecal& d : map.decals) {
        if (d.type != 1 && d.type != 2) continue;
        if (!options.decalFilter.empty() && d.textures[0].find(options.decalFilter) == std::string::npos) continue;
        if ((mapDecals == "albedo" && d.type != 1) || (mapDecals == "normals" && d.type != 2)) continue;  // one kind alone
        const float heading = d.rotation.y * options.decalTurn;
        Vec3 centre = d.position;
        if (mapDecals == "corner") {  // the position as the corner the decal is turned about
          const float c = std::cos(heading), s = std::sin(heading), hx = d.scale.x * 0.5f, hz = d.scale.z * 0.5f;
          centre.x += hx * c + hz * s;
          centre.z += -hx * s + hz * c;
        }
        centre.y = map.Height(Clamp(centre.x, 0, static_cast<float>(map.width)), Clamp(centre.z, 0, static_cast<float>(map.height)));
        const bool ok = d.type == 2 ? decals.AddNormal(d.textures[0], centre, d.scale.x, d.scale.z, heading, d.cutoffLod * options.decalRange)
                                    : decals.Add(d.textures[0], centre, d.scale.x, d.scale.z, heading, d.cutoffLod * options.decalRange);
        if (ok && d.nearCutoffLod > 0) decals.SetNearCutoff(decals.LastAdded(), d.nearCutoffLod * options.decalRange);  // [decalfade]
        if (ok) ++laid;
      }
      printf("render: %d of the map's %zu decals laid\n", laid, map.decals.size());
    }
    for (const auto& cube : map.environmentCubemaps)
      if (cube.first == "<default>") environment_ = textures.Get(cube.second);
    step("water, decals, mist");
    // the map's props (trees, rocks, wrecks): where each stands does not change
    // ([seasons] each drawn by the season's blueprint where the season lists one: SeasonLook::Prop; the simulation keeps
    // the map's own)
    const SeasonLook& seasonal = terrain.SeasonOf();
    {  // (their models' files read on the worker threads first, all together)
      std::vector<std::string> kinds;
      std::unordered_set<std::string> seen;
      for (const MapProp& p : map.props)
        if (seen.insert(p.blueprint).second) kinds.push_back(seasonal.Prop(p.blueprint));
      models.PrefetchBlueprints(kinds);
    }
    step("props' files read");
    std::unordered_map<std::string, const Model*> kindModels;  // (each kind looked up once, not once a prop)
    int lifted = 0;
    for (const MapProp& p : map.props) {
      auto known = kindModels.find(p.blueprint);
      const Model* model = known != kindModels.end() ? known->second : kindModels.emplace(p.blueprint, models.Blueprint(seasonal.Prop(p.blueprint))).first->second;
      if (!model->ok) {  // (kept in the list all the same: the simulation names a prop by its place in it)
        props_.push_back({nullptr, Mat4::Identity()});
        continue;
      }
      // [proplift] (Options::propsAsGame) where the game has it: the props' script lifts a prop stored below the ground onto it as the game starts
      // (/lua/sim/Prop.lua OnCreate, "terrain correction": below GetTerrainHeight it is warped up to it), so a map drawn
      // without a game shows its props as every game does (a game's own move comes as the simulation's change all the same).
      // Only one sunk by more than a twentieth of a unit: the editor stands props on the ground, and the last bits of its
      // height and ours differ
      float y = p.position.y;
      const float ground = map.Height(Clamp(p.position.x, 0, static_cast<float>(map.width)), Clamp(p.position.z, 0, static_cast<float>(map.height)));
      if (options.propsAsGame && y < ground - 0.05f) {
        y = ground;
        ++lifted;
      }
      const Vec3 s = p.scale * model->scale;
      Mat4 world = {{{p.axisX.x * s.x, p.axisY.x * s.y, p.axisZ.x * s.z, p.position.x},
                     {p.axisX.y * s.x, p.axisY.y * s.y, p.axisZ.y * s.z, y},
                     {p.axisX.z * s.x, p.axisY.z * s.y, p.axisZ.z * s.z, p.position.z},
                     {0, 0, 0, 1}}};
      props_.push_back({model, world});
    }
    if (lifted > 0) printf("render: %d of the map's props stored below the ground, drawn on it as the game lifts them (Prop.lua)\n", lifted);
    step("props' models made");
    {  // [stream] the map's streams (stream_water.h): the props whose levels are a stream's water or its white water
      streams.Forget();
      // [stream mouth] (the sea and the ground, for carrying a stream that ends just above the sea on into it)
      streams.seaLevel = map.water.present ? map.water.elevation : -1e9f;
      streams.groundHeight = [this](float x, float z) { return map.Height(Clamp(x, 0, static_cast<float>(map.width)), Clamp(z, 0, static_cast<float>(map.height))); };
      grass.bareMap.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));  // [cover] (no cover on them)
      grass.SetHeights(map.heights, map.width, map.height, map.heightScale, map.water.present ? map.water.elevation : -1e9f);  // [plantlod]
      for (size_t i = 0; i < props_.size(); ++i)
        if (props_[i].model && !props_[i].model->lods.empty() && props_[i].model->lods[0].stream) {
          streams.Add(props_[i].model, props_[i].world, static_cast<uint32_t>(i));
          grass.bareMap.AddStream(props_[i].model, props_[i].world, &textures);  // [cover]
        }
      if (streams.Any()) printf("render: %d stream ribbons, %d of white water (World::streamWater)\n", streams.Count(false), streams.Count(true));
    }
    // ... listed by squares of the map
    propCellsX_ = std::max(1, static_cast<int>(std::ceil(static_cast<float>(map.width) / kPropCell)));
    propCellsZ_ = std::max(1, static_cast<int>(std::ceil(static_cast<float>(map.height) / kPropCell)));
    propCells_.assign(static_cast<size_t>(propCellsX_) * static_cast<size_t>(propCellsZ_), {});
    clearings_.clear();  // [underfoot] (nothing taken from the new map's squares yet)
    decorUnder_.clear();
    for (size_t i = 0; i < props_.size(); ++i) ListProp(&props_[i], static_cast<uint32_t>(i), false);
    textures.DropFetched();
    step("props listed by squares");
    {  // wildlife (wildlife.h): where the trees stand (each its place and its top over it), for the birds' woods and the fireflies
      std::vector<Vec4> trees;
      for (const PlacedProp& p : props_) {
        if (!p.model || p.model->lods.empty() || p.model->lods[0].sway <= 0) continue;
        const float scale = std::sqrt(p.world.m[0][1] * p.world.m[0][1] + p.world.m[1][1] * p.world.m[1][1] + p.world.m[2][1] * p.world.m[2][1]);
        if ((p.model->boundsMax.y - p.model->boundsMin.y) * scale < 1.0f) continue;  // (a tree, not the undergrowth)
        trees.push_back({p.world.m[0][3], p.world.m[1][3], p.world.m[2][3], p.model->boundsMax.y * scale});
      }
      wildlifeDraw.SetMap(&map, trees);
      sunlight.SetWoods(trees, static_cast<float>(map.width), static_cast<float>(map.height));  // [sunlight] (where the woods are)
    }
    {  // [seasons] the trees the season's leaves fall from: in autumn, on a map with an autumn look, those the season draws
       // in its own look and the broadleaf trees by their names (seasons.h)
      std::vector<FallingLeaves::Tree> shedding;
      if (season == Season::Autumn && seasonal.listed)
        for (size_t i = 0; i < props_.size() && i < map.props.size(); ++i) {
          const PlacedProp& p = props_[i];
          if (!p.model || p.model->lods.empty() || p.model->lods[0].sway <= 0) continue;
          if (seasonal.Prop(map.props[i].blueprint) == map.props[i].blueprint && !Broadleaf(map.props[i].blueprint)) continue;
          const Vec3 up{p.world.m[0][1], p.world.m[1][1], p.world.m[2][1]}, side{p.world.m[0][0], p.world.m[1][0], p.world.m[2][0]};
          const float scale = Length(up), across = Length(side);
          const float height = (p.model->boundsMax.y - p.model->boundsMin.y) * scale;
          if (height < 1.5f) continue;  // (a tree, not the undergrowth)
          FallingLeaves::Tree t;
          t.foot = {p.world.m[0][3], p.world.m[1][3], p.world.m[2][3]};
          t.height = p.model->boundsMax.y * scale;
          t.crown = 0.5f * std::max(p.model->boundsMax.x - p.model->boundsMin.x, p.model->boundsMax.z - p.model->boundsMin.z) * across;
          t.low = 0.4f;
          t.key = static_cast<uint32_t>(i);
          shedding.push_back(t);
        }
      leaves.SetTrees(std::move(shedding), static_cast<float>(map.width), static_cast<float>(map.height), map.water.present ? map.water.elevation : -1e9f);
    }
    // the plants of the map's ground cover, where the visuals track has made them for it (its card atlas and table),
    // and where its trees stand (within the table's radius of each: more ferns and clumps, fewer flowers)
    plantsReady_ = false;
    if (treeMask_) device_->Destroy(treeMask_);
    treeMask_ = {};
    if (snowPropsAhead_.valid()) snowPropsAhead_.get();  // [snowhitch] (the last map's, being worked out ahead: let go of)
    if (snowProps_) device_->Destroy(snowProps_);  // [snow ground] (made again for this map's props when snow lies)
    snowProps_ = {};
    snowPropsMade_ = false;
    coverA_ = coverB_ = {};
    if (const JsonValue* cover = terrain.Cover()) {
      plantsReady_ = grass.SetPlants(*cover, &textures, kRemasteredTerrain);
      if (plantsReady_) grass.SetPlantModels(*cover, &textures, kRemasteredTerrain);  // [plantlod] (the cover's 3D plants)
      const int w = std::max(map.width / 2, 1), h = std::max(map.height / 2, 1);  // (as the density's pictures)
      std::vector<uint8_t> near(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
      const float radius = terrain.CoverTreeRadius(), texel = static_cast<float>(map.width) / static_cast<float>(w);
      const int reach = static_cast<int>(std::ceil(radius / texel));
      int trees = 0;
      for (const PlacedProp& p : props_) {
        if (!p.model || p.model->lods.empty() || p.model->lods[0].sway <= 0) continue;
        const float scale = std::sqrt(p.world.m[0][0] * p.world.m[0][0] + p.world.m[1][0] * p.world.m[1][0] + p.world.m[2][0] * p.world.m[2][0]);
        if ((p.model->boundsMax.y - p.model->boundsMin.y) * scale < 1.5f) continue;  // (a tree, not the undergrowth)
        ++trees;
        const float x = p.world.m[0][3] / texel, z = p.world.m[2][3] / texel;
        for (int dz = -reach; dz <= reach; ++dz)
          for (int dx = -reach; dx <= reach; ++dx) {
            const int tx = static_cast<int>(x) + dx, tz = static_cast<int>(z) + dz;
            if (tx < 0 || tz < 0 || tx >= w || tz >= h) continue;
            const float ex = (static_cast<float>(tx) + 0.5f - x) * texel, ez = (static_cast<float>(tz) + 0.5f - z) * texel;
            if (ex * ex + ez * ez <= radius * radius) near[static_cast<size_t>(tz) * static_cast<size_t>(w) + static_cast<size_t>(tx)] = 255;
          }
      }
      gfx::TextureDesc td;
      td.width = w;
      td.height = h;
      td.format = gfx::Format::R8;
      td.data = near.data();
      td.dataSize = near.size();
      treeMask_ = device_->CreateTexture(td);
      if (plantsReady_) printf("render: the ground cover's plants: %d trees near which they change\n", trees);
    }
  }
  if (haveMap_) {  // [restart] (the map as opened, for RestartGame)
    heightsAtOpen_ = map.heights;
    propsAtOpen_ = props_;
    decalsAtOpen_ = decals.LastAdded();
    depositsAtOpen_ = decals.Deposits().size();
  }
  sky_ = textures.SkyCube(haveMap_ ? map.skyCubemap : std::string());
  {  // the ring under a selected unit: a thin white circle, soft at both edges
    const int size = 128;
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4, 255);
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const float dx = (static_cast<float>(x) + 0.5f) / size * 2 - 1, dy = (static_cast<float>(y) + 0.5f) / size * 2 - 1;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float a = Clamp((r - 0.86f) / 0.03f, 0, 1) * Clamp((0.985f - r) / 0.03f, 0, 1);
        pixels[(static_cast<size_t>(y) * size + static_cast<size_t>(x)) * 4 + 3] = static_cast<uint8_t>(a * 255.0f);
      }
    gfx::TextureDesc td;
    td.width = td.height = size;
    td.format = gfx::Format::RGBA8;
    td.data = pixels.data();
    td.dataSize = pixels.size();
    selectionRing_ = device_->CreateTexture(td);
    LoadBrackets();
  }
  step("sky and the rest");
  batch.open = false;
  if (!device_->EndPipelineBatch(error)) {
    *error = "shader: " + *error;
    return false;
  }
  step("shaders compiled");
  // [energy shields] on as the map opens: its shaders made now, not at the first bubble in play (after the batch, so that
  // where they cannot be made the map still opens, with the plain bubbles)
  if (energyShields) energy.Prepare();
  fprintf(stderr, "render: opened in steps: %s\n", steps.c_str());
  return true;
}

// [restart] (world.h) What a game added let go of, part by part, and the map put back as Open left it. A new game's clock
// begins again near 0, so what waits for a moment of the game (events, hits, props' changes, flashes) would otherwise wait
// or linger; and its entities' numbers and its blueprints' places in the frames are used again, so what is kept by them is
// forgotten.
void World::RestartGame() {
  if (!haveMap_) return;
  // the ground: the heights as the map was opened (the simulation laid some flat under its structures: ChangeGround), drawn
  // so by the next Begin (the height texture is written within a frame); the ray tracing scene's ground made again from them
  if (heightsAtOpen_.size() == map.heights.size()) {
    map.heights = heightsAtOpen_;
    heightsRestored_ = true;
  }
  raysGround_ = false;
  // the props: the map's as opened, each listed in its square again (each square's group kept, filled anew); nothing made
  // in the game (wreckage), nothing taken away under a structure; the snow's raster of them made again when it is wanted
  if (snowPropsAhead_.valid()) snowPropsAhead_.get();
  if (snowProps_) device_->Destroy(snowProps_);
  snowProps_ = {};
  snowPropsMade_ = false;
  props_ = propsAtOpen_;
  madeProps_.clear();
  pileProps_.clear();  // [debrispile]
  for (PropCell& cell : propCells_) {
    cell.ofMap.clear();
    cell.made.clear();
    cell.underfoot.clear();
    cell.changed = true;
  }
  for (size_t i = 0; i < props_.size(); ++i) ListProp(&props_[i], static_cast<uint32_t>(i), false);
  clearings_.clear();
  decorUnder_.clear();
  propEvents_.clear();
  propsKept_.clear();  // [devicelost] (the new game's own from here; its looks come with its SetLooks)
  looksKept_.clear();
  recentProps_.clear();
  recentPruned_ = 0;
  propBeats_.clear();
  hotProps_.clear();
  hotOn_ = hotBefore_ = false;
  rocks.Forget();  // [rockhits] (the rocks unscarred, each back in its group: the squares are filled anew above)
  rockHitsNow_.clear();
  rockShotsStopped_.clear();
  rockShotsBefore_.clear();
  // the game's hits not yet shown, and what was kept of its units by their numbers
  combatPending_ = FrameCombat{};
  combat_ = FrameCombat{};
  hitsTaken = impactsTaken = hitsShown = impactsShown = propsChanged = propsGone = groundChanges = 0;
  shieldHits_.clear();
  warpSince_.clear();  // [warpin]
  shellHits_.clear();
  impactBorn_.clear();
  impactBornNext_.clear();
  moving_.clear();
  hullsSeen_.clear();
  deaths_.clear();  // [fogglow] (the new game hands the same unit numbers out again: none of them has died in it)
  deathsTold_.clear();
  leftSlabs_.clear();
  buildingNow_.clear();
  remembered_.clear();
  rememberedFor_ = -1;
  rememberedGone.clear();
  pickables_.clear();
  // what was looked up by the blueprints' places in the frames (SetLooks empties six; the new game's first looks come later)
  iconKinds_.clear();
  unitModels_.clear();
  shotModels_.clear();
  entityModels_.clear();
  originalShotModels_.clear();
  originalEntityModels_.clear();
  structureKind_.clear();
  selections_.clear();
  shotsByUnit_.clear();
  // the lamps' levels as they faded
  lampLevels_.clear();
  lampClock_ = -1;
  lampUnits_ = lampUnitsBefore_ = 0;
  lampUnitsSmooth_ = 0;
  nightShown_.clear();
  nightShownBefore_.clear();
  // the clock and what the weather has done to surfaces; what keeps frames before (the temporal upscaler, the light in the
  // air, the clouds) starts afresh with the next frame
  clock_ = 0;
  time_ = 0;
  weatherTime_ = -1;
  wet = snowLying = 0;
  tracksCovering_ = 0;
  lastWeatherKnown_ = false;
  stormSunNow_ = 0;
  weatherCut = cameraCut = true;
  // the parts (the debris first: its chunks' smoke trails are ribbons; the hot wrecks' smoke and the machinery's emitters
  // before the particles go)
  debris.Clear();
  heat.Off(&particles);
  machinery.Forget();
  effects.Clear();
  particles.Clear();  // (the map's looping effects started again by the next Render: KeepMapEffects)
  ribbons.Clear();
  decals.ForgetGame(decalsAtOpen_, depositsAtOpen_);
  lights.Forget();
  blasts.Clear();
  volumes.Clear();
  dust.Clear();
  build.Begin(0, false);
  wear.Forget();
  tracks.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));
  tracks.Forget();
  trackField.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));
  plantLife.SetMap(static_cast<float>(map.width), static_cast<float>(map.height));
  water.ripples.SetMap(&map);
  water.surf.Forget();
  sight.Set({}, 0);
  shadows.Forget();
  // the interface's lists, as for a map just opened
  selected.clear();
  hover = 0xffffffffu;
  hoverKind = 0;
  ghosts.clear();
  lines.clear();
  rings.clear();
  userDecals.clear();
  placed.clear();
  orders.legs.clear();
  orders.marks.clear();
  ranges.groups.clear();
  std::fill(std::begin(playable), std::end(playable), 0.0f);
  barAnchors.clear();
  dyingDrawn = 0;
}

// [devicelost] (world.h)
void World::Keep(Kept* out, uint32_t tick) const {
  *out = Kept{};
  if (!haveMap_) return;
  out->valid = true;
  out->tick = tick;
  out->looks = looksKept_;
  // (the ground only where the game changed it: else the map's own, which the world made anew has)
  if (heightsAtOpen_.size() == map.heights.size() && map.heights != heightsAtOpen_) out->heights = map.heights;
  out->props.reserve(propsKept_.size());
  for (const auto& [key, event] : propsKept_) {
    Kept::Prop p;
    p.event = event;
    if (event.mapIndex < 0)
      if (auto made = madeProps_.find(event.id); made != madeProps_.end()) p.glowKnown = made->second.glowKnown;
    out->props.push_back(std::move(p));
  }
  std::sort(out->props.begin(), out->props.end(), [](const Kept::Prop& a, const Kept::Prop& b) {
    return a.event.tick != b.event.tick ? a.event.tick < b.event.tick : a.event.id < b.event.id;
  });
  out->propsPending.assign(propEvents_.begin(), propEvents_.end());
  effects.Keep(&out->effects, tick);
  decals.KeepScars(static_cast<double>(tick) * 0.1, &out->scars);  // [settled]
  out->wet = wet;
  out->snowLying = snowLying;
  out->selected = selected;
  out->hover = hover;
  out->hoverKind = hoverKind;
  out->ghosts = ghosts;
  out->lines = lines;
  out->rings = rings;
  out->userDecals = userDecals;
  out->placed = placed;
  out->borders = borders;
  out->orderTextures = orders.textures;
  out->orderLegs = orders.legs;
  out->orderMarks = orders.marks;
  out->orderNumbers[0] = orders.lineScale;
  out->orderNumbers[1] = orders.curveSmoothness;
  out->orderNumbers[2] = orders.markMinPixels;
  out->orderNumbers[3] = orders.markMaxPixels;
  out->orderSegments = orders.curveSegments;
  out->rangeGroups = ranges.groups;
  out->rangeZoom[0] = ranges.zoomNear;
  out->rangeZoom[1] = ranges.zoomFar;
  std::copy(std::begin(playable), std::end(playable), out->playable);
  out->iconColors = iconColors;
  out->lifeBars = lifeBars;
  out->workBars = workBars;
  out->unexplored = unexplored;
  out->iconsOn = iconsOn;
  out->selectionMarks = selectionMarks;
}

void World::Give(const Kept& kept) {
  if (!haveMap_ || !kept.valid) return;
  if (!kept.looks.empty()) SetLooks(kept.looks);
  // the ground as the game left it (drawn so by the next Begin, as RestartGame's; the ray tracing scene's ground from it)
  if (kept.heights.size() == map.heights.size() && kept.heights != map.heights) {
    map.heights = kept.heights;
    heightsRestored_ = true;
    raysGround_ = false;
  }
  // the scenery: each prop as the simulation last had it, laid at once (not at its beat: those are long past); a wreck
  // keeps the beat it was made at, by which it cools
  for (const Kept::Prop& p : kept.props) {
    ApplyPropEvent(p.event);
    if (p.event.mapIndex < 0 && p.event.kind != FramePropEvent::Destroy)
      if (auto made = madeProps_.find(p.event.id); made != madeProps_.end()) made->second.glowKnown = p.glowKnown;
  }
  recentProps_.clear();  // ([scars] none of them is of this moment: a flash on one is judged as on any other thing)
  recentPruned_ = 0;
  propsChanged = propsGone = 0;  // (counted since the map was opened: the game's own changes from here)
  for (const FramePropEvent& e : kept.propsPending) propEvents_.push_back(e);
  effects.Queue(std::vector<FrameEffectEvent>(kept.effects));
  // [settled] (the battle scars past their game's time, settled or weathering: laid as they were with the first frame)
  decals.GiveScars(kept.scars);
  wet = kept.wet;
  snowLying = kept.snowLying;
  selected = kept.selected;
  hover = kept.hover;
  hoverKind = kept.hoverKind;
  ghosts = kept.ghosts;
  lines = kept.lines;
  rings = kept.rings;
  userDecals = kept.userDecals;
  placed = kept.placed;
  borders = kept.borders;
  orders.textures = kept.orderTextures;
  orders.legs = kept.orderLegs;
  orders.marks = kept.orderMarks;
  orders.lineScale = kept.orderNumbers[0];
  orders.curveSmoothness = kept.orderNumbers[1];
  orders.markMinPixels = kept.orderNumbers[2];
  orders.markMaxPixels = kept.orderNumbers[3];
  orders.curveSegments = kept.orderSegments;
  ranges.groups = kept.rangeGroups;
  ranges.zoomNear = kept.rangeZoom[0];
  ranges.zoomFar = kept.rangeZoom[1];
  std::copy(std::begin(kept.playable), std::end(kept.playable), playable);
  iconColors = kept.iconColors;
  lifeBars = kept.lifeBars;
  workBars = kept.workBars;
  unexplored = kept.unexplored;
  iconsOn = kept.iconsOn;
  selectionMarks = kept.selectionMarks;
}

void World::SetLooks(const std::vector<BlueprintLook>& looks) {
  if (&looks != &looksKept_) looksKept_.insert(looksKept_.end(), looks.begin(), looks.end());  // [devicelost] (Keep)
  iconKinds_.clear();  // (what was looked up before may look different now)
  unitModels_.clear();
  shotModels_.clear();
  entityModels_.clear();
  originalShotModels_.clear();
  originalEntityModels_.clear();
  int units = 0;
  for (const BlueprintLook& look : looks) {
    models.SetLook(look);
    if (look.kind != 0) continue;
    icons.Define(models.UnitBlueprintPath(look.id), look.icon, look.iconPriority, look.iconFadeIn, look.selectionX, look.selectionZ, look.lifeBarSize,
                 look.lifeBarHeight, look.lifeBarOffset, look.selectionThickness, look.selectionOffset);
    ++units;
  }
  if (!looks.empty()) printf("render: %zu blueprints' looks from the simulation (%d of them units)\n", looks.size(), units);
  // [spikes] (readAhead) a game has begun: the pictures its battles will want made ahead, after the armies' own kinds (what
  // they build is asked for each frame from now, MakeAhead): the visuals track's scorches and craters and its particles'
  // pictures, where the remastered effects are on, and the game's particles' pictures that have no remastered twin (the
  // original effects: all of them). Each made as its first use would make it; they hold about a third of a gigabyte of the
  // card with the remastered effects. (Measured on SETONS_R, Chris's mods: 37 + 82 + 745 files.)
  if (!looks.empty() && readAhead && haveMap_ && !effectsAhead_) {
    effectsAhead_ = true;
    const bool remastered = effectsLook == EffectsLook::Remastered;
    std::vector<std::string> files;
    if (remastered)
      for (const char* folder : {"/remastered/env/common/decals", "/remastered/textures/particles"})
        for (std::string& f : vfs_->FindFiles(folder, "*.dds")) files.push_back(std::move(f));
    for (std::string& f : vfs_->FindFiles("/textures/particles", "*.dds"))
      if (!remastered || EffectFile(*vfs_, f, true) == f) files.push_back(std::move(f));  // (no remastered twin)
    // (and before the armies' kinds, the game's own pads' pictures, laid under the first buildings: a few megabytes)
    std::vector<std::string> pads = vfs_->FindFiles("/env/common/decals/tarmacs", "*.dds");
    printf("render: %zu of the effects' pictures and %zu of the pads' to be made ahead\n", files.size(), pads.size());
    // (asked for once the game's first frames are over, AheadBegun: those make the map's own props and units, and the
    // threads making ahead slowed them)
    aheadPads_ = std::move(pads);
    aheadPictures_ = std::move(files);
    aheadFrames_ = 0;
  }
}

// [spikes] (PrepareAhead) the game's first frames over: what was asked for meanwhile asked for now; true from then on
bool World::AheadBegun() {
  if (!effectsAhead_ || aheadFrames_ >= kAheadAfterFrames) return true;
  if (++aheadFrames_ < kAheadAfterFrames) return false;
  if (aheadRoom_) {  // (no room on the card: left to their first use, as before)
    textures.MakeAhead(aheadPads_);
    models.MakeAhead(aheadKinds_);
    textures.MakeAhead(aheadPictures_, true);
  }
  aheadPads_.clear();
  aheadKinds_.clear();
  aheadPictures_.clear();
  return true;
}

// [session] (world.h)
void World::MakeSessionModels(const std::vector<std::string>& unitIds, const std::vector<std::pair<std::string, std::string>>& props,
                              const std::atomic<bool>* stop) {
  const bool rayed = (rayReflections || rayLightShadows) && RaysReady();  // (their ray-traced structures too, where rays are used)
  for (const std::string& id : unitIds) {
    if (stop && stop->load(std::memory_order_acquire)) return;
    const Model* m = models.Unit(id);
    if (rayed) rays.PrepareModel(m);
  }
  for (const auto& p : props) {
    if (stop && stop->load(std::memory_order_acquire)) return;
    const Model* m = PropModel(p.first, p.second);
    if (rayed) rays.PrepareModel(m);
  }
}

// [spikes] (readAhead) the kinds an army may build, made ahead (ModelCache::MakeAhead)
void World::MakeAhead(const std::vector<std::string>& unitIds) {
  if (!readAhead || !haveMap_ || sidePicture || unitIds.empty()) return;
  if (!aheadRoom_) return;  // (no room on the card: made at its first use, as before)
  if (effectsAhead_ && aheadFrames_ < kAheadAfterFrames) {  // (the game's first frames: asked for after them, AheadBegun)
    aheadKinds_.insert(aheadKinds_.end(), unitIds.begin(), unitIds.end());
    return;
  }
  models.MakeAhead(unitIds);
}

void World::LoadBrackets() {
  static const char* const kFiles[4] = {"player", "player_highlighted", "enemy", "neutral"};
  haveBrackets_ = true;
  for (int k = 0; k < 4; ++k) {
    const std::string path = std::string("/textures/ui/common/game/selection/selection_brackets_") + kFiles[k] + ".dds";
    auto file = vfs_->ReadFile(path);
    DdsImage image;
    std::vector<uint8_t> pixels;
    std::string error;
    if (!file || !ParseDds(*file, &image, &error) || !DecodeRgba(image, 0, &pixels) || image.width < 2 || image.height < 2) {
      haveBrackets_ = false;
      return;
    }
    // the picture holds all four corners: each quarter of it is one
    const int w = image.width / 2, h = image.height / 2;
    std::vector<uint8_t> corner(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int c = 0; c < 4; ++c) {
      const int x0 = (c & 1) * w, y0 = (c >> 1) * h;
      for (int y = 0; y < h; ++y)
        std::memcpy(&corner[static_cast<size_t>(y) * static_cast<size_t>(w) * 4],
                    &pixels[(static_cast<size_t>(y0 + y) * static_cast<size_t>(image.width) + static_cast<size_t>(x0)) * 4], static_cast<size_t>(w) * 4);
      gfx::TextureDesc td;
      td.width = w;
      td.height = h;
      td.format = gfx::Format::RGBA8;
      td.data = corner.data();
      td.dataSize = corner.size();
      brackets_[k][c] = device_->CreateTexture(td);
    }
  }
}

// Four corner pieces about a unit, by the original's rule (docs/engine-answers.md, "render 23"):
// the selection rectangle has half sides SelectionSizeX and Z times 0.75 (without them: the mesh's
// half extents times 1.85), its centre moved by SelectionCenterOffset in the unit's own frame. On
// each of its corners sits, centred, a square piece of side 2 f M, M the larger half side and f the
// blueprint's SelectionThickness (else 0.2); f is raised so that a piece's half side is never under
// 3 pixels (ren_SelectBracketMinPixelSize; the engine's reading says pixels, its unit not read).
// [bracket54] The centre is the middle of the mesh's bounds as placed (x, z; the engine's "render 54": the original adds the
// offset to its mesh instance's world box's centre), not the unit's origin: the UEF commander's mesh lies forward of its
// origin, and its SelectionCenterOffsetZ -0.4 brings the box's middle back over its feet (Chris, 2026-11-04: "the yellow
// squre under the acu isn't centered"). The unit's place without a mesh.
void World::AddBrackets(int kind, Vec3 position, const Quat& orientation, const StrategicIcons::Kind& unit, const Model* model) {
  if (model && model->ok) {
    const Vec3 middle = Rotate(orientation, {(model->boundsMin.x + model->boundsMax.x) * 0.5f * model->scale, 0,
                                             (model->boundsMin.z + model->boundsMax.z) * 0.5f * model->scale});
    position.x += middle.x;
    position.z += middle.z;
  }
  float hx = unit.selectionX * 0.75f, hz = unit.selectionZ * 0.75f;
  if (!unit.selectionGiven && model && model->ok) {
    hx = (model->boundsMax.x - model->boundsMin.x) * 0.5f * model->scale * 1.85f;
    hz = (model->boundsMax.z - model->boundsMin.z) * 0.5f * model->scale * 1.85f;
  }
  const float* o = unit.selectionOffset;
  if (o[0] != 0 || o[1] != 0 || o[2] != 0) position = position + Rotate(orientation, {o[0], o[1], o[2]});
  const float most = std::max(hx, hz);
  if (most <= 0) return;
  float f = std::fabs(unit.selectionThickness) >= 0.00001f ? unit.selectionThickness : 0.2f;
  const Vec3 eye = camera.target + (camera.PixelRay(view.width * 0.5f, view.height * 0.5f, static_cast<float>(view.width), static_cast<float>(view.height)).direction * -camera.distance);
  const float perPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1)) * Length(position - eye);
  f = std::max(f, 3 * perPixel / most);
  const float piece = 2 * f * most;
  const Vec3 forward = Rotate(orientation, {0, 0, 1});
  const float heading = std::atan2(forward.x, forward.z);
  for (int c = 0; c < 4; ++c) {
    // (the quarter of the bracket's picture at (-X, -Z) is u 0..0.5, v 0..0.5; a mark's picture
    // has its top at the unit's back)
    const float sx = (c & 1) ? 1.0f : -1.0f, sz = (c >> 1) ? 1.0f : -1.0f;
    const Vec3 at = position + Rotate(orientation, {sx * hx, 0, sz * hz});
    decals.AddMark(brackets_[kind][c], at, piece, piece, heading);
  }
}

// [lift] What floats rides the waves in the picture alone (ocean.h, Ocean::BobAt; World::oceanLift): a ship or a hover unit
// on the water rises and falls with the long waves under it and pitches and rolls with their slopes. [whitecaps] Its hull,
// shell, outline and lamps (the unit's drawing) and what hangs on its bones (Effects::drawnAt) are moved so. Not a building
// nor a dead unit (the callers say). False: left where the game has it.
// [hulls] A ship sits a little down in the sea (Chris, 2026-10-23: "floating on the water vs in the water"). The game puts a
// ship's place, its model's origin, at the water's level, and the models are made so (the original's water is a flat sheet
// it may see through): most of their hulls reach only a little under the origin (UES0103 0.14 world units, its flat bottom
// 0.09, against a hull side of about 0.43 to the deck), so that on the lifted waves the hull showed its whole side down to
// its bottom edge. Sunk until it draws as much as a hull of its beam does (0.35 of the beam to its keel, as a frigate's or a
// cruiser's: UES0103 0.08 down, UES0201 0.15, UES0302 0.5), never deeper than its hull already reaches (a hull made deep, as
// a battleship's, keeps nearly its own); never a unit above the water's level (a hover unit) nor one below it.
// [hull sit] And never more than half its freeboard: the height of its deck at its sides (Model::deckHeight) over the water
// (Chris's snapshot test of 25 October, item 10: the Seraphim T3 battleship, its deck 0.18 over its origin, was sunk 0.38 -
// its deck awash; "some ships sit too deep, mostly other factions'": the Seraphim's, the Aeon's and Cybran's low hulls).
static float HullSink(const Model* model) {
  const float keel = std::max(-model->boundsMin.y * model->scale, 0.0f);
  const float beam = (model->boundsMax.x - model->boundsMin.x) * model->scale;
  const float freeboard = model->deckHeight < 1e29f ? std::max(model->deckHeight * model->scale, 0.0f) : 1e30f;
  return std::clamp(0.35f * beam - keel, 0.0f, std::min(keel, 0.5f * freeboard));
}

void World::ListHull(uint32_t id, const Model* model, Vec3 position, const Quat& orientation, const Ocean::Bob& rode) {
  // (its speed from where it was the frames before, eased)
  HullSeen& seen = hullsSeen_[id];
  const double dt = clock_ - seen.clock;
  if (dt > 1e-4 && dt < 1.0) seen.speed += (Length(Vec3{position.x - seen.place.x, 0, position.z - seen.place.z}) / static_cast<float>(dt) - seen.speed) * 0.2f;
  else if (dt != 0) seen.speed = 0;
  if (dt != 0) {
    seen.place = position;
    seen.clock = clock_;
  }
  seen.stamp = frameStamp_;
  const Vec3 middle = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
  const Vec3 forward = Rotate(orientation, {0, 0, 1});
  water.ocean.AddHull(middle.x, middle.z, forward.x, forward.z, (model->boundsMax.z - model->boundsMin.z) * 0.46f * model->scale,
                      (model->boundsMax.x - model->boundsMin.x) * 0.46f * model->scale, seen.speed, rode);
  // (those not seen for a while forgotten: looked for once in a while, not for every ship of a large navy)
  if (hullsSeen_.size() > 256 && frameStamp_ - hullsSwept_ >= 120) {
    hullsSwept_ = frameStamp_;
    for (auto it = hullsSeen_.begin(); it != hullsSeen_.end();) it = frameStamp_ - it->second.stamp > 600 ? hullsSeen_.erase(it) : std::next(it);
  }
}

bool World::RideWaves(const Model* model, Vec3* position, Quat* orientation, Ocean::Bob* rode, float* sunk) const {
  if (!model || !model->ok || !water.ocean.Riding() || !haveMap_ || std::fabs(position->y - map.water.elevation) >= 0.5f) return false;
  const Vec3 size = (model->boundsMax - model->boundsMin) * model->scale;
  Ocean::Bob bob;
  if (!water.ocean.BobAt(position->x, position->z, std::max({size.x, size.z, 0.5f}), &bob) || (bob.rise == 0 && bob.slopeX == 0 && bob.slopeZ == 0)) return false;
  // [hulls] (a ship, at the water's level itself: a little down in the sea)
  const float sink = std::fabs(position->y - map.water.elevation) < 0.02f ? HullSink(model) : 0.0f;
  position->y += bob.rise - sink;
  if (rode) *rode = bob;
  if (sunk) *sunk = sink;
  // (turned from straight up to the surface's normal under it)
  const Vec3 n = Normalize(Vec3{-bob.slopeX, 1, -bob.slopeZ});
  const Vec3 axis = Cross(Vec3{0, 1, 0}, n);
  Quat tilt{axis.x, axis.y, axis.z, 1 + n.y};
  const float l = std::sqrt(tilt.x * tilt.x + tilt.y * tilt.y + tilt.z * tilt.z + tilt.w * tilt.w);
  tilt = {tilt.x / l, tilt.y / l, tilt.z / l, tilt.w / l};
  *orientation = Mul(tilt, *orientation);
  return true;
}

void World::MarkOutline(const Model* model, Vec3 position, Vec3 color, bool building, uint32_t id, const StrategicIcons::Kind* kind) {
  if (!outlinesNow_ || !model || !model->ok) return;
  // (giving way to its strategic icon as that comes in, by the icons' own rule, StrategicIcons::Add)
  const float away = Length(position - view.eye);
  float icon = 0;
  if (iconsOn && kind && kind->icon >= 0) icon = icons.ComeIn(kind, position);  // ([iconzoom] as StrategicIcons::Add)
  const float radius = Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale;
  meshes.MarkLast(model, outlines.Mark(color, building, id, outlines.Strength(radius, away, camera.fovY, icon)));
}

// [fogwar] A structure: its blueprint's Physics.MotionType is none (RULEUMT_None, or not given); a blueprint that cannot be
// read goes by the third letter of its id, "ueb0101", the game's own way of naming structures. Asked once per kind.
bool World::IsStructure(uint32_t blueprint, const std::string& id) {
  if (blueprint >= structureKind_.size()) structureKind_.resize(static_cast<size_t>(blueprint) + 1, -1);
  int8_t& kind = structureKind_[blueprint];
  if (kind < 0) {
    BpValue bp;
    if (!id.empty() && dust.readBlueprint && dust.readBlueprint(id, &bp)) {
      const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
      kind = motion.empty() || motion == "RULEUMT_None" ? 1 : 0;
    } else {
      kind = id.size() > 2 && (id[2] == 'b' || id[2] == 'B') ? 1 : 0;
    }
  }
  return kind == 1;
}

void World::AddLamps(const Model* model, Vec3 position, const Quat& orientation, const Pose* pose, const Vec3* armyColor, uint32_t id,
                     bool structure) {
  if (lampsOn_ <= 0 || !model || model->lights.empty()) return;
  // (only near what the camera looks at: a lamp lights a few units of ground; cinecam: a cinematic camera sees far
  // over the ground from low down, its lamps are those round its eye)
  const float lampReach = camera.cinematic ? 300.0f : camera.distance * 2 + 80;
  const float lampAway = camera.cinematic ? Length(position - camera.Eye()) : Length(position - camera.target);
  if (lampAway > lampReach) return;
  // [nightlight] (steadyLamps: faded out over the last tenth of that reach, not cut at it, so that zooming does not
  // switch lamps on and off where the view reaches past it)
  const float byReach = steadyLamps ? std::clamp((lampReach - lampAway) / (0.1f * lampReach), 0.0f, 1.0f) : 1.0f;
  // with many units lit, each lights only the first of its lamps (a list has the lamps that matter
  // most first): about 6,000 lights in all, by the number of units of the frame before
  ++lampUnits_;
  size_t most = static_cast<size_t>(std::clamp(6000 / std::max(lampUnitsBefore_, 1), 3, 16));
  // [nightlight] (steadyLamps: by the units of the frames before eased, and the last lamp lit by the share of it left,
  // so that the lamps a unit lights do not step as units come and go)
  float mostLit = std::clamp(6000.0f / std::max(lampUnitsSmooth_, 1.0f), 3.0f, 16.0f);
  // [lampbudget] (lampsNearFirst) a unit near what the camera looks at all of them (16 at most), whatever the zoom
  if (lampsNearFirst) {
    const float t = std::clamp((lampAway - lampReach / 6) / (0.4f * lampReach - lampReach / 6), 0.0f, 1.0f);
    const float nearShare = 1 - t * t * (3 - 2 * t);
    mostLit += (16 - mostLit) * nearShare;
    most = std::max(most, static_cast<size_t>(std::lround(mostLit)));
  }
  size_t lit = 0;
  uint64_t taken[16];
  bool takenBeam[16];
  size_t takenCount = 0;
  const size_t pendingFrom = pendingLamps_.size();
  const uint32_t pendingUnit = ++pendingUnits_;
  bool litBefore = false;  // (steadyLamps: one of its lamps was lit the frame before)
  // [buildingglow] a structure's glow and lamps by their own rule, while the glow is toned down (hullShare)
  const bool buildingRule = structure && hullBuildings && hullShare;
  for (const ModelLight& light : model->lights) {
    const uint64_t lampKey = id ? (static_cast<uint64_t>(id) << 8 | static_cast<uint64_t>(&light - model->lights.data())) + 1 : 0;
    // (a lamp on a part the script has hidden is not there: a commander's upgrade not bought yet. [lamps hidden] One on
    // no bone rides on the unit itself, its first bone: gone with it where the script has hidden that - a commander
    // before it warps in, and the invisible helper FAF's drones carry (ZXA0003, HideBone(0) on the drone's own mesh at
    // its own scale: the drone's lamps lit a second time, a little above it, Chris's snapshot test of 25 October))
    if (pose) {
      const uint32_t on = light.bone >= 0 ? static_cast<uint32_t>(light.bone) : 0;
      bool hidden = false;
      for (size_t i = 0; pose->now && i < pose->nowCount; ++i)
        if (pose->now[i].bone == on) {
          hidden = pose->now[i].hidden;
          break;
        }
      if (hidden) continue;
    }
    const size_t index = lit++;
    if (steadyLamps ? mostLit - static_cast<float>(index) <= 0 : index >= most) break;
    const float byMost = steadyLamps ? std::min(mostLit - static_cast<float>(index), 1.0f) : 1.0f;
    Vec3 bonePlace{0, 0, 0};
    Quat boneTurn;
    if (light.bone >= 0) {
      const ModelBone& rest = model->bones[static_cast<size_t>(light.bone)];
      auto posed = [&](const FrameBone* bones, uint32_t count, Vec3* p, Quat* q) {
        *p = rest.position * model->scale;
        *q = rest.rotation;
        for (uint32_t i = 0; bones && i < count; ++i)
          if (bones[i].bone == static_cast<uint32_t>(light.bone)) {
            *p = {bones[i].position[0], bones[i].position[1], bones[i].position[2]};
            *q = {bones[i].orientation[0], bones[i].orientation[1], bones[i].orientation[2], bones[i].orientation[3]};
            return;
          }
      };
      Vec3 pa, pb;
      Quat qa, qb;
      posed(pose ? pose->now : nullptr, pose ? pose->nowCount : 0, &pa, &qa);
      posed(pose ? pose->next : nullptr, pose ? pose->nextCount : 0, &pb, &qb);
      const float blend = pose ? pose->blend : 0;
      bonePlace = Lerp(pa, pb, blend);
      boneTurn = Nlerp(qa, qb, blend);
    }
    const Vec3 at = position + Rotate(orientation, bonePlace + Rotate(boneTurn, light.place * model->scale));
    // where units stand close together not every lamp is lit: a small square of ground takes three
    // beams and two other lamps (more would only pile light on light, and a pixel looks at 48 at most)
    // ([nightlight] steadyLamps: decided once every lamp is in, FlushLamps)
    if (!steadyLamps) {
      const uint64_t square = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(at.x / 2.5f)))) << 32) |
                              static_cast<uint32_t>(static_cast<int32_t>(std::floor(at.z / 2.5f)));
      // (by the units before this one: a unit's own lamps do not cut each other. Beams are counted by
      // the UNITS that shine one into the square, not by the beams: the first unit there keeps all
      // its beams, the next three one each, so that in a crowd every unit has its cone and none a
      // whole column goes dark because one engineer's three beams filled the square)
      const auto found = lampSquares_.find(square);
      const uint32_t count = found == lampSquares_.end() ? 0 : found->second;
      const uint32_t beamUnits = count & 0xffffu, others = count >> 16;
      if (light.cone > 0) {
        if (beamUnits >= 4) continue;
        bool ownHere = false;
        for (size_t i = 0; i < takenCount; ++i) ownHere = ownHere || (takenBeam[i] && taken[i] == square);
        if (beamUnits >= 1 && ownHere) continue;
      } else if (others >= 2) {
        continue;
      }
      if (takenCount < 16) {
        taken[takenCount] = square;
        takenBeam[takenCount++] = light.cone > 0;
      }
    }
    float on = lampsOn_ * light.strength * 0.5f;  // (1 in the list: a strong lamp to an eye opened for the night)
    if (model->machinery && id) on *= machinery.LampScale(id, static_cast<size_t>(&light - model->lights.data()));  // [machinery] (its pulse)
    if (light.blink > 0) {  // (each lamp on its own beat, by where it is)
      const float beat = time_ / light.blink + position.x * 0.37f + position.z * 0.53f;
      if (beat - std::floor(beat) > 0.5f) on *= 0.12f;
    }
    // the unit's own glow: in its army's colour (as light, its strongest part 1), so that one sees
    // at night whose it is
    Vec3 tint = light.color;
    if (light.hull && armyColor && lampsTeamColour) {
      tint = {std::pow(std::max(armyColor->x, 0.0f), 2.2f), std::pow(std::max(armyColor->y, 0.0f), 2.2f), std::pow(std::max(armyColor->z, 0.0f), 2.2f)};
      // (a fifth weaker than at first: Chris, 2026-10-13, "the red glow which is the army color is a bit strong")
      tint = tint * (0.8f / std::max({tint.x, tint.y, tint.z, 0.05f}));
    } else if (light.hull && !lampsTeamColour) {
      // ([lampcolour] its faction's own: its file's colour, deepened (raised to the 2.5th: the UEF's (0.27, 0.61, 1.0) to about
      // (0.04, 0.29, 1.0), a blue with more green than red, which on reddish-brown soil reads blue, not violet) and 2.4 at its
      // strongest - blue on the ground's browns shows far less than the army's red does (the interface's checks in a night
      // game: as the file has it, lavender-white; at the 1.6th, still violet))
      const Vec3 c = light.color;
      tint = Vec3{std::pow(std::max(c.x, 0.0f), 2.5f), std::pow(std::max(c.y, 0.0f), 2.5f), std::pow(std::max(c.z, 0.0f), 2.5f)};
      tint = tint * (2.4f / std::max({tint.x, tint.y, tint.z, 0.05f}));
    }
    float lampRadius = light.radius * model->scale;
    float lampAir = 1;
    if (light.hull) {  // [hullglow]
      // ([buildingglow] a structure's: stronger, up to what hullGlow 1 gives, and no farther than hullBuildingMost)
      on *= buildingRule ? std::min(hullGlow * hullBuildingGlow, std::max(hullGlow, 1.0f)) : hullGlow;
      lampRadius *= hullReach;
      if (buildingRule && hullBuildingMost > 0) lampRadius = std::min(lampRadius, hullBuildingMost);
      lampAir = hullAir;
      // (hullEvenColours: an army's colour as bright as light whatever its hue - the red the glow was set by, 0.2 of the
      // sun's light to the eye at the most; a yellow's 0.74 lowered to about 0.33, a green's 0.6 to 0.3, an orange's 0.31
      // to 0.24: not all the way, a strong red looks brighter than its light says)
      const float eye = 0.2126f * tint.x + 0.7152f * tint.y + 0.0722f * tint.z;
      if (hullEvenColours && armyColor && lampsTeamColour && eye > 0.2f) tint = tint * std::pow(0.2f / eye, 0.6f);
    } else if (buildingRule) {
      on *= hullBuildingLamps;  // [buildingglow] (its beacons, strips and windows: lights on it)
    }
    if (light.cone > 0) {
      const Vec3 along = Rotate(orientation, Rotate(boneTurn, light.direction));
      // (MOHO64_DEBUG_LAMPS=<part of a bone's name>, or 1: the first beams whose lamp rides on such a bone, with what turned them,
      // to find a lamp that shines the wrong way in a game)
      if (debugLamps_ && light.bone >= 0 && debugLampsTold_ < 3100 &&
          (debugLamps_[0] == '1' || model->bones[static_cast<size_t>(light.bone)].name.find(debugLamps_) != std::string::npos)) {
        const ModelBone& rest = model->bones[static_cast<size_t>(light.bone)];
        // (the first hundred whatever they say; after that only a bone turned away from its rest,
        // so that a rare wrong pose is still caught late in a long game)
        const float same = std::fabs(rest.rotation.x * boneTurn.x + rest.rotation.y * boneTurn.y + rest.rotation.z * boneTurn.z + rest.rotation.w * boneTurn.w);
        if (debugLampsTold_ < 100 || same <= 0.995f) {
          ++debugLampsTold_;
          const Vec3 forward = Rotate(orientation, Vec3{0, 0, 1});
          fprintf(stderr, "render: lamp on %s of a model with %zu bones: rest %.2f %.2f %.2f %.2f, turned %.2f %.2f %.2f %.2f (%s), beam %.2f %.2f %.2f, the unit faces %.2f %.2f %.2f\n",
                  rest.name.c_str(), model->bones.size(), rest.rotation.x, rest.rotation.y, rest.rotation.z, rest.rotation.w, boneTurn.x, boneTurn.y, boneTurn.z, boneTurn.w,
                  pose ? "posed" : "at rest", along.x, along.y, along.z, forward.x, forward.y, forward.z);
        }
      }
      // (the unit itself does not shadow its lamps' beams: it is named by the middle of the ball its
      // model lies in, as the mesh renderer counts it)
      const Vec3 middle = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
      // the beam in the air: faint in clear night air, plainer in rain, snow and fog
      const float air = 0.03f + 0.16f * fog + 0.08f * rain + 0.09f * snow;
      const float reach = light.radius * model->scale * 0.75f;
      const float wide = std::tan(std::min(light.cone, 120.0f) * 0.5f * 0.45f * 3.14159265f / 180) * reach;  // (its bright middle only)
      if (steadyLamps) {  // [nightlight] (lit by FlushLamps)
        PendingLamp l;
        l.at = at, l.color = tint * on, l.along = along, l.middle = middle, l.origin = position;
        l.radius = light.radius * model->scale, l.cone = light.cone, l.glow = light.glow;
        l.shaftThick = 0.03f * model->scale, l.shaftWide = wide, l.shaftReach = reach, l.shaftAir = air;
        l.weight = byReach * byMost, l.key = lampKey, l.unit = pendingUnit, l.beam = true;
        pendingLamps_.push_back(l);
      } else {
        lights.AddBeam(at, light.radius * model->scale, tint * on, along, light.cone, light.glow, &middle, &position, lampKey);
        shafts.Add(at, at + along * reach, 0.03f * model->scale, wide, tint * (on * air));
        NightNote(lampKey, 1);
      }
    } else if (steadyLamps) {  // [nightlight] (lit by FlushLamps)
      PendingLamp l;
      l.at = at, l.color = tint * on, l.origin = position, l.radius = lampRadius;
      l.weight = byReach * byMost, l.key = lampKey, l.unit = pendingUnit;
      l.hull = light.hull, l.air = lampAir;  // [hullglow]
      l.building = buildingRule;             // [buildingglow]
      pendingLamps_.push_back(l);
    } else {
      lights.Add(at, lampRadius, tint * on, &position, lampAir);  // (the unit does not shadow its own lamps: rays)
      NightNote(lampKey, 1);
    }
    if (steadyLamps && lampKey) {
      const auto level = lampLevels_.find(lampKey);
      litBefore = litBefore || (level != lampLevels_.end() && level->second.level > 0.5f);
    }
  }
  if (steadyLamps) {  // [nightlight] (the crowded squares' order: a building's lamps first, then those lit before)
    for (size_t i = pendingFrom; i < pendingLamps_.size(); ++i) {
      PendingLamp& l = pendingLamps_[i];
      l.order = structure ? 2 : litBefore ? 1 : 0;
      l.square = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(l.at.x / 2.5f)))) << 32) |
                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(l.at.z / 2.5f)));
    }
  }
  for (size_t i = 0; i < takenCount; ++i) {
    // (a beam's square is counted once for the unit, however many of its beams lie in it)
    bool counted = false;
    for (size_t k = 0; takenBeam[i] && k < i; ++k) counted = counted || (takenBeam[k] && taken[k] == taken[i]);
    if (!counted) lampSquares_[taken[i]] += takenBeam[i] ? 1u : 0x10000u;
  }
}

// [nightlight] (steadyLamps) The lamps AddLamps placed this frame, lit once all are in: the crowded squares' rule (as
// AddLamps has it without steadyLamps) taken in an order that does not hang on the order the units came in - a building's
// lamps first, then the lamps of units that were lit the frame before, then by the unit's number - and each lamp's level
// moved toward on or off over a quarter of a second, so that a lamp that rule (an engineer driving past a factory), the
// reach round the camera or the most a unit lights switches fades instead of popping. A lamp seen for the first time
// starts as the rule has it (a unit just built lights at once, as before).
void World::FlushLamps() {
  if (steadyLamps) {
    // (a side picture with its own time - the minimap's ground at time 0 - moves no lamp's level and leaves the game's
    // clock alone: the game's next frame would step by the whole way back from it)
    const bool aside = sidePicture && !extraView;
    const float step = aside ? 0.0f : lampClock_ < 0 ? 1.0f : std::clamp(static_cast<float>(time_ - lampClock_), 0.0f, 0.1f) * 4.0f;
    if (!aside) lampClock_ = time_;
    ++lampStamp_;
    std::vector<uint32_t> order(pendingLamps_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const PendingLamp &x = pendingLamps_[a], &y = pendingLamps_[b];
      if (x.order != y.order) return x.order > y.order;
      const uint64_t ux = x.key >> 8, uy = y.key >> 8;  // (the unit's number; 0 for none)
      if (ux != uy) return ux < uy;
      if (x.unit != y.unit) return x.unit < y.unit;
      return a < b;
    });
    std::vector<uint8_t> allowed(pendingLamps_.size(), 1);
    for (size_t i = 0; i < order.size();) {
      // one unit's lamps (they do not cut each other: counted into the squares after all of them)
      size_t j = i;
      while (j < order.size() && pendingLamps_[order[j]].unit == pendingLamps_[order[i]].unit) ++j;
      uint64_t taken[16];
      bool takenBeam[16];
      size_t takenCount = 0;
      for (size_t k = i; k < j; ++k) {
        const PendingLamp& l = pendingLamps_[order[k]];
        const auto found = lampSquares_.find(l.square);
        const uint32_t count = found == lampSquares_.end() ? 0 : found->second;
        const uint32_t beamUnits = count & 0xffffu, others = count >> 16;
        if (l.beam) {
          bool ownHere = false;
          for (size_t t = 0; t < takenCount; ++t) ownHere = ownHere || (takenBeam[t] && taken[t] == l.square);
          if (beamUnits >= 4 || (beamUnits >= 1 && ownHere)) {
            allowed[order[k]] = 0;
            continue;
          }
        } else if (others >= 2) {
          allowed[order[k]] = 0;
          continue;
        }
        if (takenCount < 16) {
          taken[takenCount] = l.square;
          takenBeam[takenCount++] = l.beam;
        }
      }
      for (size_t t = 0; t < takenCount; ++t) {
        bool counted = false;
        for (size_t u = 0; takenBeam[t] && u < t; ++u) counted = counted || (takenBeam[u] && taken[u] == taken[t]);
        if (!counted) lampSquares_[taken[t]] += takenBeam[t] ? 1u : 0x10000u;
      }
      i = j;
    }
    std::vector<float> hullShares;
    if (hullShare) ShareHullGlows(allowed, &hullShares);  // [hullglow]
    for (size_t i = 0; i < pendingLamps_.size(); ++i) {
      const PendingLamp& l = pendingLamps_[i];
      const float target = allowed[i] ? 1.0f : 0.0f;
      float level = target;
      if (l.key) {
        auto [it, made] = lampLevels_.try_emplace(l.key);
        LampLevel& kept = it->second;
        if (made) kept.level = target;
        else kept.level = kept.level < target ? std::min(kept.level + step, target) : std::max(kept.level - step, target);
        kept.seen = lampStamp_;
        level = kept.level;
      }
      const float shown = level * l.weight;
      NightNote(l.key, shown);
      if (shown <= 0.001f) continue;
      if (l.beam) {
        lights.AddBeam(l.at, l.radius, l.color * shown, l.along, l.cone, l.glow, &l.middle, &l.origin, l.key);
        shafts.Add(l.at, l.at + l.along * l.shaftReach, l.shaftThick, l.shaftWide, l.color * (shown * l.shaftAir));
      } else {
        float shared = hullShares.empty() ? 1.0f : hullShares[i];  // [hullglow]
        // [glowsteady] (Chris's third test, 2026-11-05: "the orange light on the ground" by every factory "flickers around a lot
        // ... like 10 factories all flashing at once", "a pulsating mist"; Unit Glow at Night off stopped it) the share eased as
        // the lamp's level is, over a quarter of a second, not set afresh each frame
        if (l.key) {
          auto kept = lampLevels_.find(l.key);
          if (kept != lampLevels_.end()) {
            float& s = kept->second.share;
            s = s < 0 ? shared : s < shared ? std::min(s + step, shared) : std::max(s - step, shared);
            shared = s;
          }
        }
        lights.Add(l.at, l.radius, l.color * (shown * shared), &l.origin, l.air);
      }
    }
    // (what was not asked for this frame nor the one before is let go: its unit gone, or beyond the reach)
    for (auto it = lampLevels_.begin(); it != lampLevels_.end();) it = lampStamp_ - it->second.seen > 1 ? lampLevels_.erase(it) : std::next(it);
    pendingLamps_.clear();
  }
  // (MOHO64_DEBUG_NIGHTLIGHT)
  if (debugNight_) {
    for (const auto& [key, shown] : nightShown_) {
      const auto before = nightShownBefore_.find(key);
      if (std::fabs(shown - (before == nightShownBefore_.end() ? 0.0f : before->second)) > 0.25f) ++nightPops_;
    }
    for (const auto& [key, shown] : nightShownBefore_)
      if (shown > 0.25f && !nightShown_.count(key)) ++nightPops_;
    nightLamps_ += static_cast<int>(nightShown_.size());
    nightSwaps_ += lights.shadowSwaps;
    nightLeftOut_ += lights.leftOut;
    std::swap(nightShown_, nightShownBefore_);
    nightShown_.clear();
    if (++nightFrames_ == 60) {
      fprintf(stderr, "render: [nightlight] 60 frames: %d lamps a frame, %d popped, %d lamp shadow maps handed to another, %d lights a frame left out of crowded squares, %d merged in the last; at most %d things on the ground clearing plants (4096 kept)\n",
              nightLamps_ / 60, nightPops_, nightSwaps_, nightLeftOut_ / 60, lights.merged, nightDiscs_);
      nightDiscs_ = 0;
      nightFrames_ =nightPops_ = nightSwaps_ = nightLeftOut_ = nightLamps_ = 0;
    }
  }
}

// [hullglow] (hullShare) Each hull glow's share where glows lie over each other: every glow lit this frame (by the crowded
// squares' rule, `allowed`, and its weight) laid on a grid of small squares as the shaders fade a light (lighting.h: 1 at
// its middle, nothing at its reach), and each glow given 1 / n of 1 + ln(n) of itself, n the glows' worth lying where it
// stands (itself included: alone, n is 1 and it stays as it was). Sixteen glows on one place light it as 3.8 of them, not
// 16; two a reach apart do not touch each other. Costs a few squares per small unit's glow, a few hundred per building's.
// [buildingglow] A structure's glow under the structures' rule (PendingLamp::building) counts only the structures' glows
// lying where it stands (a second grid of theirs alone) and takes its share to the power hullBuildingShare.
void World::ShareHullGlows(const std::vector<uint8_t>& allowed, std::vector<float>* shares) {
  shares->clear();
  float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
  size_t hulls = 0;
  for (size_t i = 0; i < pendingLamps_.size(); ++i) {
    const PendingLamp& l = pendingLamps_[i];
    if (!l.hull || !allowed[i] || l.weight <= 0 || !(l.radius > 0)) continue;
    ++hulls;
    x0 = std::min(x0, l.at.x - l.radius), x1 = std::max(x1, l.at.x + l.radius);
    z0 = std::min(z0, l.at.z - l.radius), z1 = std::max(z1, l.at.z + l.radius);
  }
  if (hulls < 2) return;
  // (squares of two world units, larger where the glows spread over more than 512 of them)
  // ([glowsteady] fixed to the world: a power of two times two units, its corners on multiples of it. Laid from the glows' own
  // extent, the squares slid with every unit that moved at the edge and changed size with the extent, and each glow's share,
  // read at its square, jumped from frame to frame: the glows pulsed)
  float cell = 2;
  while (cell < std::max(x1 - x0, z1 - z0) / 512) cell *= 2;
  x0 = std::floor(x0 / cell) * cell;
  z0 = std::floor(z0 / cell) * cell;
  const int cols = static_cast<int>((x1 - x0) / cell) + 1, rows = static_cast<int>((z1 - z0) / cell) + 1;
  hullField_.assign(static_cast<size_t>(cols) * static_cast<size_t>(rows), 0.0f);
  hullBuildingField_.assign(hullField_.size(), 0.0f);  // [buildingglow]
  auto fade = [](float far2) { return far2 >= 1 ? 0.0f : (1 - far2) * (1 - far2) / (1 + 4 * far2); };
  for (size_t i = 0; i < pendingLamps_.size(); ++i) {
    const PendingLamp& l = pendingLamps_[i];
    if (!l.hull || !allowed[i] || l.weight <= 0 || !(l.radius > 0)) continue;
    const int a = std::max(static_cast<int>((l.at.x - l.radius - x0) / cell), 0), b = std::min(static_cast<int>((l.at.x + l.radius - x0) / cell), cols - 1);
    const int c = std::max(static_cast<int>((l.at.z - l.radius - z0) / cell), 0), d = std::min(static_cast<int>((l.at.z + l.radius - z0) / cell), rows - 1);
    const float inv = 1 / (l.radius * l.radius);
    for (int z = c; z <= d; ++z) {
      const float dz = z0 + (static_cast<float>(z) + 0.5f) * cell - l.at.z;
      for (int x = a; x <= b; ++x) {
        const float dx = x0 + (static_cast<float>(x) + 0.5f) * cell - l.at.x;
        const size_t at = static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x);
        const float here = l.weight * fade((dx * dx + dz * dz) * inv);
        hullField_[at] += here;
        if (l.building) hullBuildingField_[at] += here;  // [buildingglow]
      }
    }
  }
  shares->assign(pendingLamps_.size(), 1.0f);
  for (size_t i = 0; i < pendingLamps_.size(); ++i) {
    const PendingLamp& l = pendingLamps_[i];
    if (!l.hull || !allowed[i] || l.weight <= 0 || !(l.radius > 0)) continue;
    // (where it stands, less what its own square's middle took of it short of its full 1: alone, n is its own weight)
    const int x = std::clamp(static_cast<int>((l.at.x - x0) / cell), 0, cols - 1), z = std::clamp(static_cast<int>((l.at.z - z0) / cell), 0, rows - 1);
    const float dx = x0 + (static_cast<float>(x) + 0.5f) * cell - l.at.x, dz = z0 + (static_cast<float>(z) + 0.5f) * cell - l.at.z;
    const std::vector<float>& field = l.building ? hullBuildingField_ : hullField_;  // [buildingglow]
    const float n = field[static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)] +
                    l.weight * (1 - fade((dx * dx + dz * dz) / (l.radius * l.radius)));
    if (n > 1) (*shares)[i] = (1 + std::log(n)) / n;
    if (l.building && n > 1) (*shares)[i] = std::pow((*shares)[i], std::clamp(hullBuildingShare, 0.0f, 1.0f));  // [buildingglow]
  }
}

// [snow ground] The map's props as the snow's shelter sees them (snow_ground.h), made the first time they are wanted: each
// prop's footprint (its model's widest across, a tree's crown) and its height over the ground; nothing lower than a quarter
// of a world unit, nor the streams' ribbons.
gfx::Texture World::SnowProps() {
  if (snowPropsAhead_.valid()) TakeSnowPropsAhead(true);  // [snowhitch] (being worked out ahead: waited for)
  if (snowPropsMade_ || !haveMap_) return snowProps_;
  snowPropsMade_ = true;
  const auto start = std::chrono::steady_clock::now();
  int w = 0, h = 0;
  terrain.SnowShelterSize(&w, &h);
  int trees = 0;
  const std::vector<SnowProp> list = SnowPropList(&trees);
  snowProps_ = SnowPropsRaster(device_, list, static_cast<float>(map.width), static_cast<float>(map.height), w, h, dust.wind);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  printf("render: the snow's shelter: %zu props laid (%d trees), %dx%d, %.0f ms\n", list.size(), trees, w, h, ms);
  return snowProps_;
}

// [snow ground] (the props' list SnowProps lays)
std::vector<SnowProp> World::SnowPropList(int* trees) const {
  std::vector<SnowProp> list;
  *trees = 0;
  for (const PlacedProp& p : props_) {
    if (!p.model || (!p.model->lods.empty() && p.model->lods[0].stream)) continue;
    const float sx = Length(Vec3{p.world.m[0][0], p.world.m[1][0], p.world.m[2][0]});
    const float sy = Length(Vec3{p.world.m[0][1], p.world.m[1][1], p.world.m[2][1]});
    const float sz = Length(Vec3{p.world.m[0][2], p.world.m[1][2], p.world.m[2][2]});
    const float height = p.model->boundsMax.y * sy;
    if (height < 0.25f) continue;
    const float radius = 0.5f * std::max((p.model->boundsMax.x - p.model->boundsMin.x) * sx, (p.model->boundsMax.z - p.model->boundsMin.z) * sz);
    const bool tree = !p.model->lods.empty() && p.model->lods[0].sway > 0 && (p.model->boundsMax.y - p.model->boundsMin.y) * sy >= 1.5f;
    *trees += tree ? 1 : 0;
    list.push_back({p.world.m[0][3], p.world.m[2][3], height, tree ? radius : radius * 0.8f, tree});
  }
  return list;
}

// [snowhitch] Whether snow may come in this game: falling or lying now, or the weather changing by itself in a climate whose
// spells bring snow (the cold one's; an autumn map's temperate weather draws from the autumn climate, as AdvanceWeather).
bool World::SnowAhead() const {
  if (snow > 0 || snowLying > 0) return true;
  const Climate climateNow = seasonOpen_ == Season::Autumn && climate == Climate::Temperate ? Climate::Autumn : climate;
  return weatherAuto && ClimateSnows(climateNow);
}

// [snowhitch] (PrepareAhead, while snow may come and the snow ground is on) What the first snow made, made now, so that the
// frame it begins in makes nothing (Chris's game, 2026-10-21: "froze for around 5-10 seconds right before it began to snow";
// the viewer measured 7.7 s on a new build's pipelines: the ground's snow materials read again with the rest of its set 2.0 s,
// the marks' two snow pipelines 3.0 s, the ground's snow pipeline and its matt variant 2.5 s, the props' raster 0.2 s):
// the ground's (Terrain::PrepareSnow: its materials, pipelines, the shelter's pass and drift mask), the marks' and the pads'
// snow pipelines, and the props' raster for the shelter, worked out on a thread of its own and made into its texture when
// done. The shelter itself is still made the first frame snow lies (from the ground's heights as they are then, as before).
void World::PrepareSnow() {
  terrain.PrepareSnow();
  decals.PrepareSnowGround();
  if (decals.remasteredTarmacs) decals.tarmacs.PrepareSnow();
  if (snowPropsAhead_.valid()) {
    TakeSnowPropsAhead(false);
  } else if (!snowPropsMade_ && terrain.SnowShelterWanted()) {
    int w = 0, h = 0, trees = 0;
    terrain.SnowShelterSize(&w, &h);
    std::vector<SnowProp> list = SnowPropList(&trees);
    snowPropsTrees_ = trees;
    snowPropsCount_ = static_cast<int>(list.size());
    snowPropsStart_ = std::chrono::steady_clock::now();
    const float mapWidth = static_cast<float>(map.width), mapHeight = static_cast<float>(map.height);
    const std::array<float, 2> wind = {dust.wind[0], dust.wind[1]};
    snowPropsAhead_ = std::async(std::launch::async, [list = std::move(list), mapWidth, mapHeight, w, h, wind] {
      return SnowPropsTexels(list, mapWidth, mapHeight, w, h, wind.data(), false);  // (the frames keep the cores' jobs)
    });
  }
}

// [snowhitch] The props' raster worked out ahead made into its texture once it is done (`wait`: waited for if not yet).
void World::TakeSnowPropsAhead(bool wait) {
  if (!snowPropsAhead_.valid()) return;
  if (!wait && snowPropsAhead_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const std::vector<uint16_t> texels = snowPropsAhead_.get();
  int w = 0, h = 0;
  terrain.SnowShelterSize(&w, &h);
  if (snowProps_) device_->Destroy(snowProps_);
  snowProps_ = SnowPropsTexture(device_, texels, w, h);
  snowPropsMade_ = true;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - snowPropsStart_).count();
  printf("render: the snow's shelter: %d props laid (%d trees), %dx%d, %.0f ms ahead on a thread of its own\n", snowPropsCount_, snowPropsTrees_, w, h, ms);
}

// (volume explosions) An area's harm makes a blast's volume where it goes off on or near the ground or the water. Not a flak
// burst high in the air: in two M28 games on SETONS_R every anti-air shell's harm went off 6.6 to 35 units over the ground
// or the water under it, every other weapon's within 5 but for a few shells that met an aircraft. Not under the water:
// below its surface, or a torpedo's or a depth charge's at all (the game's anti-navy shots all say so in their names,
// TANAnglerTorpedo02, AANDepthCharge01, SANUallCavitationTorpedo01, ...; their harm goes off against a hull at the
// waterline, most of it within half a unit of the surface). Nor a warp-in's clearing of its spot (IsTeleportEffect), nor
// a fire burning on: a damage-over-time pulse (DefaultDamage.AreaDoTThread, a UEF bomber's napalm: ten pulses of 4 over
// 3.6 s) is told with no weapon and no shot, and a little harm; a death weapon's, also told without them, does 1000 and more.
bool World::AreaMakesVolume(const FrameHit& hit) const {
  if (IsTeleportEffect(hit.projectile)) return false;
  if (hit.weapon.empty() && hit.projectile.empty() && hit.amount < 50) return false;
  const float x = hit.position[0], y = hit.position[1], z = hit.position[2];
  if (y > GroundHeight(x, z) + 6) return false;
  if (haveMap_ && map.water.present && y < map.water.elevation - 0.5f) return false;
  std::string shot = hit.projectile;
  for (char& c : shot) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return shot.find("torpedo") == std::string::npos && shot.find("depthcharge") == std::string::npos;
}

float World::GroundHeight(float x, float z) const {
  if (!haveMap_) return 0;
  const float y = map.Height(x, z);
  return map.water.present ? std::max(y, map.water.elevation) : y;
}

// [onset] How far rain or snow falling this hard has set in (0..1): all of it from an eighth of the most. What it does at
// once - the sky's cover it brings, the ground getting wet or snowy, the mist it keeps off - grows with it from the first
// drop instead of all at once (as it falls gradually now, weather.cpp's WeatherAt); the same as before from an eighth on.
static float SetIn(float falling) { return std::clamp(falling * 8, 0.0f, 1.0f); }

// The game's weather moved on to this frame (with a time of day, the modern look): what it is by itself, and what it has
// done to surfaces by now. The time since the last frame's.
float World::AdvanceWeather(float time, double clock) {
  if (weatherAuto) {
    // [seasons] an autumn map's weather draws from the autumn climate where the host leaves it temperate
    const Climate climateNow = seasonOpen_ == Season::Autumn && climate == Climate::Temperate ? Climate::Autumn : climate;
    const WeatherNow now = WeatherAt(weatherSeed, clock, climateNow, weatherSpell);
    cloudCover = now.clouds;
    rain = now.rain;
    snow = now.snow;
    fog = now.fog;
    if (climateNow == Climate::Autumn) fog = std::max(fog, AutumnMorningFog(weatherSeed, timeOfDay));  // [seasons] (its mornings' fog)
  }
  // (a side picture with its own time - the minimap's ground at time 0 - shows the weather as it is and moves nothing on:
  // with the clock set to its time the game's next frame took half a second of rain, snow and covered tracks in one step)
  if (sidePicture && !extraView) return 0;
  const float dt = weatherTime_ < 0 ? 0 : std::clamp(time - weatherTime_, 0.0f, 0.5f);
  weatherTime_ = time;
  wet = std::clamp(wet + dt / 15 * SetIn(rain) - dt / 120 * (1 - SetIn(rain)), 0.0f, 1.0f);  // ([onset] by how far it has set in)
  // (snow builds up over two and a half minutes of snowfall and is gone in five; while it melts,
  // the ground it leaves is wet)
  snowLying = std::clamp(snowLying + dt / 150 * SetIn(snow) - dt / 300 * (1 - SetIn(snow)), 0.0f, 1.0f);  // ([onset])
  if (snow <= 0 && snowLying > 0) wet = std::max(wet, std::min(snowLying * 3, 1.0f));
  return dt;
}

// (photomode) The picture's own moment in for this frame (Begin to the end of Render): the game's own weather first moved
// on as every frame moves it, then it and the hour kept aside.
void World::PutMomentIn(float time, double clock) {
  TakeMomentOut();  // (a Begin whose Render did not come: the game's own back first)
  if (timeOfDay >= 0) AdvanceWeather(time, clock);
  gameWeather_ = {timeOfDay, cloudCover, rain, snow, fog, wet, snowLying, weatherAuto};
  timeOfDay = std::fmod(std::clamp(moment.hour, 0.0f, 24.0f), 24.0f);
  weatherAuto = false;
  cloudCover = moment.clouds;
  rain = moment.rain;
  snow = moment.snow;
  fog = moment.fog;
  wet = moment.wet;
  snowLying = moment.snowLying;
  momentShown_ = true;
}

void World::TakeMomentOut() {
  if (!momentShown_) return;
  const GameWeather& g = gameWeather_;
  timeOfDay = g.timeOfDay;
  cloudCover = g.cloudCover;
  rain = g.rain;
  snow = g.snow;
  fog = g.fog;
  wet = g.wet;
  snowLying = g.snowLying;
  weatherAuto = g.weatherAuto;
  momentShown_ = false;
}

void World::Begin(const Camera& cam, int width, int height, float time, double clock) {
  CpuTimes::Part("light, sky, weather set");
  // [hazeamt] the player's amount of haze and mist this frame (World::hazeMist; the modern look's)
  const float hazeAmount = look == Look::Modern ? std::clamp(hazeMist, 0.0f, 2.0f) : 1.0f;
  air.amount = sunlight.amount = distantLand.hazeAmount = hazeAmount;
  particles.remastered = ribbons.remastered = decals.remastered = effectsLook == EffectsLook::Remastered;
  effects.warpInColours = warpInColours && look == Look::Modern && effectsLook == EffectsLook::Remastered;  // [warpin]
  decals.battleScars = battleScars;
  decals.SetSettleScars(settledScars);  // [settled]
  decals.SetModernRelief(markRelief && look == Look::Modern);  // [scars]
  // hot metal (hot_metal.h): this frame's, the modern look's only; the marks laid from now on glow while fresh
  hotOn_ = hotMetal && look == Look::Modern;
  decals.hotMetal = hotOn_;
  decals.heat = &heat;
  if (hotOn_ && !sidePicture && !extraView) heat.Begin(clock);  // [views] (a view besides the first: the first's glows kept)
  particles.quality = effectsQuality;
  camera = cam;
  const Camera& placing = Placing();  // (photomode: a photo's tile places as the whole photo; else the camera itself)
  // [machinery] once a view, before any of its units are posed (a game's in AddFrame, the viewer's placed units, the stand's)
  machinery.Begin(clock, !extraView && !sidePicture, placing.Eye());
  {  // [decalfade] the decals' fade, measured from the camera the picture is placed by (decals.h, Decals::Lod)
    Vec3 right, up, back;
    placing.Axes(&right, &up, &back);
    decals.fadeFraction = decalFadeFraction;
    decals.lod.eye = placing.Eye();
    decals.lod.forward = back * -1.0f;
    decals.lod.widthPerDepth = 2 * std::tan(placing.fovY * 0.5f) * static_cast<float>(width) / static_cast<float>(std::max(height, 1));
    decals.lod.scale = placing.lodScale > 0 ? placing.lodScale : 1.0f;
  }
  time_ = time;
  clock_ = clock;
  barAnchors.clear();
  if (heightsRestored_) {  // [restart] (RestartGame: the ground's heights as the map was opened, drawn so; within a frame)
    heightsRestored_ = false;
    terrain.HeightsChanged(0, 0, map.width, map.height);
    lights.GroundChanged(0, 0, static_cast<float>(map.width), static_cast<float>(map.height));  // [lightshade]
  }
  // (the scene at a share of the screen's size when FSR brings it up; what is measured on the
  // screen - icons, bars, the points the interface asks about - at the screen's own)
  screenWidth_ = width;
  screenHeight_ = height;
  const float share = photo ? 1.0f : std::clamp(upscale, 0.25f, 1.0f);  // (cinecam: a photo at its own size)
  const int sceneWidth = share < 0.999f ? std::max(16, static_cast<int>(std::lround(static_cast<float>(width) * share))) : width;
  const int sceneHeight = share < 0.999f ? std::max(16, static_cast<int>(std::lround(static_cast<float>(height) * share))) : height;
  view = View::Make(camera, sceneWidth, sceneHeight, time);
  if (upscaleLod && sceneHeight < height) view.lodPixels = static_cast<float>(height) / static_cast<float>(sceneHeight);  // [uplod]
  // the temporal upscaler (FSR 3): its shaders made the first time it is asked for
  temporalNow_ = false;
  if (temporalUpscale && !sidePicture && !photo && gSceneSamples == 1 && motionView <= 0) {
    if (fsr3State_ == 0) {
      std::string error;
      fsr3State_ = fsr3.Init(&error) ? 1 : -1;
      if (fsr3State_ < 0) fprintf(stderr, "render: FSR 3 cannot be used (%s); FSR 1 stays\n", error.c_str());
    }
    temporalNow_ = fsr3State_ > 0;
  }
  // frame generation: its passes made the first time it is asked for, the device asked to call MakeBetween at
  // each frame's end (or no longer); on for this frame where the device draws the window into its own picture
  // (cinecam: a photo's frame makes none and keeps none, and leaves the device as it is)
  if (photo) {
    framegenNow_ = framegenReady_ = false;
  } else if (extraView) {  // [views] (frame generation is the first view's: what it keeps of this frame stays)
    framegenNow_ = false;
  } else if (!sidePicture) {
    const bool wanted = frameGeneration && gSceneSamples == 1 && motionView <= 0;
    if (wanted && framegenState_ == 0) {
      std::string error;
      framegenState_ = framegen.Init(&error) ? 1 : -1;
      if (framegenState_ < 0) fprintf(stderr, "render: FSR 3 frame generation cannot be used (%s)\n", error.c_str());
    }
    const bool ask = wanted && framegenState_ > 0;
    if (ask != framegenAsked_) {
      if (ask && !device_->SetFrameGeneration([this] { return MakeBetween(); })) {
        fprintf(stderr, "render: this device cannot show frames made between; frame generation is off\n");
        framegenState_ = -1;
      }
      if (!ask) device_->SetFrameGeneration({});
      framegenAsked_ = ask && framegenState_ > 0;
    }
    framegenNow_ = framegenAsked_ && device_->FrameGenerating() && device_->WindowPicture();
    framegenReady_ = false;
  }
  if (!sidePicture && !photo && !extraView) {  // [views] (a view besides the first: the first's time between frames)
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    frameSeconds_ = lastFrameAt_ > 0 ? static_cast<float>(std::clamp(now - lastFrameAt_, 0.001, 0.25)) : 1 / 60.0f;
    lastFrameAt_ = now;
  }
  // [cloudkeep] what is kept of the frames before, by the time a frame took where it is shorter than a 60th of a second
  clouds.frameShare = air.frameShare = bounce.frameShare =
      steadyFrames || photo || photoTiled ? 1.0f : std::clamp(frameSeconds_ * 60, 0.25f, 1.0f);
  // motion: the view without its jitter kept, this frame's and the frame before's; with the temporal
  // upscaler, the view shifted by this frame's jitter
  const bool moving = (motionOn || motionView > 0 || temporalNow_ || framegenNow_) && gSceneSamples == 1 && !sidePicture && !photo;
  meshes.motion = moving;
  meshes.swayMotion = plantMotion;  // [leftover] (and what sways)
  meshes.keepLast = sidePicture || photo;
  lensSnap_ = photo;
  if (!sidePicture && !photo) {
    const Vec3 eye = camera.Eye();
    const bool jumped = cameraCut || Length(eye - lastEye_) > 0.3f * std::max(camera.distance, 10.0f);
    lensSnap_ = jumped;
    temporalCut_ = jumped || !temporalBefore_;
    framegenCut_ = jumped || !framegenBefore_;
    cameraCut = false;
    lastEye_ = eye;
    temporalBefore_ = temporalNow_;
    framegenBefore_ = framegenNow_;
  }
  if (moving) {
    float jx = 0, jy = 0;
    if (temporalNow_) Fsr3::Jitter(temporalFrame_++, sceneWidth, width, &jx, &jy);
    motion.Begin(&view, jx, jy, temporalNow_ && temporalCut_);
  }
  air.unjittered = moving && temporalNow_ ? &motion.Now() : nullptr;  // [airjitter] (the air's history without the jitter)
  // the light: the map's sun, in the chosen look
  ViewConstants& c = view.constants;
  const Vec3 sunDirection = haveMap_ ? Normalize(map.lighting.sunDirection) : Normalize(Vec3{0.4f, 0.8f, 0.45f});
  auto set = [](float* to, Vec3 v) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
  };
  c.inspect[0] = static_cast<float>(inspect);
  std::fill(std::begin(c.snowShade), std::end(c.snowShade), 0.0f);  // [snowshade] (set with the weather, below)
  std::fill(std::begin(c.snowShadeMore), std::end(c.snowShadeMore), 0.0f);
  // wet shorelines and caustics
  if (waterDetail && look == Look::Modern && haveMap_ && map.water.present) {
    c.shore[0] = water.shoreSoak && water.soakFixed ? 2.0f : 1.0f;  // [soakfix] (2: the wet band and the soak by the map's own heights)
    c.shore[1] = map.water.elevation;
    c.shore[2] = 0.8f;
    c.shore[3] = 2.4f;
  } else {
    std::fill(std::begin(c.shore), std::end(c.shore), 0.0f);
  }
  // light through leaves (y: as much as leaves let through before, in their strongest colour)
  std::fill(std::begin(c.leaves), std::end(c.leaves), 0.0f);
  if (leafLight && look == Look::Modern) {
    c.leaves[0] = 1;
    c.leaves[1] = 0.6f;
  }
  // reflections by ray tracing (where the shaders were built for it)
  reflectNow_ = rayReflections && RayShadersOn() && look == Look::Modern && haveMap_ && !sidePicture && RaysReady();
  if (reflectNow_) {
    c.rayEffects[0] = 1;
    c.rayEffects[1] = 1000;
    c.rayEffects[2] = static_cast<float>(map.width);
    c.rayEffects[3] = static_cast<float>(map.height);
  }
  // the small lights' shadows by rays (where the shaders were built for it)
  lightRaysNow_ = rayLightShadows && RayShadersOn() && look == Look::Modern && haveMap_ && !sidePicture && RaysReady();
  if (lightRaysNow_) {
    c.rayLights[0] = 1;
    c.rayLights[1] = static_cast<float>(std::max(rayLightRays, 1));
    c.rayLights[2] = rayLightSize;
    c.rayLights[3] = rayBeamSize;
  }
  // bounce light: its probes placed round the camera's target (their shaders made the first time it is wanted)
  bounceNow_ = false;
  if (bounceLight && look == Look::Modern && haveMap_ && !sidePicture && RaysReady()) {
    if (!bounceTried_) {
      bounceTried_ = true;
      std::string why;
      bounceMade_ = bounce.Init(&why);
      if (!bounceMade_) fprintf(stderr, "render: bounce light cannot be used (%s)\n", why.c_str());
    }
    if (bounceMade_) {
      // (cinecam: a cinematic camera's probes round a place forty units ahead of its eye, at the finest spacing, where
      // its target may lie in the air far off)
      const Vec3 ahead = placing.cinematic ? placing.Eye() + Vec3{-std::sin(placing.heading), 0, -std::cos(placing.heading)} * 40.0f : placing.target;
      bounce.Place(ahead, placing.cinematic ? 30.0f : placing.distance, [this](float x, float z) { return GroundHeight(x, z); }, &c);
      bounceNow_ = true;
    }
  }
  if (temporalNow_)  // (the temporal upscaler: textures read for the screen's size and finer)
    c.temporal[0] = std::log2(static_cast<float>(sceneWidth) / static_cast<float>(width)) + temporalMipBias;
  else if (upscaleMipBias && sceneWidth < width)  // [upmip] (FSR 1: the textures read for the window's size too)
    c.temporal[0] = std::log2(static_cast<float>(sceneWidth) / static_cast<float>(width));
  if (softShadows && look == Look::Modern) {  // (soft shadows: on, how the edges grow, how far casters are looked for, the widest edge)
    c.softShadows[0] = 1;
    c.softShadows[1] = sunSize;
    c.softShadows[2] = 0.8f;
    c.softShadows[3] = 1.6f;
  }
  // the world beyond the map's edge: six tenths of the map's shorter side out, then lost in the distance
  if (surroundAhead_.valid()) {  // [surahead] (read ahead as the map opened: taken before anything of the frame looks at it)
    TakeSurroundAhead();
    surroundTried_ = true;
  }
  terrain.beyondReach = 0;
  terrain.beyondCopies = true;
  if (haveMap_ && worldBeyond && look == Look::Modern) {
    // (surround) the map's own sculpted surround, with the option: looked for beside the map's file the first time
    if (mapSurround && !surroundTried_) {
      surroundTried_ = true;
      const std::string& path = terrain.mapPath;
      if (path.size() > 6 && path.compare(path.size() - 6, 6, ".scmap") == 0) {
        const std::string name = path.substr(0, path.size() - 6) + "_surround.json";
        if (auto json = vfs_->ReadFile(name)) {
          const std::string folder = path.substr(0, path.find_last_of('/') + 1);
          std::string why;
          if (!surround.Open(map, *json, [&](const std::string& file) { return vfs_->ReadFile(folder + file); }, &why))
            fprintf(stderr, "render: %s: %s (beyond the edge the map laid out again is drawn)\n", name.c_str(), why.c_str());
        }
      }
    }
    surround.blockOut = surroundBlockOut;        // (the detail look's switches)
    surround.edgeMirror = surroundEdgeMirror;    // [edgehand]
    surround.handOver = surroundHandOver;
    surround.forestFloor = surroundForestFloor;
    const bool sculpted = mapSurround && surround.Usable();
    // (its scenery read the first time it is wanted; where its single trees stand, for the ground's forest)
    if (sculpted && surroundScenery && !surroundSceneryTried_) ReadSurroundScenery();
    const bool sceneryNow = sculpted && surroundScenery && !surroundCells_.empty();
    surround.treesFrom = sceneryNow ? surroundTrees_[0] : 0.0f;
    surround.treesTo = sceneryNow ? surroundTrees_[1] : 0.0f;
    surround.sightFrom = surroundSceneryFrom;
    surround.sightTo = surroundSceneryTo;
    // (sky) with the land far off on, farther: BeyondLand::reach; (surround) as far as the map's surround goes
    terrain.beyondReach = sculpted ? surround.Reach()
                                   : (beyondLand ? std::clamp(distantLand.reach, 0.6f, 1.0f) : 0.6f) * static_cast<float>(std::min(map.width, map.height));
    terrain.beyondCopies = !sculpted;
    c.beyond[0] = static_cast<float>(map.width);
    c.beyond[1] = static_cast<float>(map.height);
    c.beyond[2] = 1 / terrain.beyondReach;
    c.beyond[3] = beyondHaze ? 0.78f : -0.78f;  // (negative: no haze but at the far end, lighting.h)
    // (cinecam) a cinematic camera low over the land sees that world go over into the horizon's own haze, as far land
    // and sea do, not greyed (view.h, BeyondGrey): from up high the grey sets the map apart; from low down it would lie
    // as a grey band under the bright horizon
    if (camera.cinematic) c.skyExtra[3] = 1;
    if (beyondLand) distantLand.SetView(&c);  // (beyondland) the soft edge (beyond_land.h)
    // [edgeband] past a sculpted surround's edge the dimming comes in over a short band (surroundEdgeBand), so that
    // where units cannot go reads from the game's height: over the soft edge's own 60 units nothing told the two apart
    if (beyondLand && sculpted && surroundEdgeBand > 0) c.beyondLand[1] = std::max(surroundEdgeBand, 0.01f);
  }
  set(c.sunDirection, sunDirection);
  set(c.sunColor, haveMap_ ? map.lighting.sunColor : Vec3{1, 1, 1});
  set(c.sunAmbience, haveMap_ ? map.lighting.sunAmbience : Vec3{0.2f, 0.2f, 0.2f});
  set(c.shadowFill, haveMap_ ? map.lighting.shadowFill : Vec3{0.3f, 0.3f, 0.4f});
  c.lightMultiplier = haveMap_ ? map.lighting.multiplier : 1;
  c.look = look == Look::Modern ? 1.0f : 0.0f;
  c.modern[0] = sun;
  c.modern[1] = sky;
  c.modern[2] = exposure;
  c.modern[3] = reflection;
  lampsOn_ = 0;
  lights.night = 1;
  shafts.Begin();
  wakes.Begin(wakesRemade && look == Look::Modern);  // [wakes]
  {  // ripples on the water (their pipelines made the first frame they are on, not at the game's first splash)
    const bool ripplesOn = waterRipples && look == Look::Modern && haveMap_ && map.water.present;
    if (!extraView) water.ripples.Begin(clock_, ripplesOn, sidePicture);  // [views] (moved on once a frame, by the first view)
    if (!sidePicture) water.ClearAfloat();  // [floatfoam] (what floats listed anew for each view, AddFrame; a side picture drawn
                                            // without its own AddFrame keeps the frame's)
    if (ripplesOn) water.PrepareRipples();
    // (winter water's pipeline made while the option is on, before the first snow needs it)
    if (winterWater && look == Look::Modern && haveMap_ && map.water.present) water.PrepareWinter(ripplesOn);
  }
  lampUnitsBefore_ = lampUnits_;
  lampUnits_ = 0;
  // [nightlight] (steadyLamps: the units counted eased over some twenty frames; the frame's lamps placed anew)
  lampUnitsSmooth_ = lampUnitsSmooth_ <= 0 ? static_cast<float>(lampUnitsBefore_) : lampUnitsSmooth_ + (static_cast<float>(lampUnitsBefore_) - lampUnitsSmooth_) * 0.05f;
  pendingLamps_.clear();
  pendingUnits_ = 0;
  if (moving_.size() > 60000) moving_.clear();  // (entity ids pile up over a long game)
  lampSquares_.clear();
  // (volume explosions, volume_blasts.h: on in the modern look; while on they hear of every flash a script makes)
  volumesOn_ = volumeExplosions && look == Look::Modern && haveMap_;
  particles.volumesOn = volumesOn_;
  if (volumesOn_ != static_cast<bool>(lights.onFlash)) {
    if (volumesOn_) {
      lights.onFlash = [this](const std::string& ramp, Vec3 at, float size, double born, float seconds, const FlashFrom& from) {
        volumes.Offer(ramp, at, size, born, seconds, from);
      };
      // (VolumeReplaces = "inside": the emitters a volume covers, taken back as it is born, or not started under it)
      volumes.onBorn = [this](Vec3 at, float radius, double born) { particles.VolumeBorn(at, radius, born, volumes.insideSeconds); };
      particles.volumeCovers = [this](Vec3 at, double when) { return volumes.Covers(at, when); };
    } else {
      lights.onFlash = nullptr;
      volumes.onBorn = nullptr;
      particles.volumeCovers = nullptr;
      volumes.Clear();
    }
  }
  // a time of day: the computed sky's light in place of the map's (modern look)
  Vec3 lightDirection = sunDirection;
  // (photomode) a moment of the picture's own: this frame's hour and weather, the game's own moved on and kept aside
  if (moment.on && look == Look::Modern) PutMomentIn(time, clock);
  // [seasons] an autumn map's weather draws from the autumn climate where the host leaves it temperate (AdvanceWeather)
  const Climate climateNow = seasonOpen_ == Season::Autumn && climate == Climate::Temperate ? Climate::Autumn : climate;
  if (timeOfDay >= 0 && look == Look::Modern) {
    // the weather: what it is by itself, its clouds, and what it has done to surfaces by now
    float flash = 0;
    uint32_t stroke = 0;
    const float dt = momentShown_ ? 0.0f : AdvanceWeather(time, clock);  // (photomode: with a moment, PutMomentIn moved it on)
    flash = LightningAt(weatherSeed, clock, rain, &stroke);
    // (the tracks in it: falling snow covers them again in about a minute and a half of heavy fall)
    tracksCovering_ = snow > 0 ? dt * snow / 90 : 0;
    decals.snowLying = snowLying;
    decals.snowing = snow;
    const float cover = std::max({cloudCover, SetIn(rain) * (0.8f + 0.2f * rain), SetIn(snow) * (0.75f + 0.2f * snow), fog * 0.6f});  // ([onset])
    skyLight = SkyAt(timeOfDay, cover, skyTables && look == Look::Modern);
    // [stormcells] (Chris, 2026-11-06: "lets do separate tall towers") a storm's sky dark of itself, slate grey, the sun's
    // glow round it dimmed: with the 3D clouds' cells standing apart, what shows between them is a storm's sky, not a fair one
    if (stormCells && volumetricClouds) {
      const float t = std::clamp((rain - 0.6f) / 0.35f, 0.0f, 1.0f), s = t * t * (3 - 2 * t);
      if (s > 0) {
        auto slate = [&](Vec3 v, float dark) {
          const float b = (v.x + v.y + v.z) / 3;
          const Vec3 grey = Vec3{0.92f, 0.95f, 1.0f} * b;
          return (v * (1 - 0.9f * s) + grey * (0.9f * s)) * (1 - dark * s);
        };
        skyLight.zenith = slate(skyLight.zenith, 0.55f);
        skyLight.horizon = slate(skyLight.horizon, 0.35f);
        // (the sky drawn behind the 3D clouds: toward the same grey, overhead's and the horizon's between)
        skyLight.storm = 0.85f * s;
        const Vec3 between = (skyLight.zenith + skyLight.horizon) * 0.5f;
        skyLight.stormGrey = (between.x + between.y + between.z) / 3;
        skyLight.glow = skyLight.glow * (1 - 0.75f * s);
        // (the sky from tables: all of it the clouds' grey, darker)
        skyLight.greyShare = skyLight.greyShare + (1 - skyLight.greyShare) * s;
        skyLight.grey *= 1 - 0.45f * s;
      }
      air.storm = s;
    } else {
      air.storm = 0;
    }
    air.moonShare = starryNight ? 1 - 0.35f * skyLight.moonIn : 1.0f;  // [starry] (the moon's glow in the air softer; (dusk turn) as its light comes in)
    Vec3 bolt{0, 0, 0};
    if (flash > 0) {  // lightning: for a moment the clouds light everything (less to an eye opened for the night)
      bolt = Vec3{0.75f, 0.82f, 1.0f} * (flash * 1.1f / std::pow(skyLight.exposure, 0.6f));
      skyLight.zenith += bolt;
      skyLight.horizon += bolt * 0.6f;
    }
    lampsOn_ = std::clamp((0.30f - skyLight.daylight) / 0.18f, 0.0f, 1.0f);
    clouds.boltLight = Vec3{0, 0, 0};  // (the volumetric clouds' flash within: only while a bolt shows, below)
    if (flash > 0.02f) {
      // the bolt itself: from the clouds to the ground somewhere round what the camera looks at (the
      // place and its zigzag follow from which stroke it is, so it stands still while it flashes),
      // and its light on the ground where it strikes
      const float reach = std::max(placing.distance * 0.4f, 25.0f);  // (near enough to be in the picture most times)
      Vec3 foot = placing.target + Vec3{(WeatherDraw(weatherSeed, stroke, 71) - 0.5f) * 2 * reach, 0, (WeatherDraw(weatherSeed, stroke, 73) - 0.5f) * 2 * reach};
      foot.y = haveMap_ ? GroundHeight(foot.x, foot.z) : placing.target.y;
      const Vec3 light = Vec3{0.8f, 0.86f, 1.0f} * (flash * 22 / skyLight.exposure);
      const float top = foot.y + 150, wide = std::max(0.3f, placing.distance * 0.009f);
      Vec3 from{foot.x + (WeatherDraw(weatherSeed, stroke, 79) - 0.5f) * 40, top, foot.z + (WeatherDraw(weatherSeed, stroke, 83) - 0.5f) * 40};
      // (the volumetric clouds lit from within where the bolt leaves them, a little up in the layer)
      clouds.boltAt = Vec3{from.x, clouds.Underside(camera.Eye().y) + clouds.Thick() * 0.3f, from.z};  // (cloudzoom: where the march has the layer)
      clouds.boltReach = clouds.Thick() * 0.9f;
      clouds.boltLight = Vec3{0.8f, 0.86f, 1.0f} * (flash * 8 / skyLight.exposure);
      const int pieces = 18;
      for (int k = 1; k <= pieces; ++k) {
        const float t = static_cast<float>(k) / pieces;
        const uint32_t n = static_cast<uint32_t>(k);
        Vec3 to = from + (foot - from) * (1.0f / static_cast<float>(pieces - k + 1));
        if (k < pieces) {  // (a zigzag, smaller toward the ground)
          const float far = 22 * (0.35f + 0.65f * (1 - t));
          to.x += (WeatherDraw(weatherSeed, stroke, 100 + n) - 0.5f) * far;
          to.z += (WeatherDraw(weatherSeed, stroke, 200 + n) - 0.5f) * far;
        }
        shafts.AddBolt(from, to, wide, light);
        shafts.AddBolt(from, to, wide * 5, light * 0.08f);  // (its glow in the wet air)
        if (k < pieces - 3 && WeatherDraw(weatherSeed, stroke, 300 + n) < 0.3f) {
          // a branch: three thinner, dimmer pieces off to one side and downward, ending in the air
          Vec3 at = to;
          float turn = WeatherDraw(weatherSeed, stroke, 400 + n) * 6.2832f;
          for (uint32_t b = 0; b < 3; ++b) {
            const float step = (top - foot.y) / pieces * 0.7f;
            turn += (WeatherDraw(weatherSeed, stroke, 800 + n * 4 + b) - 0.5f) * 2.2f;  // (it wanders too)
            const Vec3 next = at + Vec3{std::cos(turn) * step * (0.9f + WeatherDraw(weatherSeed, stroke, 500 + n * 4 + b)),
                                        -step * (0.5f + 0.8f * WeatherDraw(weatherSeed, stroke, 600 + n * 4 + b)),
                                        std::sin(turn) * step * (0.9f + WeatherDraw(weatherSeed, stroke, 700 + n * 4 + b))};
            shafts.AddBolt(at, next, wide * (0.6f - 0.13f * static_cast<float>(b)), light * (0.45f - 0.12f * static_cast<float>(b)));
            at = next;
          }
        }
        from = to;
      }
      lights.Add(foot + Vec3{0, 3, 0}, 45, Vec3{0.8f, 0.86f, 1.0f} * (flash * 6 / skyLight.exposure));
    }
    // (volume explosions) a nuke's flash lights the sky and everything under it for a moment, as lightning does, and
    // the clouds over it from within (to an eye opened for the night less than lightning: lit as by lightning's measure,
    // the whole night picture went white)
    if (volumesOn_ && !sidePicture) {
      const Vec3 nuke = volumes.SkyFlash(clock) * (1 / std::pow(skyLight.exposure, 0.9f));
      skyLight.zenith += nuke;
      skyLight.horizon += nuke * 0.6f;
      bolt += nuke;
      Vec3 at, glow;
      float reach = 0;
      if (clouds.boltLight.x + clouds.boltLight.y + clouds.boltLight.z <= 0 && volumes.CloudFlash(clock, &at, &reach, &glow)) {
        clouds.boltAt = Vec3{at.x, std::max(clouds.base, camera.Eye().y + 40) + clouds.Thick() * 0.15f, at.z};
        clouds.boltReach = reach;
        clouds.boltLight = glow * (1 / skyLight.exposure);
      }
    }
    c.falling[0] = rain;
    c.falling[1] = snow;
    c.falling[2] = flash;
    // the game's glowing effects and their flashes of light were made for the day's exposure
    c.falling[3] = std::pow(1 / skyLight.exposure, 0.65f);
    lights.night = c.falling[3];
    c.weather[0] = wet;
    c.weather[1] = snowLying;
    // (rain and snow thicken the air a little; the air never hides all of a far thing, or the
    // whole map seen from high up would be gone)
    // [weather] (Chris's snapshot test, 2026-10-25: "Foggy weather: too heavy, the map hard to see, odd light. Wanted: toned
    // down and low, near the ground, not the whole map") With the low fog (World::lowFog), fog past the quarter the rain's and
    // snow's murk brings (a storm's, a snowfall's, an overcast day's: those as they were) goes mostly into a layer that hugs
    // the ground and the water, the mist's (below: deeper than the mist, thickest over the water and in the hollows, thin
    // over the hills), and only a little of it into the air all over: from far up the map shows through it.
    const float fogLow = lowFog ? std::max(fog - 0.25f, 0.0f) / 0.75f : 0.0f;
    // [mistpatch] (World::mistPatches) a thick fog leaves only a little in the air all over (at full fog a tenth, not 0.36:
    // from the game's camera the ground next to it showed through a third to two fifths of grey, Chris's second test,
    // 2026-11-04, measured on his pictures); what there is of it lies in the layer's patches. A storm's and an overcast day's
    // murk (fog up to a quarter) as before.
    const bool patches = lowFog && mistPatches;
    const float fogAll = !lowFog ? fog
                         : patches ? std::min(fog, 0.25f) * (1 - 0.6f * fogLow)
                                   : std::min(fog, 0.25f) + 0.15f * std::max(fog - 0.25f, 0.0f);
    c.weather[2] = 0.0035f * fogAll + 0.0007f * rain + 0.0010f * snow;
    c.weather[3] = 0.55f + 0.3f * fogAll;
    distantLand.thickAir = c.weather[2];  // (beyondland: the weather's own, before a cinematic camera's least)
    // (cinecam) a cinematic camera low over the land sees far toward the horizon: some of the air's haze then, even
    // in clear weather, so that the distance lies back as it does outdoors
    if (camera.cinematic) c.weather[2] = std::max(c.weather[2], cinematicHaze * hazeAmount);  // ([hazeamt])
    // the mist on low ground
    mistNow = mist;
    if (mistNow < 0) {
      auto rise = [](float from, float to, float h) {
        const float t = std::clamp((h - from) / (to - from), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
      };
      // by the hour: a third of it lies from nine in the evening, half of it from half past five
      // to seven, and the morning has burnt it off by half past nine ([mist1107] Chris, 2026-11-07, the mini trailer's dawn:
      // "the morning mist might be a bit too strong I think we should halve it": its peak was all of it)
      const float h = timeOfDay;
      const float byHour = h < 12 ? (0.3f + 0.2f * rise(4, 5.5f, h)) * (1 - rise(7, 9.5f, h)) : 0.3f * rise(19, 21, h);
      // ([seasons] more in autumn)
      // ([hazeamt] the hour's and the wet ground's by the player's amount; the fog's is the weather's)
      mistNow = byHour * (climateNow == Climate::Dry ? 0.25f : climateNow == Climate::Autumn ? 1.3f : 1.0f) * hazeAmount + 0.5f * (lowFog ? std::min(fog, 0.25f) : fog) + 0.4f * wet * (1 - SetIn(rain)) * hazeAmount;  // ([onset]; [weather] the low fog's below)
      mistNow = std::clamp(mistNow * (1 - 0.5f * rain), 0.0f, 1.0f) * (1 - SetIn(snow));
    } else if (hazeAmount != 1) {
      mistNow = std::min(mistNow * hazeAmount, std::max(mistNow, 1.0f));  // ([hazeamt] a set amount of mist too)
    }
    // [weather] the low fog in the mist's layer: up to 0.75 mists more, the layer half as deep again at its thickest (it
    // fills the hollows and leaves the hilltops). ([fog1103] Chris's big test, 2026-11-02: "Fog makes everything hard to see should
    // be more low to the ground but visible": the game's low camera looked along it through the ground's units; was 1.2
    // mists, three times as deep, hiding at most 0.8: now lower, thinner, at most 0.45)
    const float fogLayer = fogLow * 0.75f * (1 - SetIn(snow));
    if (haveMap_ && mistNow + fogLayer > 0) {
      // (seen straight down through all of it at its floor, in the middle of a bank, full mist hides a third)
      const float depth = mistDepth_ * (1 + 0.5f * fogLow);
      c.mist[0] = 0.23f * (mistNow + fogLayer) / depth;
      c.mist[1] = mistFloor_;
      c.mist[2] = 1 / depth;
      c.mist[3] = 0.6f - 0.15f * fogLow;  // ([fog1103] the low fog hides less of what is in it, not more)
      c.mistMore[0] = 0.75f * fogLayer / (mistNow + fogLayer);  // (the low fog's share of the layer: its banks more even)
      if (patches) {  // [mistpatch] (as thick as the fog is, the layer in its patches alone, a morning's mist with it: view.h, MistPatches)
        c.mistMore[1] = fogLow;
        c.mistMore[2] = std::clamp(mistClear, 0.05f, 0.95f);
      }
    }
    c.mistMore[3] = look == Look::Modern ? std::clamp(unitDetail, 0.0f, 2.0f) : 0.0f;  // [detail]
    {  // [stormcells] (section 99: sun pockets that show) in a storm the sun 1.4 times as strong where it gets through the
       // cover (the cover's shadows, made 0.9 of it below, keep it off the rest: the storm as dark overall)
      const float t = stormCells && volumetricClouds ? std::clamp((rain - 0.6f) / 0.35f, 0.0f, 1.0f) : 0.0f;
      stormSunNow_ = t * t * (3 - 2 * t);
      SkyLight lit = skyLight;
      lit.color = lit.color * (1 + 0.4f * stormSunNow_);
      SetSky(lit, &c);
    }
    if (skyTables && look == Look::Modern) {  // the sky from tables: what reads the computed sky reads its view (view.h, ComputedSky)
      c.skyTable[0] = 1;
      c.skyTable[1] = skyLight.greyShare;
      c.skyTable[2] = skyLight.grey;
      c.skyTable[3] = skyLight.faint;
      c.skyExtra[0] = bolt.x;
      c.skyExtra[1] = bolt.y;
      c.skyExtra[2] = bolt.z;
    }
    lightDirection = skyLight.direction;
    // [snowshade] (World::snowShade) the shade on lying snow: as much of it as the sky is overcast (none under a half-clouded
    // sky, all of it from some nine tenths: a snowfall's), by day only (the sun's weak light through the clouds is the sun's,
    // not the moon's), and only while snow lies (lighting.h then takes it on each surface as snow lies there)
    if (snowShade && haveMap_ && snowLying > 0 && !skyLight.moon) {
      const float overcast = std::clamp((cover - 0.5f) / 0.35f, 0.0f, 1.0f);
      const float shade = overcast * overcast * (3 - 2 * overcast) * std::clamp(skyLight.daylight * 4, 0.0f, 1.0f);
      if (shade > 0) {
        c.snowShade[0] = shade;
        c.snowShade[1] = std::clamp(snowShadeTune[0], 0.0f, 1.0f);
        c.snowShade[2] = std::clamp(snowShadeTune[1], 0.0f, 1.0f);
        c.snowShade[3] = std::clamp(snowShadeTune[2], 0.0f, 1.0f);
        c.snowShadeMore[0] = std::clamp(snowShadeTune[3], 0.0f, 2.0f);
        c.snowShadeMore[1] = std::clamp(snowShadeTune[4], 0.0f, 1.0f);
        c.snowShadeMore[2] = std::clamp(snowShadeTune[5], 1.0f, 30.0f);
        c.snowShadeMore[3] = std::clamp(snowShadeTune[6], 0.0f, 1.0f);
      }
    }
    // winter water (winter_water.h): the water goes over to its winter look with the lying snow, half of the way while
    // a third of a covering lies and all of it by two thirds; back again as it thaws
    if (winterWater && haveMap_ && map.water.present && snowLying > 0) {
      const float t = std::clamp(snowLying / 0.65f, 0.0f, 1.0f);
      c.winterWater[0] = std::max(t * t * (3 - 2 * t), 1e-4f);
      c.winterWater[1] = map.water.elevation;
      c.winterWater[2] = static_cast<float>(map.width);
      c.winterWater[3] = static_cast<float>(map.height);
    }
  }
  {  // [sunlight] drifting cloud shadows and the light shafts' low band (sunlight.h), by the sky and the weather just reckoned
    const bool day = timeOfDay >= 0 && look == Look::Modern && haveMap_;
    // ([mistpatch] in a thick fog the low band's haze and the haze under the clouds thicken by a quarter of it, World::mistPatches:
    // the fog lies in the mist's layer's patches; over all the ground they were the most of the grey close to the camera)
    sunlight.cloudMix = volumetricClouds ? clouds.mix : 0.0f;  // [cloudmix2]
    sunlight.Begin(&view, day && driftingShadows, day && lightShafts && volumetricFog && !sidePicture, placing, mistNow, wet,
                   lowFog && mistPatches ? 0.25f * fog : fog,
                   static_cast<float>(map.width), static_cast<float>(map.height), terrain.HeightUnit(),
                   map.water.present ? map.water.elevation : -1e9f);
    if (stormSunNow_ > 0 && c.cloudDrift[0] > 0.5f)  // ([stormcells] the cover's shadows take more of the sun in a storm)
      c.cloudDrift[1] = c.cloudDrift[1] + (std::max(c.cloudDrift[1], 0.9f) - c.cloudDrift[1]) * stormSunNow_;
    if (timeOfDay >= 0) c.cloudDrift[3] *= skyLight.fallIn;  // (dusk turn: the sea darkens under them whatever light falls)
  }
  if (!sidePicture && !photo && !extraView) {  // (airfix) a weather cut (World::weatherCut): the moment or the weather jumped since the window's last frame
    const bool lit = timeOfDay >= 0 && look == Look::Modern;
    const float now[7] = {lit ? timeOfDay : -1.0f, cloudCover, rain, snow, fog, wet, lit ? mistNow : 0.0f};
    bool jumped = false;
    if (lastWeatherKnown_) {
      const float hour = std::fabs(now[0] - lastWeather_[0]);
      jumped = (now[0] < 0) != (lastWeather_[0] < 0) || (now[0] >= 0 && std::min(hour, 24 - hour) > 0.5f);
      for (int i = 1; i < 7; ++i) jumped = jumped || std::fabs(now[i] - lastWeather_[i]) > 0.1f;
    }
    std::copy(std::begin(now), std::end(now), lastWeather_);
    lastWeatherKnown_ = true;
    if (weatherCuts && (weatherCut || jumped)) {
      air.Restart();
      clouds.Restart();
    }
    weatherCut = false;
  }
  {  // movement dust (movement_dust.h): on in the modern look, with what the weather has done to the ground and the air
    dustNow_ = movementDust && look == Look::Modern && haveMap_ && !sidePicture;
    MovementDust::Weather weatherNow;
    if (timeOfDay >= 0 && look == Look::Modern)
      weatherNow = {wet, rain, snow, snowLying, cloudCover, fog, skyLight.daylight, climateNow == Climate::Cold, climateNow == Climate::Dry};
    dust.Begin(clock_, dustNow_, weatherNow, camera.Eye());
  }
  {  // [horizon] the weather on the horizon (horizon.h): what of it this frame, by the weather just reckoned
    horizonNow_ = false;
    if (horizonWeather && look == Look::Modern && haveMap_ && terrain.beyondReach > 0 && timeOfDay >= 0 && !sidePicture) {
      Horizon::Weather w;
      w.hour = timeOfDay;
      w.snowLying = snowLying;
      w.rain = rain;
      w.snow = snow;
      w.fog = fog;
      w.wet = wet;
      w.mist = mistNow;
      w.haze = hazeAmount;  // [hazeamt]
      w.fallIn = skyLight.fallIn;
      w.cloudCover = c.skyClouds[0];
      w.cold = climate == Climate::Cold;
      w.seed = weatherSeed;
      // [snowsur] (beside a sculpted surround the winter sea's floes begin far out: Horizon::iceFrom)
      horizon.iceFrom = mapSurround && surround.Usable() && !terrain.beyondCopies ? std::max(surroundIceFrom, 0.0f) * terrain.beyondReach : 0.0f;
      horizonNow_ = horizon.Begin(map, terrain.beyondReach, w, time, camera, view.width, view.height);
    }
  }
  {  // [surf] surf and sea spray (surf.h): the sea as rough as the weather's wind (as the trees' gusts have it: rain and
     // snow bring it, a storm most), the spray drifting with that wind; the water's surf pipelines made while it is on
    const bool surfOn = surf && look == Look::Modern && haveMap_ && map.water.present;
    surfNow_ = surfOn && !sidePicture;
    const bool weatherOn = timeOfDay >= 0 && look == Look::Modern;
    const float rough = surfRoughness >= 0 ? surfRoughness : weatherOn ? std::clamp(0.15f + 0.85f * rain + 0.3f * snow, 0.0f, 1.0f) : 0.15f;
    const float gust = 1 + 2 * rough;
    const float wind[2] = {dust.wind[0] * gust, dust.wind[1] * gust};
    water.surf.Begin(time, surfOn, sidePicture, rough, wind, camera.Eye());
    if (surfOn) water.PrepareSurf(waterRipples, winterWater, waterDetail);  // ([shore] waterDetail: the wet beach's too)
    // [shore] (wet shorelines) while the surf lays the beach's wetness (surf.h, SurfWetAt; water.cpp, WetBeach), the ground's
    // own wet band by height is narrowed to what a calm bay keeps (0.3 over the water), as much as the surf is shown
    if (c.shore[0] > 0.5) c.shore[2] -= (c.shore[2] - 0.3f) * water.surf.WetShare();
  }
  {  // [ocean] the wind sea (ocean.h): as rough as the surf's sea (the weather's wind), the wind's way the weather's; the
     // water's ocean pipelines made while it is on
    const bool oceanOn = oceanWaves && look == Look::Modern && haveMap_ && map.water.present;
    const bool weatherOn = timeOfDay >= 0 && look == Look::Modern;
    const float rough = surfRoughness >= 0 ? surfRoughness : weatherOn ? std::clamp(0.15f + 0.85f * rain + 0.3f * snow, 0.0f, 1.0f) : 0.15f;
    if (!extraView) water.ocean.Begin(time, oceanOn, sidePicture, rough, dust.wind);  // [views] (a view besides the first: the first's sea)
    water.ocean.lift = oceanOn && oceanLift;  // [lift]
    if (!sidePicture) effects.drawnMoves = water.ocean.Riding();  // [whitecaps] (the effects on a riding hull's bones ride with it)
    effects.snowLying = weatherOn ? snowLying : 0.0f;              // [snowkick]
    if (oceanOn) water.PrepareOcean(waterRipples, winterWater, surf, waterDetail, oceanLift);
  }
  // [stream] streams as water (stream_water.h): their props left out of the models' drawing while the streams' pass draws them
  streamNow_ = streamWater && look == Look::Modern && haveMap_ && !sidePicture && streams.Any() && streams.Prepare();
  meshes.divertStream = streamNow_;
  // [step2b] the still scenery picked on the card: where it pays, or every frame (and the 3D plants' draws by their lists'
  // counts with it)
  meshes.cardScenery = cardScenery > 0;
  meshes.cardByNeed = cardScenery == 1;
  grass.indirectModels = cardScenery > 0;
  CpuTimes::Part("sun shadows placed");
  if (haveMap_ && shadowsOn) {
    if (shadows.Size() != shadowSize) shadows.SetSize(shadowSize);
    shadows.ground = &terrain;  // [shadowtri] (the fit's floor: the lowest ground under what may be seen, Terrain::LowestIn)
    shadows.fitCasters = shadowFit;  // [cardcost] (nothing the camera sees lies much below the ground's lowest point)
    shadows.fitFloor = terrain.Lowest() - 32;
    // [cardcost] (the far cascades kept only in the game's own frames: a frame for something else draws into the same maps)
    shadows.keepEvery = shadowKeep >= 2 && !sidePicture && !extraView && !photo && !photoTiled ? shadowKeep : 0;
    // [shadowstep] (the cascades' sizes on their ladder, kept from frame to frame: only in the game's own frames, as above)
    shadows.zoomSteps = shadowZoomSteps && !sidePicture && !extraView && !photo && !photoTiled;
    shadows.Place(placing, lightDirection, &view);
  }
  CpuTimes::Part("tracks, icons, deposits begun");

  // the tracks in lying snow: read by the ground while snow lies
  if (!extraView) tracks.Begin();  // [views] (pressed once a frame, by the first view)
  // (deep tracks in their place, when on: their field made the first time)
  // (a frame for something else, the minimap's ground: without them, the field left as it is for the game's frames)
  deepNow_ = false;
  if (deepTracks && haveMap_ && look == Look::Modern && !sidePicture) {
    if (!deepTried_) {
      deepTried_ = true;
      std::string why;
      if (!trackField.Init(&why)) fprintf(stderr, "render: deep tracks cannot be drawn (%s)\n", why.c_str());
    }
    deepNow_ = trackField.Ready();
  }
  if (!sidePicture && !extraView) {  // [views] (the field stays round the first view's camera)
    trackField.on = deepNow_;
    trackField.churnedHoles = churnedHoles;  // [churn]
    trackField.Begin(placing.target, clock);
  }
  if (!deepNow_ && haveMap_ && tracks.on && snowLying > 0 && look == Look::Modern && timeOfDay >= 0) {
    c.tracksArea[0] = tracks.InverseWidth();
    c.tracksArea[1] = tracks.InverseHeight();
    c.tracksArea[2] = 1;
  }
  terrain.tracksMap = deepNow_ ? trackField.Atlas() : tracks.Map();
  terrain.tracksField = deepNow_ ? trackField.Constants() : TrackFieldConstants{};
  terrain.parallax = groundParallax && look == Look::Modern;  // [parallax]
  // [tarmac] remastered tarmacs: the modern look with the remastered effects; their relief close by with the ground's, its
  // reach and shade; the ground's tracks; its tier
  decals.remasteredTarmacs = remasteredTarmacs && look == Look::Modern && effectsLook == EffectsLook::Remastered && haveMap_;
  decals.tarmacs.parallax = terrain.parallax;
  decals.tarmacs.parallaxReach = terrain.parallaxReach;
  decals.tarmacs.parallaxShadows = terrain.parallaxShadows;
  decals.tarmacs.depth = terrain.parallaxDepth;
  decals.tarmacs.tracksMap = terrain.tracksMap;
  decals.tarmacs.field = terrain.tracksField;
  decals.tarmacs.ultra = terrain.ultraGround;
  if (terrain.mudMask != mudMask) coverA_ = coverB_ = {};  // [ground mud] (where plants grow made again: none on the mud)
  terrain.mudMask = mudMask;      // [ground mud] (the terrain draws it in the modern look only)
  terrain.rainGround = rainGround;  // [rain ground] (likewise)
  terrain.snowGround = snowGround;  // [snow ground] (likewise, while snow lies; its drifts by the wind the dust drifts with)
  terrain.mattGrazing = groundMatt;  // [leftover] (likewise)
  terrain.snowWind[0] = dust.wind[0];
  terrain.snowWind[1] = dust.wind[1];
  meshes.Begin();
  placedDraw.Begin();  // [pings] (the models of the CommandFeedback family taken this frame)
  energy.Begin();  // [energy shields]
  // (buildfx) the remastered build look (build_effects.h): the effects list this frame's build beams while it is on, and
  // the models are drawn by their build look's shaders (made the first frame it is on); a frame for something else
  // leaves the sparks under way as they are
  buildNow_ = remasteredBuild && look == Look::Modern && effectsLook == EffectsLook::Remastered && !sidePicture;
  meshes.remasteredGate = remasteredGate && look == Look::Modern && effectsLook == EffectsLook::Remastered;  // [glitches]
  build.Begin(clock, buildNow_, sidePicture);
  meshes.remasteredBuild = buildNow_ && meshes.PrepareBuild();
  // (unitsfx) the real exhausts (exhausts.h): the effects hand this frame's engines' beams to them while they are on
  exhaustsNow_ = realExhausts && look == Look::Modern && !sidePicture;
  exhausts.Begin(clock, exhaustsNow_);
  if (!extraView) PrepareAhead();  // [hitches] ([views] once a frame)
  // (icons and bars at the screen's size: the scene's view with the screen's numbers)
  screenView_ = view;
  if (temporalNow_) screenView_.SetMatrix(motion.Now());  // (icons, bars and what is picked: without the jitter)
  if (screenWidth_ != view.width || screenHeight_ != view.height) {
    screenView_.width = screenWidth_;
    screenView_.height = screenHeight_;
    screenView_.constants.viewport[0] = static_cast<float>(screenWidth_);
    screenView_.constants.viewport[1] = static_cast<float>(screenHeight_);
    screenView_.constants.viewport[2] = 1.0f / static_cast<float>(screenWidth_);
    screenView_.constants.viewport[3] = 1.0f / static_cast<float>(screenHeight_);
  }
  icons.Begin(screenView_);
  // units seen behind what hides them (hidden_outlines.h): marked as they are listed, this frame's
  outlinesNow_ = hiddenOutlines && look == Look::Modern && (!sidePicture || extraView) && (!photo || warmAll) && !camera.cinematic;  // ([session] warmAll)  // ([views] every view's)  // (cinecam: no outlines on photos or a cinematic camera, as no icons)
  outlines.Begin(screenWidth_, screenHeight_);
  // the resource deposits' icons, from where their marks on the ground are no longer drawn (cinecam: not for a
  // cinematic camera, which sees far over the ground: they would stand along the horizon)
  if (depositIcons && !camera.cinematic && !decals.Deposits().empty()) {
    // (small, as the original has them from all the way up: 8 pixels, measured in Chris's picture
    // of it; 16 while two and a half units of ground at the deposit still take that many)
    if (massIcon_[0] == -2) {
      for (int k = 0; k < 2; ++k) {
        massIcon_[k] = icons.Picture("/env/common/splats/mass_strategic.dds", k ? 16 : 8);
        hydrocarbonIcon_[k] = icons.Picture("/env/common/splats/hydrocarbon_strategic.dds", k ? 16 : 8);
      }
    }
    const float perUnit = static_cast<float>(std::max(screenHeight_, 1)) / (2 * std::tan(camera.fovY * 0.5f));  // pixels a world unit takes at distance 1
    for (const Decals::Deposit& d : decals.Deposits()) {
      if (OutsidePlayable(d.position.x, d.position.z)) continue;  // (in the dark beyond the part that can be played)
      const float away = Length(d.position - view.eye), full = std::max(d.cutoff * depositIconFrom, 1.0f);
      const int k = 2.5f * perUnit / std::max(away, 1.0f) >= 16.0f * icons.Pixel() ? 1 : 0;
      const float measure = decals.Measure(d.position, view.eye);  // [decalfade] (as its mark's fade measures it)
      icons.AddUnder(d.hydrocarbon ? hydrocarbonIcon_[k] : massIcon_[k], d.position, Clamp((measure - full * 0.8f) / (full * 0.2f), 0, 1));
    }
  }
  lights.Begin(clock);
  pickables_.clear();
  CpuTimes::Part("scenery listed");
  // the scenery's changes that are due by now, then the scenery
  while (!propEvents_.empty() && static_cast<double>(propEvents_.front().tick) * 0.1 <= clock + 1e-6) {
    ApplyPropEvent(propEvents_.front());
    propEvents_.pop_front();
  }
  if (plantsReact && look == Look::Modern && !sidePicture) SmoothProps(clock);  // [plants] (a felled tree falling: between beats)
  if (hotOn_ || hotBefore_) UpdateHotProps();  // (hot metal: the wrecks that have cooled go back into their groups)
  // [rockhits] rocks that answer fire: their scars eased on (once a frame); switched off, the scarred rocks go back into their
  // groups as they were
  if (!rockHits && !rocks.HeldList().empty()) {
    for (uint32_t i : rocks.HeldList())
      if (i < props_.size() && props_[i].cell >= 0) propCells_[static_cast<size_t>(props_[i].cell)].changed = true;
    rocks.Forget();
  }
  if (!sidePicture) rocksNow_ = rockHits && look == Look::Modern && haveMap_;  // (a picture for something else between: the game's frame's kept)
  if (rocksNow_ && !rocks.Made()) MakeRocks();
  if (rocksNow_ && !extraView) rocks.Update(clock_);
  // (only the squares near what the camera sees: a square counts when it, widened by the largest
  // prop's reach and by room for a shadow thrown into the picture from outside it, is in view)
  propCellsSeen = propsAdded = 0;
  pilesNow_ = blastDebris && look == Look::Modern && haveMap_ && !sidePicture;  // [debrispile] (as the debris is moved: Render)
  meshes.addingScenery = true;  // (trees and rocks: drawn before what is laid on the ground, Render)
  if (!propCells_.empty()) {
    const float margin = propReach_ + 30;
    for (int cz = 0; cz < propCellsZ_; ++cz)
      for (int cx = 0; cx < propCellsX_; ++cx) {
        PropCell& cell = propCells_[static_cast<size_t>(cz) * static_cast<size_t>(propCellsX_) + static_cast<size_t>(cx)];
        if (cell.ofMap.empty() && cell.made.empty()) continue;
        const Vec3 lo{static_cast<float>(cx) * kPropCell - margin, ranges.low - 20, static_cast<float>(cz) * kPropCell - margin};
        const Vec3 hi{static_cast<float>(cx + 1) * kPropCell + margin, ranges.high + 60, static_cast<float>(cz + 1) * kPropCell + margin};
        if (!view.BoxVisible(lo, hi)) continue;
        ++propCellsSeen;
        // (its group with the model renderer, filled anew when a prop of it has changed)
        if (cell.group == 0xffffffffu) {
          cell.group = meshes.AddGroup();
          meshes.SetGroupOrder(cell.group, static_cast<uint32_t>(cz * propCellsX_ + cx));  // [gpuscenery] (the order it is listed in)
        }
        if (cell.changed) {
          meshes.ClearGroup(cell.group);
          for (uint32_t i : cell.ofMap)
            if (!rocks.Held(i)) meshes.AddToGroup(cell.group, props_[i].model, props_[i].world);  // ([rockhits] a scarred rock on its own, below)
          for (uint32_t id : cell.made) {
            auto made = madeProps_.find(id);
            // (a hot wreck is held out while it glows: added on its own with its heat, below)
            // ([debrispile] a wreck drawn as its pile, or its model on its own: ListPiles)
            if (made != madeProps_.end() && made->second.model && !made->second.hot && !made->second.pile)
              meshes.AddToGroup(cell.group, made->second.model, made->second.world);
          }
          cell.changed = false;
        }
        meshes.ListGroup(cell.group);
        propsAdded += static_cast<int>(cell.ofMap.size() + cell.made.size());
      }
  }
  // hot metal: the wrecks held out of their groups while they glow, each with its heat (hot_metal.h, HotMetal::Tint)
  for (uint32_t id : hotProps_) {
    auto made = madeProps_.find(id);
    if (made == madeProps_.end() || !made->second.model) continue;
    if (made->second.pile && pilesNow_) continue;  // [debrispile] (its pile glows instead)
    const PlacedProp& p = made->second;
    Vec3 centre;
    float radius;
    Sphere(p.model, p.world, &centre, &radius);
    const float age = static_cast<float>(clock_ - p.born), seconds = heat.WreckSeconds(radius);
    if (!GlowShown(p, centre)) {  // [fogglow] (a wreck the army never saw the unit of, out of sight: cold, no light, no smoke)
      meshes.Add(p.model, p.world, {1, 1, 1}, 1);
      continue;
    }
    heatTint_ = HotMetal::Tint(age, seconds);
    meshes.Add(p.model, p.world, {1, 1, 1}, 1, nullptr, &heatTint_);
    if (!sidePicture && !extraView) heat.Glowing(id, centre, radius, age, seconds, true);
  }
  ListPiles();  // [debrispile]
  // [rockhits] the scarred rocks held out of their groups, each with its scars (rock_hits.h, RockHits::Tint; the original
  // look's shading passes them over); those taken away since (reclaimed, or under a structure) not
  for (uint32_t i : rocks.HeldList()) {
    if (i >= props_.size() || !props_[i].model || props_[i].cell < 0) continue;
    const Vec3 scars = rocks.Tint(i);
    meshes.Add(props_[i].model, props_[i].world, {1, 1, 1}, 1, nullptr, &scars);
  }
  // the world beyond the map's edge has the map's trees and rocks too: each stands again in the
  // mirrored copies of the map the ground out there is made of, turned as its mirror image is
  // turned (the model itself is not mirrored: that would turn it inside out), as far out as the
  // ground goes. They follow the map's own: a wood felled on the map is felled out there.
  CpuTimes::Part("scenery beyond the edge listed");
  propsBeyond = 0;
  if (terrain.beyondReach > 0 && terrain.beyondCopies && !propCells_.empty()) {  // (surround: none on the sculpted land yet)
    const float w = static_cast<float>(map.width), h = static_cast<float>(map.height), reach = terrain.beyondReach;
    const float margin = propReach_ + 30;
    for (int tz = -1; tz <= 1; ++tz)
      for (int tx = -1; tx <= 1; ++tx) {
        if (!tx && !tz) continue;
        const float sx = tx ? -1.0f : 1.0f, sz = tz ? -1.0f : 1.0f;
        // (a place x of the map lies at ox + sx * x in this copy)
        const float ox = tx ? static_cast<float>(tx) * w + w : 0, oz = tz ? static_cast<float>(tz) * h + h : 0;
        for (int cz = 0; cz < propCellsZ_; ++cz)
          for (int cx = 0; cx < propCellsX_; ++cx) {
            const PropCell& cell = propCells_[static_cast<size_t>(cz) * static_cast<size_t>(propCellsX_) + static_cast<size_t>(cx)];
            if (cell.ofMap.empty()) continue;
            const float xa = ox + sx * static_cast<float>(cx) * kPropCell, xb = ox + sx * static_cast<float>(cx + 1) * kPropCell;
            const float za = oz + sz * static_cast<float>(cz) * kPropCell, zb = oz + sz * static_cast<float>(cz + 1) * kPropCell;
            const Vec3 lo{std::min(xa, xb) - margin, ranges.low - 20, std::min(za, zb) - margin};
            const Vec3 hi{std::max(xa, xb) + margin, ranges.high + 60, std::max(za, zb) + margin};
            // (only a band along the edge, a quarter of the way the ground goes out: farther out the
            // haze has taken most of it, and trees cost; only near the camera, where a tree is more
            // than a dot; and only what the camera sees)
            const float dx = std::max({-hi.x, lo.x - w, 0.0f}), dz = std::max({-hi.z, lo.z - h, 0.0f});
            // ((sky) as wide as it was before the land far off took the ground farther out: BeyondLand::reach)
            const float band = std::min(reach, 0.6f * std::min(w, h)) * 0.25f + margin;
            if (dx * dx + dz * dz > band * band || !view.BoxVisible(lo, hi)) continue;
            const Vec3 nearest{std::clamp(view.eye.x, lo.x, hi.x), std::clamp(view.eye.y, lo.y, hi.y), std::clamp(view.eye.z, lo.z, hi.z)};
            if (Length(nearest - view.eye) > 450) continue;
            for (uint32_t i : cell.ofMap) {
              Mat4 m = props_[i].world;
              const float s[3] = {sx, 1, sz};
              for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) m.m[r][k] *= s[r] * s[k];
              m.m[0][3] = ox + sx * m.m[0][3];
              m.m[2][3] = oz + sz * m.m[2][3];
              meshes.Add(props_[i].model, m, {1, 1, 1});
              ++propsBeyond;
            }
          }
      }
  }
  // (surround) the map's sculpted surround has its own scenery (surroundScenery): the squares of it in view, each as a
  // group listed apart (the scene's passes only; no shadow map, not the card's, not in the ray tracing scene)
  surroundPropsListed = 0;
  meshes.apartPixels = surroundSceneryPixels;
  meshes.apartLod = surroundSceneryLod;
  if (terrain.beyondReach > 0 && !terrain.beyondCopies && surroundScenery && !sidePicture)
    for (SurroundCell& cell : surroundCells_) {
      if (!view.BoxVisible(cell.lo, cell.hi)) continue;
      // (thinned with the distance from the eye, by its nearest point: its three lots out to the far end, half-way, the near end)
      const Vec3 nearest{std::clamp(view.eye.x, cell.lo.x, cell.hi.x), std::clamp(view.eye.y, cell.lo.y, cell.hi.y), std::clamp(view.eye.z, cell.lo.z, cell.hi.z)};
      const float away = Length(nearest - view.eye);
      const float reach[3] = {surroundSceneryTo, 0.5f * (surroundSceneryFrom + surroundSceneryTo), surroundSceneryFrom};
      for (int k = 0; k < 3; ++k) {
        if (cell.props[k].empty() || (surroundSceneryTo > surroundSceneryFrom && away > reach[k])) continue;
        if (cell.group[k] == 0xffffffffu) {
          cell.group[k] = meshes.AddGroup();
          for (uint32_t i : cell.props[k]) meshes.AddToGroup(cell.group[k], surroundProps_[i].model, surroundProps_[i].world);
        }
        meshes.ListGroupApart(cell.group[k]);
        surroundPropsListed += static_cast<int>(cell.props[k].size());
      }
    }
  CpuTimes::Part("frame, ghosts, placed, lines");
  meshes.addingScenery = false;
  // (the original's frame round the map, also with a world that goes on beyond it: Chris, 2026-10-14,
  // "keep the out of bounds thing but just have the mechanical thing")
  if (haveMap_ && frame)
    for (const std::string& border : borders) {
      const bool whole = border.size() > 3 && border.compare(border.size() - 3, 3, ".bp") == 0;
      const Model* model = models.Blueprint(whole ? border : border + ".bp");
      if (!model->ok) continue;
      const Vec3 middle{static_cast<float>(map.width) * 0.5f, groundMiddle_, static_cast<float>(map.height) * 0.5f};
      meshes.Add(model, Mat4::FromTRS(middle, Quat{}, static_cast<float>(map.width) * model->scale), {1, 1, 1});
    }
  // the interface's ghosts, lines and rings
  // ([ghostfaint] the same building queued by several builders comes as one ghost a builder: drawn once. Two see-through
  // ghosts on one place were blended in an order that changed from frame to frame - Chris, 2026-11-06: the green outline
  // "flickers" while one unit of a group still has it in its queue)
  std::vector<const Ghost*> shown;
  for (const Ghost& g : ghosts) {
    bool twin = false;
    for (const Ghost* o : shown)
      twin = twin || (o->blueprint == g.blueprint && std::fabs(o->position.x - g.position.x) < 0.05f &&
                      std::fabs(o->position.z - g.position.z) < 0.05f && std::fabs(o->heading - g.heading) < 0.01f);
    if (!twin) shown.push_back(&g);
  }
  for (const Ghost* gp : shown) {
    const Ghost& g = *gp;
    const Model* model = models.Unit(g.blueprint);
    if (!model->ok) continue;
    const Quat turn{0, std::sin(g.heading * 0.5f), 0, std::cos(g.heading * 0.5f)};
    // ([ghostfaint] a ghost over its finished building - a builder still has it queued, as the original shows it - drawn a
    // hair larger, so that its surfaces never lie at the building's own depth and flicker against it)
    meshes.Add(model, Mat4::FromTRS(g.position, turn, model->scale * 1.01f), {0.6f, 0.6f, 0.6f}, 1, nullptr, &g.tint);
    if (ghostLook == 1) meshes.SeeThroughGhost();  // [glitches]
  }
  // what the script places that is an ordinary model (the others are drawn after the water, placed.h)
  for (const PlacedMesh& p : placed) {
    if (p.hidden || PlacedMeshes::Flat(p.shader)) continue;
    const Model* model = models.Mesh(p.mesh, p.texture, p.shader, p.uniformScale);
    if (!model->ok) continue;
    Mat4 at = Mat4::FromTRS(p.position, p.orientation, model->scale);
    for (int row = 0; row < 3; ++row) {
      at.m[row][0] *= p.scale.x;
      at.m[row][1] *= p.scale.y;
      at.m[row][2] *= p.scale.z;
    }
    meshes.Add(model, at, p.color);
  }
  // the interface's pictures on the ground, for this frame
  for (const UserDecal& d : userDecals) {
    if (d.texture.empty() || d.sizeX <= 0 || d.sizeZ <= 0) continue;
    const gfx::Texture t = textures.Get(d.texture, gfx::Texture{});
    if (!t) continue;
    const float y = haveMap_ ? GroundHeight(Clamp(d.x, 0, static_cast<float>(map.width)), Clamp(d.z, 0, static_cast<float>(map.height))) : 0;
    decals.AddMark(t, {d.x, y, d.z}, d.sizeX, d.sizeZ, 0);
  }
  // (0xAARRGGBB to the debug drawing's 0xAABBGGRR)
  auto swapped = [](uint32_t c) { return (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16); };
  const Vec3 lift{0, 0.15f, 0};
  for (const Line& l : lines) {
    if (l.width > 1.25f && guidePull > 0) {  // [glitches] over the ground cover, with a dark edge
      const uint32_t c = swapped(l.color);  // (the edge: its own colour at a fifth, so a red guide's edge is a dark red)
      const uint32_t dark = 0xff000000u | ((((c >> 16) & 0xffu) / 5) << 16) | ((((c >> 8) & 0xffu) / 5) << 8) | ((c & 0xffu) / 5);
      draw.WideLine(l.a + lift, l.b + lift, dark, l.width + 2, guidePull * 0.97f);
      draw.WideLine(l.a + lift, l.b + lift, swapped(l.color), l.width, guidePull);
    } else if (l.width > 1.25f) draw.WideLine(l.a + lift, l.b + lift, swapped(l.color), l.width);
    else draw.Line(l.a + lift, l.b + lift, swapped(l.color));
  }
  for (const Ring& r : rings) {
    // along the ground, in as many pieces as its size asks for
    const int pieces = std::clamp(static_cast<int>(r.radius * 2), 24, 160);
    Vec3 before;
    for (int i = 0; i <= pieces; ++i) {
      const float a = static_cast<float>(i) / static_cast<float>(pieces) * 6.2831853f;
      Vec3 p{r.centre.x + std::cos(a) * r.radius, 0, r.centre.z + std::sin(a) * r.radius};
      p.y = (haveMap_ ? GroundHeight(Clamp(p.x, 0, static_cast<float>(map.width)), Clamp(p.z, 0, static_cast<float>(map.height))) : r.centre.y) + 0.2f;
      if (i) draw.Line(before, p, swapped(r.color));
      before = p;
    }
  }
  CpuTimes::Part(nullptr);
}

std::vector<Vec3> World::WreckPlaces(size_t most) const {
  std::vector<Vec3> places;
  for (const auto& made : madeProps_) {
    if (places.size() >= most) break;
    const Model* m = made.second.model;
    if (m && !m->lods.empty() && m->lods[0].wreck) places.push_back({made.second.world.m[0][3], made.second.world.m[1][3], made.second.world.m[2][3]});
  }
  return places;
}

int World::PropCellAt(const Mat4& world) const {
  if (propCells_.empty()) return -1;
  const int cx = std::clamp(static_cast<int>(world.m[0][3] / kPropCell), 0, propCellsX_ - 1);
  const int cz = std::clamp(static_cast<int>(world.m[2][3] / kPropCell), 0, propCellsZ_ - 1);
  return cz * propCellsX_ + cx;
}

// Puts a prop that has a model into its square's list (`key`: its index in props_, or a made one's
// entity id), and takes one out again.
void World::ListProp(PlacedProp* prop, uint32_t key, bool made) {
  prop->cell = prop->model ? PropCellAt(prop->world) : -1;
  if (prop->cell < 0) return;
  PropCell& cell = propCells_[static_cast<size_t>(prop->cell)];
  cell.changed = true;
  (made ? cell.made : cell.ofMap).push_back(key);
  // how far it reaches from its place: its bounds' far corner, by the matrix's largest scale
  const Model* m = prop->model;
  float scale = 0;
  for (int k = 0; k < 3; ++k) scale = std::max(scale, Length(Vec3{prop->world.m[0][k], prop->world.m[1][k], prop->world.m[2][k]}));
  propReach_ = std::max(propReach_, std::max(Length(m->boundsMin), Length(m->boundsMax)) * scale);
}

// (surround) The scenery of the map's sculpted surround (World::surroundScenery): the file its json names, in the map's
// own decor format, read and placed the first time it is wanted. Each piece by the blueprint the season draws it in, as
// the map's props; on the surround's ground as it is drawn (the file has the heights of the surround as it came: within
// the strip joined to the map's edge the ground was moved, Surround::JoinMoved); nothing inside the map (a piece the
// file has there is left out: the map's own props stand there). Listed by squares, each with its box.
void World::ReadSurroundScenery() {
  surroundSceneryTried_ = true;
  const std::string& path = terrain.mapPath;
  if (surround.Decor().empty() || path.find('/') == std::string::npos) return;
  const auto started = std::chrono::steady_clock::now();
  const std::string name = path.substr(0, path.find_last_of('/') + 1) + surround.Decor();
  std::vector<MapProp> list;
  std::string why;
  const auto file = vfs_->ReadFile(name);
  if (!file || !LoadDecor(*file, &list, &why)) {
    fprintf(stderr, "render: the surround's scenery %s: %s (none is drawn)\n", name.c_str(), file ? why.c_str() : "no such file");
    surround.LetGoAsCame();
    return;
  }
  const SeasonLook& seasonal = terrain.SeasonOf();
  {  // (their models' files read on the worker threads first, all together)
    std::vector<std::string> kinds;
    std::unordered_set<std::string> seen;
    for (const MapProp& p : list)
      if (seen.insert(p.blueprint).second) kinds.push_back(seasonal.Prop(p.blueprint));
    models.PrefetchBlueprints(kinds);
  }
  const float w = static_cast<float>(map.width), h = static_cast<float>(map.height);
  std::unordered_map<std::string, const Model*> kindModels;
  std::unordered_map<uint64_t, uint32_t> cellOf;  // (a square's number among surroundCells_, by its column and row)
  int inside = 0, unread = 0, trees = 0;
  float farthestTree = 0;
  for (const MapProp& p : list) {
    if (p.position.x >= 0 && p.position.x <= w && p.position.z >= 0 && p.position.z <= h) {
      ++inside;
      continue;
    }
    auto known = kindModels.find(p.blueprint);
    const Model* model = known != kindModels.end() ? known->second : kindModels.emplace(p.blueprint, models.Blueprint(seasonal.Prop(p.blueprint))).first->second;
    if (!model->ok) {
      ++unread;
      continue;
    }
    const Vec3 s = p.scale * model->scale;
    const float y = p.position.y + surround.JoinMoved(p.position.x, p.position.z);
    PlacedProp placed{model, Mat4{{{p.axisX.x * s.x, p.axisY.x * s.y, p.axisZ.x * s.z, p.position.x},
                                   {p.axisX.y * s.x, p.axisY.y * s.y, p.axisZ.y * s.z, y},
                                   {p.axisX.z * s.x, p.axisY.z * s.y, p.axisZ.z * s.z, p.position.z},
                                   {0, 0, 0, 1}}}};
    const int cx = static_cast<int>(std::floor(p.position.x / kSurroundCell)), cz = static_cast<int>(std::floor(p.position.z / kSurroundCell));
    const uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32 | static_cast<uint32_t>(cz);
    auto found = cellOf.find(key);
    if (found == cellOf.end()) {
      found = cellOf.emplace(key, static_cast<uint32_t>(surroundCells_.size())).first;
      surroundCells_.push_back({{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, {}, {0xffffffffu, 0xffffffffu, 0xffffffffu}});
    }
    SurroundCell& cell = surroundCells_[found->second];
    Vec3 centre;
    float radius;
    Sphere(model, placed.world, &centre, &radius);
    cell.lo = {std::min(cell.lo.x, centre.x - radius), std::min(cell.lo.y, centre.y - radius), std::min(cell.lo.z, centre.z - radius)};
    cell.hi = {std::max(cell.hi.x, centre.x + radius), std::max(cell.hi.y, centre.y + radius), std::max(cell.hi.z, centre.z + radius)};
    // (which lot: by its number, scattered, the same every time)
    cell.props[(static_cast<uint32_t>(surroundProps_.size()) * 2654435761u >> 13) % 3].push_back(static_cast<uint32_t>(surroundProps_.size()));
    surroundProps_.push_back(placed);
    // (a tree, as the map's are told: it sways and stands a world unit high or more)
    const float tall = (model->boundsMax.y - model->boundsMin.y) * Length(Vec3{placed.world.m[0][1], placed.world.m[1][1], placed.world.m[2][1]});
    if (!model->lods.empty() && model->lods[0].sway > 0 && tall >= 1.0f) {
      ++trees;
      const float dx = std::max({-p.position.x, p.position.x - w, 0.0f}), dz = std::max({-p.position.z, p.position.z - h, 0.0f});
      farthestTree = std::max(farthestTree, std::sqrt(dx * dx + dz * dz));
    }
  }
  surround.LetGoAsCame();
  textures.DropFetched();
  // (where the trees thin out: the visuals session's tool sets them thick over the first 36 hundredths of the way to where
  // the last stands, 150 of 420 world units, and fewer and fewer from there)
  surroundTrees_[0] = 0.36f * farthestTree;
  surroundTrees_[1] = farthestTree;
  surround.TreesGiven(&surroundTrees_[0], &surroundTrees_[1]);  // (the json's own "decor_trees", where it says)
  printf("render: the surround's scenery: %zu pieces of %zu kinds in %zu squares from %s, %d of them trees out to %.0f world units past the edge"
         "%s (read and placed in %.0f ms)\n",
         surroundProps_.size(), kindModels.size(), surroundCells_.size(), name.c_str(), trees, farthestTree,
         inside || unread ? (", " + std::to_string(inside) + " inside the map and " + std::to_string(unread) + " whose model cannot be read left out").c_str() : "",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

// [surahead] The surround read ahead is taken (waited for, where its thread is not done): from here the surround is the
// frame's own again. Says how long the reading took beside the load and how long this waited.
void World::TakeSurroundAhead() {
  if (!surroundAhead_.valid()) return;
  const auto started = std::chrono::steady_clock::now();
  const SurroundAhead r = surroundAhead_.get();
  if (!r.found) return;
  if (r.upper) surround.SetUpper(r.upperOf);
  printf("render: the map's surround: read ahead beside the load in %.0f ms (its upper layer %s); the first frame waited %.1f ms for it\n", r.ms,
         r.upper ? "with it" : "not", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

// (surround) The map before is gone: its surround's scenery with it (their groups emptied; looked for again when wanted).
void World::ForgetSurroundScenery() {
  for (SurroundCell& cell : surroundCells_)
    for (uint32_t group : cell.group)
      if (group != 0xffffffffu) meshes.ClearGroup(group);
  surroundCells_.clear();
  surroundProps_.clear();
  surroundSceneryTried_ = false;
  surroundTrees_[0] = surroundTrees_[1] = 0;
}

void World::UnlistProp(PlacedProp* prop, uint32_t key, bool made) {
  if (prop->cell < 0 || static_cast<size_t>(prop->cell) >= propCells_.size()) return;
  PropCell& cell = propCells_[static_cast<size_t>(prop->cell)];
  cell.changed = true;
  std::vector<uint32_t>& list = made ? cell.made : cell.ofMap;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == key) {
      list[i] = list.back();
      list.pop_back();
      break;
    }
  prop->cell = -1;
}

// [underfoot] Chris's second test (2026-11-04, his picture "foliage shouldn't be glitching into a structure. If a structure is
// built the foliage below should be removed"): a bush's leaves stood out of a building's walls. They were the map's
// render-only props (its decor: the plants, small rocks and branches kept out of the .scmap so that the simulation never
// makes them, scmap.h LoadDecor; on SETONS_R 43,326 of its 55,172), which nothing ever took away: the simulation does not
// know them, and the ground cover's pressing (grass.cpp, kTrample) is for the grass's cards and 3D plants alone. A
// structure (IsStructure: its blueprint's MotionType none) that stands or is being built now takes those whose place lies
// within its model's box on the ground, turned as it stands and kUnderfootMargin wider all round, out of their squares'
// lists the first frame it is listed (as one gone is taken out, ApplyPropEvent: its square's group filled anew), and
// gives them back once it is no longer in the frame (ReleaseClearings), unless another structure still stands over them:
// each prop counts the structures over it (a mass extractor upgraded: the new one is built over the old, which goes when
// it is done). Only once a structure: a structure does not move. A stream's water is drawn by its own pass, whatever stands
// on it. [simunder] (Chris's third test, 2026-11-05: plants still poked into structures) A prop is taken when its leaves
// reach into the box, not only when its root stands in it (kLeafReach: a bush rooted beside a mass extractor in his big test's
// game spread its leaves over it); and the simulation's own props (the .scmap's trees and bushes, reclaimable or not) are
// taken out of the picture the same way, the simulation keeping them. A change the game makes to one taken (felled, burnt,
// reclaimed: ApplyPropEvent) hands it back first (ForgetUnderfoot), and the game's word stands.
namespace {
constexpr float kUnderfootMargin = 0.25f;  // world units round a structure's box: a bush rooted at its very edge goes too
constexpr float kLeafShare = 0.6f, kLeafReachMost = 2.0f;  // [simunder] a prop's leaves reach this share of its width, at most this far
}  // namespace

void World::ClearUnderStructure(uint32_t id, const Model* model, Vec3 position, const Quat& orientation) {
  if (!clearUnderStructures || props_.empty() || propCells_.empty() || !model || !model->ok) return;  // ([simunder] the .scmap's props too)
  auto [entry, fresh] = clearings_.try_emplace(id);
  entry->second.stamp = frameStamp_;
  if (!fresh) return;
  // its box on the ground: its bounds' middle, its two ways along the ground and half its size along each, in the world
  const float scale = model->scale;
  const Vec3 lo = model->boundsMin * scale, hi = model->boundsMax * scale;
  const Vec3 middle = position + Rotate(orientation, Vec3{0.5f * (lo.x + hi.x), 0, 0.5f * (lo.z + hi.z)});
  const Vec3 across = Rotate(orientation, Vec3{1, 0, 0}), along = Rotate(orientation, Vec3{0, 0, 1});
  const float acrossFlat = std::sqrt(across.x * across.x + across.z * across.z), alongFlat = std::sqrt(along.x * along.x + along.z * along.z);
  if (acrossFlat < 1e-4f || alongFlat < 1e-4f) return;  // (on its side: nothing to stand over)
  const float ax = across.x / acrossFlat, az = across.z / acrossFlat, fx = along.x / alongFlat, fz = along.z / alongFlat;
  const float halfAcross = 0.5f * (hi.x - lo.x) * acrossFlat + kUnderfootMargin, halfAlong = 0.5f * (hi.z - lo.z) * alongFlat + kUnderfootMargin;
  // (the squares its box reaches, by its corners' extent on the map)
  const float reachX = std::fabs(ax) * halfAcross + std::fabs(fx) * halfAlong + kLeafReachMost,
              reachZ = std::fabs(az) * halfAcross + std::fabs(fz) * halfAlong + kLeafReachMost;  // ([simunder] and leaves reaching in)
  const int cx0 = std::clamp(static_cast<int>((middle.x - reachX) / kPropCell), 0, propCellsX_ - 1);
  const int cz0 = std::clamp(static_cast<int>((middle.z - reachZ) / kPropCell), 0, propCellsZ_ - 1);
  const int cx1 = std::clamp(static_cast<int>((middle.x + reachX) / kPropCell), 0, propCellsX_ - 1);
  const int cz1 = std::clamp(static_cast<int>((middle.z + reachZ) / kPropCell), 0, propCellsZ_ - 1);
  std::vector<uint32_t>& taken = entry->second.props;
  for (int cz = cz0; cz <= cz1; ++cz)
    for (int cx = cx0; cx <= cx1; ++cx) {
      const PropCell& cell = propCells_[static_cast<size_t>(cz) * static_cast<size_t>(propCellsX_) + static_cast<size_t>(cx)];
      // (those still standing, and those another structure has taken already: this one stands over them too)
      for (const std::vector<uint32_t>* list : {&cell.ofMap, &cell.underfoot})
        for (uint32_t i : *list) {
          if (i >= props_.size()) continue;
          const PlacedProp& p = props_[i];
          if (!p.model || p.model->lods.empty() || p.model->lods[0].stream) continue;
          const float dx = p.world.m[0][3] - middle.x, dz = p.world.m[2][3] - middle.z;
          // ([simunder] how far its leaves reach round its root: a share of its own half width on the ground)
          const Vec3 b0 = p.model->boundsMin, b1 = p.model->boundsMax;
          const float sx = std::sqrt(p.world.m[0][0] * p.world.m[0][0] + p.world.m[1][0] * p.world.m[1][0] + p.world.m[2][0] * p.world.m[2][0]);
          const float sz = std::sqrt(p.world.m[0][2] * p.world.m[0][2] + p.world.m[1][2] * p.world.m[1][2] + p.world.m[2][2] * p.world.m[2][2]);
          const float half = std::max(std::max(std::fabs(b0.x), std::fabs(b1.x)) * sx, std::max(std::fabs(b0.z), std::fabs(b1.z)) * sz);
          const float leaf = std::min(half * kLeafShare, kLeafReachMost);
          if (std::fabs(dx * ax + dz * az) <= halfAcross + leaf && std::fabs(dx * fx + dz * fz) <= halfAlong + leaf) taken.push_back(i);
        }
    }
  for (uint32_t i : taken) {
    if (decorUnder_[i]++ > 0) continue;  // (out already, under another)
    const int c = props_[i].cell;
    UnlistProp(&props_[i], i, false);
    if (c >= 0) propCells_[static_cast<size_t>(c)].underfoot.push_back(i);
  }
}

// [simunder] A map's prop taken away under a structure, handed back (the game changes it): out of the count and of its
// square's list of those taken; the structures' own lists still name it, which ReleaseClearings then passes over.
void World::ForgetUnderfoot(uint32_t i) {
  auto under = decorUnder_.find(i);
  if (under == decorUnder_.end()) return;
  decorUnder_.erase(under);
  const int c = PropCellAt(props_[i].world);
  if (c < 0) return;
  std::vector<uint32_t>& list = propCells_[static_cast<size_t>(c)].underfoot;
  for (size_t k = 0; k < list.size(); ++k)
    if (list[k] == i) {
      list[k] = list.back();
      list.pop_back();
      break;
    }
}

// [mapfx] The map's looping effects started where they are not running (the first frame, and after the particles were
// cleared), at their height over the ground there, each looping for good (its LIFETIME -1).
void World::KeepMapEffects() {
  if (mapEffects_.empty() || !haveMap_) return;
  for (MapEffect& fx : mapEffects_) {
    if (fx.handle && particles.Running(fx.handle)) continue;
    const Vec3 at{fx.at.x, map.Height(fx.at.x, fx.at.z) + fx.at.y, fx.at.z};
    fx.handle = particles.Start(fx.emitter, at, fx.turn, fx.scale, clock_);
    if (fx.handle) particles.SetParam(fx.handle, "LIFETIME", -1);
  }
}

void World::ReleaseClearings() {
  for (auto it = clearings_.begin(); it != clearings_.end();) {
    if (it->second.stamp == frameStamp_) {
      ++it;
      continue;
    }
    for (uint32_t i : it->second.props) {
      auto under = decorUnder_.find(i);
      if (under == decorUnder_.end() || --under->second > 0) continue;
      decorUnder_.erase(under);
      const int c = PropCellAt(props_[i].world);
      if (c >= 0) {
        std::vector<uint32_t>& list = propCells_[static_cast<size_t>(c)].underfoot;
        for (size_t k = 0; k < list.size(); ++k)
          if (list[k] == i) {
            list[k] = list.back();
            list.pop_back();
            break;
          }
      }
      if (props_[i].model) ListProp(&props_[i], i, false);  // ([simunder] not one the game has taken away since)
    }
    it = clearings_.erase(it);
  }
}

void World::ChangeGround(const std::vector<FrameGroundChange>& changes) {
  // (the game's scripts lay a structure's ground flat a grid point at a time: the changes near each
  // other are sent to the ground in one rectangle, a few rectangles a frame instead of hundreds)
  struct Rect {
    int x0, z0, x1, z1;
  };
  std::vector<Rect> dirty;
  constexpr int kNear = 8;  // grid points apart that still join one rectangle
  for (const FrameGroundChange& c : changes) {
    // (the map's grid points, as the simulation keeps them: whole steps of the height scale)
    const int x0 = std::max(c.x0, 0), z0 = std::max(c.z0, 0), x1 = std::min(c.x1, map.width), z1 = std::min(c.z1, map.height);
    const int across = c.x1 - c.x0 + 1;
    if (x0 > x1 || z0 > z1 || across <= 0 || c.heights.size() < static_cast<size_t>(across) * static_cast<size_t>(c.z1 - c.z0 + 1)) continue;
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        const float h = c.heights[static_cast<size_t>(z - c.z0) * static_cast<size_t>(across) + static_cast<size_t>(x - c.x0)];
        map.heights[static_cast<size_t>(z) * static_cast<size_t>(map.width + 1) + static_cast<size_t>(x)] =
            static_cast<uint16_t>(std::clamp(h / map.heightScale, 0.0f, 65535.0f));
      }
    bool joined = false;
    for (Rect& d : dirty)
      if (x0 <= d.x1 + kNear && x1 >= d.x0 - kNear && z0 <= d.z1 + kNear && z1 >= d.z0 - kNear) {
        d = {std::min(d.x0, x0), std::min(d.z0, z0), std::max(d.x1, x1), std::max(d.z1, z1)};
        joined = true;
        break;
      }
    if (!joined) dirty.push_back({x0, z0, x1, z1});
    ++groundChanges;
  }
  for (const Rect& d : dirty) {
    terrain.HeightsChanged(d.x0, d.z0, d.x1, d.z1);
    // [lightshade] (the ground tables kept of lights that reach there are of the ground as it was)
    lights.GroundChanged(static_cast<float>(d.x0), static_cast<float>(d.z0), static_cast<float>(d.x1), static_cast<float>(d.z1));
  }
}

void World::AddCombat(FrameCombat&& combat) {
  hitsTaken += static_cast<int>(combat.hits.size());
  impactsTaken += static_cast<int>(combat.impacts.size());
  FrameCombat& p = combatPending_;
  p.hitsKept = combat.hitsKept;
  p.impactsKept = combat.impactsKept;
  p.healthKept = combat.healthKept;
  // (a host that never adds frames - a menu's picture - must not let them pile up without end)
  constexpr size_t kMost = 200000;
  if (p.hits.size() + combat.hits.size() > kMost || p.impacts.size() + combat.impacts.size() > kMost || p.health.size() + combat.health.size() > kMost) {
    p.hits.clear();
    p.impacts.clear();
    p.health.clear();
  }
  for (FrameHit& h : combat.hits) {
    if (h.killing && h.targetKind == 0) debris.Killed(h.target, h);  // [debrisstyle] (which death its baked set plays)
    p.hits.push_back(std::move(h));
  }
  for (FrameImpact& m : combat.impacts) p.impacts.push_back(std::move(m));
  for (const FrameHealth& h : combat.health) p.health.push_back(h);
}

// Moves what AddCombat gave whose beat the moment shown has reached into combat_, each list's order kept.
void World::TakeDueCombat(double shown) {
  auto take = [shown](auto& from, auto& to) {
    size_t kept = 0;
    for (size_t i = 0; i < from.size(); ++i) {
      if (static_cast<double>(from[i].tick) <= shown + 1e-6) to.push_back(std::move(from[i]));
      else if (kept != i) from[kept++] = std::move(from[i]);
      else ++kept;
    }
    from.resize(kept);
  };
  take(combatPending_.hits, combat_.hits);
  take(combatPending_.impacts, combat_.impacts);
  take(combatPending_.health, combat_.health);
  hitsShown += static_cast<int>(combat_.hits.size());
  impactsShown += static_cast<int>(combat_.impacts.size());
  combat_.hitsKept = combatPending_.hitsKept;
  combat_.impactsKept = combatPending_.impactsKept;
  combat_.healthKept = combatPending_.healthKept;
}

void World::AddPropEvents(const std::vector<FramePropEvent>& events) {
  for (const FramePropEvent& e : events) propEvents_.push_back(e);
}

bool World::PileKind(const std::string& unitId) {  // [debrispile]
  debris.baked.files = vfs_;
  debris.baked.models = &models;
  debris.baked.heat = &heat;
  // ([debrishull] or a water structure's wreck: drawn as its hull without the thrown parts, those lying round it)
  return debris.baked.HasPile(unitId) || debris.baked.HullOf(unitId);
}

// The model of a prop as the simulation has it now: its blueprint's own, or the mesh a script set
// in its place. The game makes a burnt version of every mesh under the name "<mesh>_wreck": a dead
// unit's wreck is the prop DefaultWreckage with "/units/<id>/<id>_mesh_wreck", a burnt tree its
// own blueprint with its own mesh's wreck.
const Model* World::PropModel(const std::string& given, const std::string& mesh) {
  // [seasons] drawn by the season's blueprint where the season lists one (the simulation's events name the map's own)
  const std::string blueprint = terrain.SeasonOf().Prop(given);
  if (mesh.empty()) return models.Blueprint(blueprint);
  std::string low = mesh;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (low.size() > 6 && low.compare(low.size() - 6, 6, "_wreck") == 0) {
    std::string of = blueprint;
    for (char& c : of) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (of.find("defaultwreckage") == std::string::npos) return models.Wreck(models.Blueprint(blueprint), true);
    const size_t slash = low.rfind('/');
    std::string id = low.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t end = id.rfind("_mesh_wreck");
    if (end != std::string::npos) id.resize(end);
    if (const Model* own = models.RemasteredWreck(id)) return own;  // (a remastered unit's own wreck, as painted)
    return models.Wreck(models.Unit(id), false);
  }
  const Model* named = vfs_->Exists(moho::Vfs::Normalize(mesh + ".bp")) ? models.Blueprint(terrain.SeasonOf().Prop(mesh + ".bp")) : nullptr;
  return named && named->ok ? named : models.Blueprint(blueprint);
}

bool World::IsProp(uint32_t id) const {
  if (madeProps_.count(id) || recentProps_.count(id)) return true;
  for (const FramePropEvent& e : propEvents_)
    if (e.id == id) return true;
  return false;
}

void World::ApplyPropEvent(const FramePropEvent& e) {
  const bool ofMap = e.mapIndex >= 0;
  {  // [scars] (remembered a little while: a flash made on it in the same beat is no explosion, IsProp)
    const double when = static_cast<double>(e.tick) * 0.1;
    recentProps_[e.id] = when;
    if (when > recentPruned_ + 5 || when < recentPruned_ - 5) {  // (those older than ten seconds forgotten, every five)
      recentPruned_ = when;
      for (auto it = recentProps_.begin(); it != recentProps_.end();) it = std::fabs(it->second - when) > 10 ? recentProps_.erase(it) : std::next(it);
    }
  }
  if (ofMap && static_cast<size_t>(e.mapIndex) >= props_.size()) return;
  {  // [devicelost] (Keep) what the simulation last said of this prop, for a world made anew
    const uint64_t key = ofMap ? (1ull << 32) | static_cast<uint32_t>(e.mapIndex) : e.id;
    if (e.kind == FramePropEvent::Destroy && !ofMap) {
      propsKept_.erase(key);  // (one made in the game and gone again: nothing to say of it)
    } else {
      auto [it, fresh] = propsKept_.try_emplace(key);
      const uint32_t madeAt = it->second.tick;
      it->second = e;
      // (one made in the game: told again as its making, at the beat it was made - a wreck's age is by it)
      if (!ofMap && e.kind != FramePropEvent::Destroy) {
        it->second.kind = FramePropEvent::Create;
        if (!fresh) it->second.tick = madeAt;
      }
    }
  }
  if (ofMap) ForgetUnderfoot(static_cast<uint32_t>(e.mapIndex));  // [simunder] (the game's change first: it is its prop)
  // (out of its square's list first; it is listed again where it is now)
  if (ofMap) {
    UnlistProp(&props_[static_cast<size_t>(e.mapIndex)], static_cast<uint32_t>(e.mapIndex), false);
  } else {
    auto had = madeProps_.find(e.id);
    if (had != madeProps_.end()) UnlistProp(&had->second, e.id, true);
  }
  if (e.kind == FramePropEvent::Destroy) {
    if (!ofMap && plantsReact) {  // [scars] a unit's wreck gone (reclaimed, shot to pieces): what burnt round it goes (PlantLife::Heal)
      auto had = madeProps_.find(e.id);
      if (had != madeProps_.end() && had->second.unitWreck && had->second.model) {
        Vec3 centre;
        float radius;
        Sphere(had->second.model, had->second.world, &centre, &radius);
        // (its death's burn, AddWreck's, and a margin for its fires' burns about it)
        plantLife.Heal(centre.x, centre.z, (0.3f + 1.4f * std::min(radius, 12.0f)) * 1.15f + 1.5f, static_cast<double>(e.tick) * 0.1);
      }
    }
    if (ofMap) props_[static_cast<size_t>(e.mapIndex)].model = nullptr;
    else madeProps_.erase(e.id), pileProps_.erase(e.id);  // ([debrispile] its pile goes with it: ListPiles tells it no more)
    ++propsGone;
    return;
  }
  const Model* model = PropModel(e.blueprint, e.mesh);
  PlacedProp placed{model && model->ok ? model : nullptr, Mat4::Identity()};
  if (placed.model) {
    placed.world = Mat4::FromTRS({e.position[0], e.position[1], e.position[2]}, Quat{e.orientation[0], e.orientation[1], e.orientation[2], e.orientation[3]}, model->scale);
    for (int row = 0; row < 3; ++row)
      for (int k = 0; k < 3; ++k) placed.world.m[row][k] *= e.scale[k];
  }
  if (ofMap) {
    props_[static_cast<size_t>(e.mapIndex)] = placed;
    ListProp(&props_[static_cast<size_t>(e.mapIndex)], static_cast<uint32_t>(e.mapIndex), false);
  } else {
    PlacedProp& kept = madeProps_[e.id];
    // (hot metal: when it appeared, kept through its changes, and whether it is a unit's wreck: the game's
    // DefaultWreckage with the unit's "_mesh_wreck")
    const double born = kept.born >= 0 ? kept.born : static_cast<double>(e.tick) * 0.1;
    const bool hot = kept.hot, glowKnown = kept.glowKnown;  // ([fogglow] kept through its changes)
    kept = placed;
    kept.born = born;
    kept.hot = hot;
    std::string low = e.blueprint + "|" + e.mesh;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    kept.wreck = low.find("defaultwreckage") != std::string::npos || low.find("_mesh_wreck") != std::string::npos;
    kept.unitWreck = low.find("defaultwreckage") != std::string::npos;  // [groundfix]
    // [debrispile] a unit's wreck whose kind has a baked set (its mesh "/units/<id>/<id>_mesh_wreck"): drawn as its pile
    kept.pile = false;
    if (kept.unitWreck && kept.model) {
      const size_t slash = low.rfind('/'), end = low.rfind("_mesh_wreck");
      const std::string unit = slash != std::string::npos && end != std::string::npos && end > slash ? low.substr(slash + 1, end - slash - 1) : std::string();
      kept.pile = !unit.empty() && PileKind(unit);
      if (kept.pile) pileProps_[e.id] = unit;
    }
    if (!kept.pile) pileProps_.erase(e.id);
    // [fogglow] (made now: whether its unit was ever seen; a change keeps what was decided)
    if (e.kind == FramePropEvent::Create && kept.unitWreck && kept.model && sight.On()) {
      Vec3 centre;
      float radius;
      Sphere(kept.model, kept.world, &centre, &radius);
      kept.glowKnown = WreckKnown(centre, radius, born);
    } else if (e.kind != FramePropEvent::Create) {
      kept.glowKnown = glowKnown;
    }
    // [plants] (a unit's fresh wreck: its death burnt the ground round it; a burnt tree's mesh is no unit's wreck. Only
    // where a death's explosion went off there just before: the map's own scenery wrecks the game makes as it starts,
    // or later, are old and burnt nothing)
    if (plantsReact && kept.model && e.kind == FramePropEvent::Create && low.find("defaultwreckage") != std::string::npos) {
      Vec3 centre;
      float radius;
      Sphere(kept.model, kept.world, &centre, &radius);
      bool died = false;
      for (const Blasts::Blast& b : blasts.Recent()) {
        const float dx = b.at.x - centre.x, dz = b.at.z - centre.z, reach = radius + b.size + 2;
        if (b.hot && b.born >= born - 6 && b.born <= born + 1 && dx * dx + dz * dz <= reach * reach) died = true;
      }
      if (died) plantLife.AddWreck(centre.x, centre.z, radius, born);
    }
    ListProp(&kept, e.id, true);
    if (hotOn_) HoldHot(e.id, &kept);
  }
  ++propsChanged;
}

// [plants] A prop the game moves beat by beat (a felled tree falls over in three to fifteen beats, and sinks away later:
// src/sim/prop_motor.cpp) is drawn between where the last beat left it and where the next beat has it (told already,
// not yet due), as the units are, instead of jumping ten times a second. Its square's group is filled anew each frame it
// moves; the next beat's change, when due, puts it exactly where the game has it.
void World::SmoothProps(double clock) {
  // (a matrix's rotation as a quaternion, its scale taken out of each column)
  auto turnOf = [](const Mat4& m, Vec3* scale) {
    Vec3 c[3];
    for (int k = 0; k < 3; ++k) {
      const Vec3 axis{m.m[0][k], m.m[1][k], m.m[2][k]};
      const float s = std::max(Length(axis), 1e-8f);
      (&scale->x)[k] = s;
      c[k] = axis * (1 / s);
    }
    Quat q;
    const float trace = c[0].x + c[1].y + c[2].z;
    if (trace > 0) {
      const float s = std::sqrt(trace + 1) * 2;
      q = {(c[1].z - c[2].y) / s, (c[2].x - c[0].z) / s, (c[0].y - c[1].x) / s, 0.25f * s};
    } else if (c[0].x > c[1].y && c[0].x > c[2].z) {
      const float s = std::sqrt(1 + c[0].x - c[1].y - c[2].z) * 2;
      q = {0.25f * s, (c[1].x + c[0].y) / s, (c[2].x + c[0].z) / s, (c[1].z - c[2].y) / s};
    } else if (c[1].y > c[2].z) {
      const float s = std::sqrt(1 + c[1].y - c[0].x - c[2].z) * 2;
      q = {(c[1].x + c[0].y) / s, 0.25f * s, (c[2].y + c[1].z) / s, (c[2].x - c[0].z) / s};
    } else {
      const float s = std::sqrt(1 + c[2].z - c[0].x - c[1].y) * 2;
      q = {(c[2].x + c[0].z) / s, (c[2].y + c[1].z) / s, 0.25f * s, (c[0].y - c[1].x) / s};
    }
    return q;
  };
  for (const FramePropEvent& e : propEvents_) {
    const double due = static_cast<double>(e.tick) * 0.1;
    if (due - clock > 0.1 + 1e-6) break;  // (the list is in the beats' order: only the next beat's)
    if (e.kind != FramePropEvent::Change) continue;
    PlacedProp* p = nullptr;
    if (e.mapIndex >= 0) {
      if (static_cast<size_t>(e.mapIndex) < props_.size()) p = &props_[static_cast<size_t>(e.mapIndex)];
    } else {
      auto made = madeProps_.find(e.id);
      if (made != madeProps_.end()) p = &made->second;
    }
    if (!p || !p->model) continue;
    // (where the last beat left it: kept the first frame it is drawn toward this beat, before it is moved)
    const int64_t key = e.mapIndex >= 0 ? e.mapIndex : -1 - static_cast<int64_t>(e.id);
    PropBeat& last = propBeats_[key];
    if (last.tick != e.tick) last = {e.tick, p->world};
    Mat4 next = Mat4::FromTRS({e.position[0], e.position[1], e.position[2]}, Quat{e.orientation[0], e.orientation[1], e.orientation[2], e.orientation[3]},
                              p->model->scale);
    for (int row = 0; row < 3; ++row)
      for (int k = 0; k < 3; ++k) next.m[row][k] *= e.scale[k];
    const float t = static_cast<float>(std::clamp(1 - (due - clock) / 0.1, 0.0, 1.0));
    Vec3 scaleA, scaleB;
    const Quat a = turnOf(last.at, &scaleA), b = turnOf(next, &scaleB);
    // (a step too small to see - sinking away, a hundredth of a unit a beat - is left to the beats: its square's group
    // would be filled anew every frame for nothing)
    const float turned = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    const float dx = next.m[0][3] - last.at.m[0][3], dy = next.m[1][3] - last.at.m[1][3], dz = next.m[2][3] - last.at.m[2][3];
    if (turned > 0.99999f && dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) continue;
    Mat4 now = Mat4::FromTRS({last.at.m[0][3] + (next.m[0][3] - last.at.m[0][3]) * t, last.at.m[1][3] + (next.m[1][3] - last.at.m[1][3]) * t,
                              last.at.m[2][3] + (next.m[2][3] - last.at.m[2][3]) * t},
                             Nlerp(a, b, t));
    const Vec3 scale = scaleA + (scaleB - scaleA) * t;
    for (int row = 0; row < 3; ++row)
      for (int k = 0; k < 3; ++k) now.m[row][k] *= (&scale.x)[k];
    p->world = now;
    if (p->cell >= 0) propCells_[static_cast<size_t>(p->cell)].changed = true;
  }
  for (auto it = propBeats_.begin(); it != propBeats_.end();)  // (those long done)
    it = static_cast<double>(it->second.tick) * 0.1 < clock - 1 ? propBeats_.erase(it) : std::next(it);
}

void World::WatchWear(const Frame& frame) {
  wear.scars = unitScars;
  wear.snow = unitSnow;
  wear.dirt = unitDirt;  // [unit dirt]
  wear.grime = unitGrime, wear.damage = unitDamage;  // [grime]
  if (unitScars) models.hitShapes = true;  // (models loaded from now on keep their triangles for where hits lie)
  if (unitGrime || unitDamage) models.wearArt = true;  // [grime] (and their wear art: unit_grime.h)
  if (look != Look::Modern || !(unitScars || unitSnow || unitDirt || unitGrime || unitDamage)) return;
  const bool weather = timeOfDay >= 0;  // (the weather is only there with a time of day)
  auto modelOf = [&](uint32_t blueprint, bool make) -> const Model* {
    if (!frame.blueprints || blueprint >= frame.blueprints->size() || (*frame.blueprints)[blueprint].empty()) return nullptr;
    // (the models the units are drawn with, as AddFrame makes them; one made here only for a unit hit before it is drawn)
    if (!make && (blueprint >= unitModels_.size() || !unitModels_[blueprint].known)) return nullptr;
    const Model* m = Known(unitModels_, blueprint, frame.blueprints->size(), [&] { return models.Unit((*frame.blueprints)[blueprint]); });
    return m && m->ok ? m : nullptr;
  };
  wear.frostLying = weather ? snowLying : 0.0f, wear.groundWet = weather ? wet : 0.0f;  // [grime]
  if (unitScars || unitSnow || unitGrime || unitDamage) wear.Watch(frame, clock_, weather ? snow : 0.0f, weather ? snowLying : 0.0f, modelOf);
  if (unitDirt || unitGrime) {  // [unit dirt] each unit's wetness, mud and dust, by the ground under it and the weather (unit_wear.h; [grime] its grime's too)
    if (!wear.HasGround() && haveMap_) {
      float kinds[10][6];
      for (int i = 0; i < 10; ++i) std::copy(terrain.LayerKind(i), terrain.LayerKind(i) + 6, kinds[i]);
      wear.SetGround(map, kinds);
    }
    UnitWear::DirtWeather w;
    if (weather) {
      w.rain = rain;
      w.wet = wet;
      w.snowLying = snowLying;
      // (the sun on it: by day, as much as the clouds let through)
      const float cover = std::max({cloudCover, SetIn(rain) * (0.8f + 0.2f * rain), SetIn(snow) * 0.75f, fog * 0.6f});  // ([onset])
      w.sun = std::clamp(skyLight.daylight, 0.0f, 1.0f) * (1 - std::clamp(cover, 0.0f, 1.0f));
    } else {
      w.sun = 0.6f;  // (the map's own light: a fair day)
    }
    w.waterLevel = haveMap_ && map.water.present ? map.water.elevation : -1e9f;
    w.pace = unitDirtPace;
    wear.Dirt(frame, clock_, w, modelOf);
  }
  // where much snow fell off a unit at once: a puff of it (the game's own snowy kick-up)
  std::vector<Vec4> puffs;
  wear.TakePuffs(&puffs);
  for (const Vec4& p : puffs)
    if (!OutsidePlayable(p.x, p.z)) {
      particles.Start(kSnowPuffClumps, {p.x, p.y, p.z}, Quat{}, p.w);
      particles.Start(kSnowPuffCloud, {p.x, p.y, p.z}, Quat{}, p.w * 0.6f);
    }
}

// [rockhits] The map's rocks (rock_hits.h: by their blueprints' names), found the first time rockHits is on for a map.
void World::MakeRocks() {
  rocks.Begin(props_.size());
  for (size_t i = 0; i < props_.size() && i < map.props.size(); ++i)
    if (props_[i].model && RockHits::IsRock(map.props[i].blueprint, props_[i].model)) rocks.Add(static_cast<uint32_t>(i), props_[i].model, props_[i].world);
  rocks.Finish(static_cast<float>(map.width), static_cast<float>(map.height));
  printf("render: rock hits: %zu of the map's %zu props are rocks\n", rocks.Count(), props_.size());
}

// [rockhits] A hit that may have struck rocks (world.h): the rocks it touches scarred (RockHits::Hit), each thrown chips of its
// own stone and a puff of rock dust, as the budgets allow; a rock scarred for the first time taken out of its group.
void World::RockHit(Vec3 at, float size, Vec3 way) {
  if (!rocksNow_ || !rocks.Made()) return;
  // (told twice at one moment of the game's - a shot's impact and the blast the game makes of it - it acts once, by the larger)
  if (clock_ != rockHitsClock_) {
    rockHitsClock_ = clock_;
    rockHitsNow_.clear();
  }
  for (const Vec4& h : rockHitsNow_)
    if (h.w >= size && Length(Vec3{h.x, h.y, h.z} - at) < 1.0f) return;
  rockHitsNow_.push_back({at.x, at.y, at.z, size});
  std::vector<RockHits::Touched> touched;
  if (!rocks.Hit(at, size, clock_, &touched, way)) return;
  const float s = std::clamp(size, 0.15f, 3.0f);
  for (const RockHits::Touched& t : touched) {
    if (t.prop >= props_.size() || !props_[t.prop].model || props_[t.prop].cell < 0) continue;  // (gone, or under a structure)
    if (t.fresh) propCells_[static_cast<size_t>(props_[t.prop].cell)].changed = true;  // (its group filled anew without it)
    // chips: more and larger for a larger hit, small, none larger than a sixth of a small rock
    const int chips = rocks.TakeChips(std::min(static_cast<int>(std::lround((2 + 4 * std::sqrt(s)) * t.strength)), 12), clock_);
    if (chips > 0) debris.Chip(t.model, t.at, t.out, chips, std::min(0.04f + 0.04f * std::sqrt(s), t.across * 0.15f), t.strength);
    // a puff of rock dust where it was struck
    if (rocks.TakeDust(clock_))
      particles.Start(size < 0.8f ? rocks.dustSmall : rocks.dustLarge, t.at, Quat{}, std::clamp(0.6f + 0.5f * s, 0.6f, 2.2f) * (0.6f + 0.4f * t.strength));
  }
}

// [debrispile] The wrecks drawn as their piles (pileProps_, PlacedProp::pile): with the debris on and the modern look each told
// to the baked debris every frame (it draws it, matched to its death: baked_debris.h; one no longer told is gone), else each
// drawn as its model on its own (a hot one with its heat, hotProps_). The game's prop is the same either way.
void World::ListPiles() {
  const bool before = meshes.addingScenery;
  meshes.addingScenery = true;  // (as the scenery's groups)
  for (auto it = pileProps_.begin(); it != pileProps_.end();) {
    auto made = madeProps_.find(it->first);
    if (made == madeProps_.end() || !made->second.pile || !made->second.model) {
      it = pileProps_.erase(it);
      continue;
    }
    const PlacedProp& p = made->second;
    if (pilesNow_) {
      const Mat4& m = p.world;
      const float yaw = std::atan2(m.m[0][2], m.m[2][2]);  // (its heading: where its front, the model's z, looks)
      const float scale = Length(Vec3{m.m[0][0], m.m[1][0], m.m[2][0]});
      debris.baked.Wreck(it->first, it->second, {m.m[0][3], m.m[1][3], m.m[2][3]}, Quat{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}, scale, p.born);
      // [debrishull] (a water structure: its hull stands as its wreck, in the paint's burnt grey)
      if (const Model* hull = debris.baked.HullOf(it->second)) meshes.Add(hull, p.world, {0.3f, 0.3f, 0.3f});
    } else if (!p.hot) {
      meshes.Add(p.model, p.world, {1, 1, 1});
    }
    ++it;
  }
  meshes.addingScenery = before;
}

// Hot metal (hot_metal.h): a fresh wreck is held out of its square's group of still scenery while it glows (the group's
// instances are kept from frame to frame; a hot wreck's heat changes every frame), and added on its own with its heat
// (Begin); once it has cooled, or hot metal is switched off, it goes back into the group.
void World::HoldHot(uint32_t id, PlacedProp* prop) {
  // ([fogglow] nor one the game makes as it starts, its own scenery's wrecks at its first second: they are old, and glowed
  // round the map for the first minute)
  if (!hotOn_ || prop->hot || !prop->wreck || !prop->model || prop->born < 1.0) return;
  Vec3 centre;
  float radius;
  Sphere(prop->model, prop->world, &centre, &radius);
  if (clock_ - prop->born >= static_cast<double>(heat.WreckSeconds(radius)) || UnderWater(centre, radius)) return;
  prop->hot = true;
  if (prop->cell >= 0) propCells_[static_cast<size_t>(prop->cell)].changed = true;
  hotProps_.push_back(id);
}

void World::UpdateHotProps() {
  if (hotOn_ && !hotBefore_)  // (just switched on: the wrecks of the last minutes that still glow)
    for (auto& made : madeProps_) HoldHot(made.first, &made.second);
  if (!hotOn_ && hotBefore_) heat.Off(&particles);
  for (size_t i = 0; i < hotProps_.size();) {
    auto made = madeProps_.find(hotProps_[i]);
    bool keep = false;
    if (made != madeProps_.end()) {
      PlacedProp& p = made->second;
      if (hotOn_ && p.hot && p.model) {
        Vec3 centre;
        float radius;
        Sphere(p.model, p.world, &centre, &radius);
        keep = clock_ - p.born < static_cast<double>(heat.WreckSeconds(radius)) && !UnderWater(centre, radius);
      }
      if (!keep && p.hot) {
        p.hot = false;
        if (p.cell >= 0) propCells_[static_cast<size_t>(p.cell)].changed = true;
      }
    }
    if (keep) {
      ++i;
    } else {
      hotProps_[i] = hotProps_.back();
      hotProps_.pop_back();
    }
  }
  hotBefore_ = hotOn_;
}

// [fogglow] Was the unit a wreck made here at `born` belongs to ever seen by the army the picture is for: a death told
// near it (within its size and a little) up to a minute before. None found (a wreck the game made otherwise): not known.
bool World::WreckKnown(Vec3 centre, float radius, double born) const {
  const float reach = radius + 3;
  for (const KnownDeath& d : deaths_) {
    const float dx = d.at.x - centre.x, dz = d.at.z - centre.z;
    if (d.when <= born + 1 && born - d.when < 60 && dx * dx + dz * dz <= reach * reach && d.known) return true;
  }
  return false;
}

// [fogglow] A hot wreck glows (its heat, its light, its smoke) where the army knew its unit, or where it sees now.
bool World::GlowShown(const PlacedProp& p, Vec3 centre) const {
  if (p.glowKnown || !haveMap_ || !sight.On()) return true;
  return sight.SeenAt(centre.x / static_cast<float>(map.width), centre.z / static_cast<float>(map.height));
}

// (a wreck that lies under the water - a ship's on the bottom - is quenched: it does not glow)
bool World::UnderWater(Vec3 centre, float radius) const {
  return haveMap_ && map.water.present && centre.y + radius * 0.3f < map.water.elevation;
}

void World::Sphere(const Model* model, const Mat4& world, Vec3* centre, float* radius) {
  const Vec3 mid = (model->boundsMin + model->boundsMax) * 0.5f;
  const Vec4 c = world * Vec4{mid.x, mid.y, mid.z, 1};
  *centre = {c.x, c.y, c.z};
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  *radius = Length(model->boundsMax - model->boundsMin) * 0.5f * scale;
}

const Vec3* World::WreckHeat(uint32_t key, const Model* model, const Mat4& at) {
  if (!hotOn_ || !model || !model->ok) return nullptr;
  Vec3 centre;
  float radius;
  Sphere(model, at, &centre, &radius);
  const float age = static_cast<float>(clock_ - heat.FirstSeen(key)), seconds = heat.WreckSeconds(radius);
  if (age >= seconds || UnderWater(centre, radius)) return nullptr;
  if (!sidePicture && !extraView) heat.Glowing((uint64_t{1} << 40) | key, centre, radius, age, seconds, true);
  heatTint_ = HotMetal::Tint(age, seconds);
  return &heatTint_;
}

// Whose a shield's mesh is (effect_files.h, MeshFaction)
std::string World::MeshFaction(const std::string& mesh) { return render::MeshFaction(mesh); }

void World::WatchDeaths(const Frame& frame, const std::vector<Vec3>& armyColors) {
  // (a frame without units says nothing of deaths: the effects stand hands the world its frame without them, as it
  // draws them itself, and its units here)
  if (!blastDebris || look != Look::Modern) debrisWarm_ = false;  // [groundfix]
  if (!blastDebris || look != Look::Modern || !haveMap_ || sidePicture || frame.units.empty()) return;
  // [groundfix] where each kind of unit's paint glows, read ahead on the debris's own thread (Debris::Warm): the kinds
  // drawn from now on as AddFrame first makes their models, and those made already as the debris is switched on
  debris.files = vfs_;
  // [bakeddebris] (a unit's baked set: its files, its model, the hot metal's time, the ground it lies on)
  debris.baked.files = vfs_;
  debris.baked.models = &models;
  debris.baked.heat = &heat;
  debris.baked.groundHeight = debris.groundHeight;
  if (!debrisWarm_) {
    debrisWarm_ = true;
    for (const KnownModel& k : unitModels_)
      if (k.known && k.model) debris.Warm(k.model);
  }
  debris.Watch(frame, clock_, [this](const std::string& id) { return models.Unit(id); },
               [&](uint32_t army) { return armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : armyColors[army % armyColors.size()]; }, &blasts);
}

void World::AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors) {
  auto colorOf = [&](uint32_t army) { return armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : armyColors[army % armyColors.size()]; };
  TakeDueCombat(static_cast<double>(frame.tick) + frame.blend);  // (hits, impacts, health whose beat is shown now: combat_)
  // [fogglow] who died and whether the army saw them (its own side's always; another's seen now or at some time)
  if (sight.On() && !extraView) {
    for (const FrameUnit& u : frame.units)
      if (u.dead && deathsTold_.insert(u.id).second)
        deaths_.push_back({{u.position[0], u.position[1], u.position[2]}, clock_, u.recon == 0xff || u.Seen() || (u.recon & 0x10) != 0});
    // (a clock that went back - a replay run again from an earlier moment: the units that died after it live again, and
    // die again to be told)
    if (std::any_of(deaths_.begin(), deaths_.end(), [&](const KnownDeath& d) { return clock_ < d.when - 1; })) deathsTold_.clear();
    deaths_.erase(std::remove_if(deaths_.begin(), deaths_.end(), [&](const KnownDeath& d) { return clock_ - d.when > 60 || clock_ < d.when - 1; }),
                  deaths_.end());
    if (deathsTold_.size() > 8192) deathsTold_.clear();
  }
  // blasts where the game says an area's damage went off (an area's hits, one a target, merge into one blast), beside
  // the flashes and scorches the effects tell of. A push is no explosion and left out: the trees' own, 'TreeForce', is
  // the same shot's; 'Force' rings knock trees and props over (a commander's warp-in sends several round its start,
  // which made blasts there, and plants that react burnt the start black). Nor is 'TreeFire', which sets trees alight (a
  // Galactic Colossus's claw drops what it held with it, ADFTractorClaw.lua: it made volume explosions there)
  auto push = [](const std::string& type) {
    std::string low = type;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("force") != std::string::npos || low == "treefire";
  };
  for (const FrameHit& h : combat_.hits)
    if (h.radius > 0 && !h.shield && h.seen && !push(h.damageType))
      blasts.FromImpact({h.position[0], h.position[1], h.position[2]}, h.radius, h.amount, h.tick * 0.1);
  // [rockhits] where shots and beams met something that is not water, the air or a shield (a rock among it, or the ground
  // beside one), as the game says it: a shell's or a beam's direct hit (a blast's area comes with the blasts, below)
  if (rocksNow_ && combat_.impactsKept)
    for (const FrameImpact& m : combat_.impacts)
      if (m.seen && m.kind != FrameImpact::Crash && m.type != "Water" && m.type != "Underwater" && m.type != "Air" && m.type != "Shield")
        RockHit({m.position[0], m.position[1], m.position[2]}, m.kind == FrameImpact::Beam ? 0.15f : 0.25f, {m.direction[0], m.direction[1], m.direction[2]});
  WatchDeaths(frame, armyColors);  // (the debris of what has died: blastDebris)
  if (volumesOn_ && combat_.hitsKept)  // (volume explosions: harm to an area is a blast that large; a push is none)
    for (const FrameHit& h : combat_.hits)
      if (h.radius > 0 && h.seen && !push(h.damageType)) {
        if (VolfxLog* log = VolfxLog::Get()) log->SetArea(h.weapon, h.projectile, h.damageType, h.amount);  // (MOHO64_DEBUG_VOLFX: what harmed it)
        if (AreaMakesVolume(h)) {
          std::string type = h.damageType;  // ('Nuke': a strategic missile's own harm, NukeDamage.lua)
          for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          volumes.OfferArea({h.position[0], h.position[1], h.position[2]}, h.radius, static_cast<double>(h.tick) * 0.1, type == "nuke");
        } else if (VolfxLog* log = VolfxLog::Get()) log->Declined(false, h.radius, "high in the air, under the water, a warp-in's or burning on");
      }
  CpuTimes::Part("units seen first: their files read ahead");
  {  // (their models' files read on the worker threads, all together; the models are made below)
    std::vector<std::string> fresh;
    for (const FrameUnit& u : frame.units) {
      if (!u.Seen() || u.blueprint >= frame.blueprints->size() || (u.blueprint < unitModels_.size() && unitModels_[u.blueprint].known)) continue;
      const std::string& id = (*frame.blueprints)[u.blueprint];
      if (!id.empty() && std::find(fresh.begin(), fresh.end(), id) == fresh.end()) fresh.push_back(id);
    }
    if (!fresh.empty()) models.Prefetch(fresh);
    if (readAhead)  // [hitches] (what each new kind upgrades to, made ahead: ModelCache::ReadAheadUpgrade)
      for (const std::string& id : fresh) models.ReadAheadUpgrade(id);
  }
  CpuTimes::Part("units listed");
  {  // hits on personal shields: each the nearest unit with its shield up that the hit lies at (its
     // place on the model at rest, where the shield's lattice is painted), for the lattice to light there
    std::vector<Vec3> hits;
    effects.TakeHits(&hits);
    for (const Vec3& hit : hits) {
      const FrameUnit* best = nullptr;
      float nearest = 1e30f, bestReach = 12;
      for (const FrameUnit& u : frame.units) {
        if (u.dead || !u.Seen() || u.blueprint >= frame.blueprints->size()) continue;
        // (a personal shield: the script has set a shield's mesh on the unit, or the unit's blueprint has
        // a look of its own for one, Display.PersonalShield)
        bool personal = false;
        if (u.mesh && u.mesh < frame.blueprints->size()) {
          std::string set = (*frame.blueprints)[u.mesh];
          for (char& c : set) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          personal = set.find("shield") != std::string::npos;
        }
        const std::string& id = (*frame.blueprints)[u.blueprint];
        if (!personal && u.hasShield && !id.empty()) personal = models.PersonalShield(id).any;
        if (!personal) continue;
        const float d = Length(hit - Vec3{u.position[0], u.position[1], u.position[2]});
        if (d >= nearest) continue;
        // (a big unit is hit far from its middle: within its own size of it, at least 12)
        float reach = 12;
        if (!id.empty()) {
          const Model* m = models.Unit(id);
          if (m && m->ok) reach = std::max(reach, Length(m->boundsMax - m->boundsMin) * 0.5f * m->scale * 1.2f);
        }
        nearest = d, best = &u, bestReach = reach;
      }
      if (!best || nearest > bestReach) continue;  // (a bubble's hit, or one far from any)
      const Quat turn{best->orientation[0], best->orientation[1], best->orientation[2], best->orientation[3]};
      std::vector<ShieldHit>& list = shieldHits_[best->id];
      list.push_back({Rotate(Conjugate(turn), hit - Vec3{best->position[0], best->position[1], best->position[2]}), clock_});
      if (list.size() > 8) list.erase(list.begin());
    }
    for (auto it = shieldHits_.begin(); it != shieldHits_.end();) {
      std::vector<ShieldHit>& list = it->second;
      list.erase(std::remove_if(list.begin(), list.end(), [this](const ShieldHit& h) { return clock_ - h.clock > 1.5 || clock_ < h.clock; }), list.end());
      it = list.empty() ? shieldHits_.erase(it) : std::next(it);
    }
  }
  if (unitScars || unitSnow || unitDirt || unitGrime || unitDamage) {  // (battle scars and snow on units: what hit whom this beat; unit_wear.h; [unit dirt]; [grime])
    CpuTimes::Part("units' battle scars, snow and dirt watched");  // ([groundfix] the dirt's too)
    // what the simulation says hit a unit or its shield (REQUESTS 36): the marks where and by what; where it says so,
    // health drops with no hit told make none (reclaim, a script's SetHealth)
    wear.exactHits = combat_.hitsKept;
    for (const FrameHit& h : combat_.hits) {
      if ((h.targetKind != 0 && h.targetKind != 1) || !(unitScars || unitSnow || unitGrime || unitDamage)) continue;  // [unit dirt] (the dirt takes no hits)
      UnitWear::HitEvent e;
      e.tick = h.tick;
      e.unit = h.target;
      e.at = {h.position[0], h.position[1], h.position[2]};
      e.way = {h.direction[0], h.direction[1], h.direction[2]};
      e.radius = h.radius;
      e.blueprint = h.projectile;
      e.beam = h.beam;
      e.weapon = h.weapon;
      e.damageType = h.damageType;
      e.amount = h.amount;
      e.shielded = h.shield;
      wear.Tell(e);
    }
    WatchWear(frame);
    CpuTimes::Part("units listed");
  }
  ++frameStamp_;
  rememberedGone.clear();
  dyingDrawn = 0;
  bool noFocus = false;
  // [stealthlook] the player's side (own or allied, every army without a focus) and the radar stealth fields it gives off
  auto ourSide = [&](const FrameUnit& u) {
    const uint32_t army = u.Army();
    return focusArmy < 0 || static_cast<int>(army) == focusArmy || (army < 64 && ((alliedArmies >> army) & 1) != 0);
  };
  stealthFields_.clear();
  cloakedNow_ = 0;
  if (stealthLook)
    for (const FrameUnit& u : frame.units)
      if (!u.dead && u.stealthField > 0 && ourSide(u)) stealthFields_.push_back({u.position[0], u.position[2], u.stealthField});
  for (const FrameUnit& u : frame.units) {
    if (u.recon == 0xff) noFocus = true;
    if (u.dead) {
      if (u.Seen()) remembered_.erase(u.id);  // (the army saw it go)
      if (!clearings_.empty()) {  // [underfoot] (a structure dying still stands over what it took until its death is over)
        auto clearing = clearings_.find(u.id);
        if (clearing != clearings_.end()) clearing->second.stamp = frameStamp_;
      }
      // A unit that is dying is there until its death is over: a plane falling out of the sky, a ship
      // going down, a walker toppling. Drawn as the simulation moves and poses it, without anything
      // that belongs to a live unit (symbol, bars, lamps, wake; it cannot be pointed at).
      const std::string& dying = (*frame.blueprints)[u.blueprint];
      const Model* body = u.Seen() && !dying.empty() && !OutsidePlayable(u.position[0], u.position[2])
                              ? Known(unitModels_, u.blueprint, frame.blueprints->size(), [&] { return models.Unit(dying); })
                              : nullptr;
      // [debrishull] (a ship whose baked parts were thrown: its hull without them, posed by its sinking as the ship)
      if (body && body->ok)
        if (const Model* hull = debris.HullFor(u.id)) body = hull;
      if (debris.Replaced(u.id)) body = nullptr;  // [debrisairsea] (an aircraft down: its baked pieces are it now)
      if (body && body->ok) {
        Pose pose;
        pose.now = frame.bones.data() + u.firstBone;
        pose.nowCount = u.boneCount;
        pose.next = frame.bones.data() + u.nextFirstBone;
        pose.nextCount = u.nextBoneCount;
        pose.blend = frame.blend;
        meshes.Add(body, Mat4::FromTRS({u.position[0], u.position[1], u.position[2]}, Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]}, body->scale),
                   colorOf(u.Army()), u.fractionComplete, &pose, nullptr, nullptr, u.id + 1);
        if (const WearLook* w = WearOf(u.id)) meshes.Wear(*w);  // (its scars go down with it)
        ++dyingDrawn;
        // (ripples: a ship going down, a plane coming down into the water)
        if (!extraView)  // [views] (the ripples are told once a frame, by the first view)
          water.ripples.AddHull(u.id, {u.position[0], u.position[1], u.position[2]}, Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]},
                              body->boundsMin * body->scale, body->boundsMax * body->scale);
      }
      continue;
    }
    // of another army's unit only what the army the picture is for knows: the unit when it is seen,
    // its blip when it is on radar, sonar or omni alone, nothing otherwise
    const std::string& blueprint = (*frame.blueprints)[u.blueprint];
    // (what the army has seen before it remembers: a building stays where it stood, drawn as it
    // was; a unit keeps its own icon when it is a blip)
    // [fogwar] Which are remembered, by the recon bits as the original goes (the engine's reading, 2026-11-03): seen at
    // some time and not now (0x10 without 8); with no radar, sonar or omni bit that can only be a structure (the army's
    // blips of mobile units are deleted when they leave its intel); with one, a mobile unit shows as its blip and a
    // structure as remembered, told by its blueprint's MotionType (no longer by the third letter of its id, "ueb0101":
    // mods' structures named otherwise were not remembered).
    const bool known = u.SeenBefore();
    const bool building = IsStructure(u.blueprint, blueprint);
    const bool remembered = known && u.recon != 0xff && ((u.recon & 7) == 0 || building);
    const bool blip = u.Blip() && !remembered;
    Vec3 position{u.position[0], u.position[1], u.position[2]};
    Quat orientation{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    const Vec3 color = colorOf(u.Army());
    float fractionComplete = u.fractionComplete;
    // another army's building is kept as it is while it is seen (0x10: seen at some time, which
    // the army's own and its allies' never say), and drawn as it was kept while it is not: what
    // has become of it since, the army does not know
    // [fogwar] a still photo of it (Chris, 2026-11-02: under the fog one sees the structures as last scouted, nothing
    // more): its pose and hidden bones, its treads' scroll, its health and its wear as they were, and nothing that works -
    // no machinery, lamps, dust, outline (below, by asRemembered; its effects by Effects::Hidden, its shield's bubble by
    // the bubble's own sight)
    bool asRemembered = false;
    const Remembered* photo = nullptr;
    if (u.recon != 0xff && (u.recon & 0x10) && (u.Seen() ? building : remembered)) {
      if (u.Seen()) {
        Remembered& r = remembered_[u.id];
        r.blueprint = u.blueprint, r.position = position, r.color = color, r.orientation = orientation;
        r.fractionComplete = fractionComplete, r.stamp = frameStamp_;
        r.bones.assign(frame.bones.begin() + std::min<size_t>(u.firstBone, frame.bones.size()),
                       frame.bones.begin() + std::min<size_t>(static_cast<size_t>(u.firstBone) + u.boneCount, frame.bones.size()));
        r.treads[0] = u.treads[0], r.treads[1] = u.treads[1];
        r.health = u.health, r.maxHealth = u.maxHealth;
        r.wearKept = false;
      } else {
        auto kept = remembered_.find(u.id);
        if (kept != remembered_.end()) {
          kept->second.stamp = frameStamp_;
          position = kept->second.position;
          orientation = kept->second.orientation;
          fractionComplete = kept->second.fractionComplete;
          asRemembered = true;
          photo = &kept->second;
        }
      }
    }
    if (!u.Seen() && !blip && !remembered) continue;
    if (OutsidePlayable(position.x, position.z)) continue;  // (beyond the part of the map that can be played now)
    if (!blueprint.empty()) {  // its strategic icon, when it is far enough away
      if (u.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[u.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      icons.Add(kind, position, blip && !known ? color : IconColor(u.Army(), color), isSelected, blip, known);
      const Model* bracketed = (isSelected || u.id == hover) && !blip
                                   ? Known(unitModels_, u.blueprint, frame.blueprints->size(), [&] { return models.Unit(blueprint); })
                                   : nullptr;
      if (!selectionMarks) {
        // (none: a cinematic's)
      } else if (isSelected && haveBrackets_) {  // the original's brackets about it, turned as the unit is
        AddBrackets(0, position, orientation, *kind, bracketed);
      } else if (isSelected) {  // (the game's pictures are not there: a ring)
        const Vec3 forward = Rotate(orientation, {0, 0, 1});
        decals.AddMark(selectionRing_, position, kind->selectionX * 2.2f, kind->selectionZ * 2.2f, std::atan2(forward.x, forward.z));
      } else if (u.id == hover && hoverKind >= 1 && hoverKind <= 3 && haveBrackets_) {
        AddBrackets(hoverKind, position, orientation, *kind, bracketed);
      }
    }
    const bool freshModel = u.blueprint >= unitModels_.size() || !unitModels_[u.blueprint].known;  // [groundfix]
    const Model* model = blueprint.empty() || blip
                             ? nullptr
                             : Known(unitModels_, u.blueprint, frame.blueprints->size(), [&] { return models.Unit(blueprint); });
    if (freshModel && debrisWarm_ && model) debris.Warm(model);  // [groundfix] (a kind first drawn: where its paint glows, read ahead)
    // [underfoot] a structure standing or being built (seen, or as last seen): the map's decor under it taken away
    if (building && model) ClearUnderStructure(u.id, model, position, orientation);
    // Its bars, as the original draws them (Chris's pictures of it, 2026-10-13): the life bar under
    // the unit, about as wide as the unit, black with a filling that is green, yellow at middling
    // health and red when low, on every unit near enough to be a model (and on a selected or
    // pointed-at one at any distance); the work bar directly under it, black with an orange filling,
    // on the unit that does the work (of the army the picture is for: the others' work is not the
    // player's to see). And the game's pause mark over a paused unit, above its top.
    // (not on a unit whose script has hidden all of it: what a building is upgraded to stands in
    // the same place, unseen, until it is done, and the original shows no bar for it)
    bool allHidden = false;
    if (photo) {  // [fogwar] (as it was)
      for (const FrameBone& b : photo->bones)
        if (b.bone == 0) {
          allHidden = b.hidden;
          break;
        }
    } else {
      for (uint32_t i = u.firstBone; i < u.firstBone + u.boneCount && i < frame.bones.size(); ++i)
        if (frame.bones[i].bone == 0) {
          allHidden = frame.bones[i].hidden;
          break;
        }
    }
    // [fogwar] (a remembered one: no bars at all - Chris in the original, 2026-11-03: a remembered enemy structure under the
    // fog shows no life bar, only its name and owner on hover; its work and its shield are not the army's to know either)
    const float health = photo ? photo->health : u.health, maxHealth = photo ? photo->maxHealth : u.maxHealth;
    if (!blip && !blueprint.empty() && (u.Seen() || photo) && !allHidden) {
      // (the kind as looked up once for the blueprint above, where it was found)
      const StrategicIcons::Kind* kind = u.blueprint < iconKinds_.size() ? iconKinds_[u.blueprint] : nullptr;
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool own = focusArmy < 0 || static_cast<int>((u.id >> 20) & 0xff) == focusArmy;
      const float away = Length(position - view.eye);
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      const uint32_t army = (u.id >> 20) & 0xff;
      const bool friendly = own || (army < 64 && ((alliedArmies >> army) & 1) != 0);
      const bool life = lifeBars && !photo && maxHealth > 0 && (friendly || lifeBarsOnEnemies) && (isSelected || u.id == hover || icons.Measure(position) < kind->fadeIn * 0.9f);  // ([iconzoom] as the icons)
      // (a firing cycle only on the unit under the mouse or selected: Chris, 2026-10-14, the Summit's
      // reload bar "should only show if I put my cursor over it instead of at all times")
      const bool work = workBars && own && !photo && u.workProgress > 0 && u.workProgress < 1 && (!kind->fireClock || isSelected || u.id == hover);
      if (life || work) {
        // (the size, height and place the unit's blueprint gives its life bar, in world units: the bar
        // lies that far from the unit's middle down the screen, along the ground)
        const float perPixel = std::max(2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(screenHeight_, 1)) * away, 1e-5f);
        // (straight down the screen for every unit, wherever in the picture it stands: against the
        // camera's up, along the ground. Toward the eye itself would pull the bars of units at the
        // picture's sides inward, off their units.)
        Vec3 right, up, back;
        camera.Axes(&right, &up, &back);
        Vec3 toEye{-up.x, 0, -up.z};
        const float flat = Length(toEye);
        toEye = flat > 1e-3f ? toEye * (1 / flat) : Vec3{0, 0, 1};
        const Vec3 at = position + toEye * kind->lifeBarOffset;
        const float wide = std::clamp(kind->lifeBarSize / perPixel, 10.0f, 900.0f);
        const float thick = std::clamp(std::floor(kind->lifeBarHeight / perPixel + 0.5f), 3.0f, 14.0f);
        // (the life bar, under it the shield's where the unit has one up, under that its work)
        const bool shield = life && !photo && (u.hasShield || u.shieldRatio > 0);  // (down: an empty bar)
        const size_t before = icons.BarCount();
        if (life) icons.AddBar(at, health / maxHealth, wide, 0, 0, thick);
        if (shield) icons.AddBar(at, u.shieldRatio, wide, 2, thick, thick);
        if (work) icons.AddBar(at, u.workProgress, wide, 1, (life ? thick : 0) + (shield ? thick : 0), thick);
        // (a unit in the life bars' list: where its name would go)
        float ax = 0, ay = 0;
        if (life && icons.BarsAnchor(before, &ax, &ay)) barAnchors.push_back({u.id, ax, ay});
      }
      // ([pausemark] ui request 42, Chris's big test: a switched-off extractor had no mark; the original marks an allied unit
      // that is paused or has a toggle off; [fogwar] none on a remembered photo)
      if (workBars && friendly && !photo && (u.paused || u.ToggledOff())) {
        if (pauseMark_ == -2) pauseMark_ = icons.MarkIcon("pause");
        const float top = model && model->ok ? std::max(model->boundsMax.y * model->scale, 0.3f) + 0.35f : kind->selectionZ * 1.1f + 0.4f;
        icons.AddMark(pauseMark_, position + Vec3{0, top, 0}, 16);
      }
    }
    float radius = 0.5f;
    Vec3 centre = position + Vec3{0, 0.4f, 0};
    bool pickBox = false;  // [pick]
    Vec3 pickLo, pickHi;
    if (blip) {
      // (its blip alone, which can be pointed at)
    } else if (PlacedMeshes::Feedback(model)) {
      // [pings] a unit whose mesh is drawn by CommandFeedback4 (the ferry beacons): by that shader's rules after the water
      // (placed.h), unlit, casting no shadow; none of a unit's other looks
      placedDraw.TakeModel(model, Mat4::FromTRS(position, orientation, model->scale));
      centre = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
      radius = Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale;
    } else if (model && model->ok) {
      // (it treads the lying snow where it stands: what is on the ground and seen; a track must not
      // give away a unit the army does not see)
      if (snowLying > 0 && haveMap_ && !u.dead && u.Seen() && position.y - GroundHeight(position.x, position.z) < 1.0f && !deepNow_ && !extraView) {
        const Vec3 size = (model->boundsMax - model->boundsMin) * model->scale;
        tracks.Add(u.id, position, std::clamp(std::max(size.x, size.z) * 0.42f, 0.35f, 12.0f));
      }
      // (deep tracks: what drives or walks on the ground presses it, as the army sees it go)
      if (deepNow_ && !u.dead && u.Seen() && fractionComplete >= 1) PressUnit(u, frame, blueprint, model, position, orientation);
      // [lift] what floats rides the waves in the picture alone (ocean.h, Ocean::BobAt; World::oceanLift): a ship or a hover
      // unit on the water rises and falls with the long waves under it and pitches and rolls with their slopes; its place in
      // the game, its picking, its icon, its life bar and its wake stay where the game has it
      Vec3 drawAt = position;
      Quat drawTurn = orientation;
      Ocean::Bob rode;  // [hulls]
      float sunk = 0;
      const bool riding = !sidePicture && !building && !u.dead && RideWaves(model, &drawAt, &drawTurn, &rode, &sunk);  // ([whitecaps] the effects on its bones too)
      Pose pose;
      pose.now = frame.bones.data() + u.firstBone;
      pose.nowCount = u.boneCount;
      pose.next = frame.bones.data() + u.nextFirstBone;
      pose.nextCount = u.nextBoneCount;
      pose.blend = frame.blend;
      if (photo) {  // [fogwar] (as it stood when last seen: its bones then, not the rest pose)
        pose.now = pose.next = photo->bones.data();
        pose.nowCount = pose.nextCount = static_cast<uint32_t>(photo->bones.size());
        pose.blend = 0;
      }
      // [machinery] a factory's parts at work (machinery.h): posed after the simulation's bones, as it is seen and finished
      float belt = -1;
      if (model->machinery && !asRemembered && u.Seen() && fractionComplete >= 1)
        belt = machinery.PoseUnit(u, *model, drawAt, drawTurn, UnitFaction(blueprint), &pose);
      // a personal shield: the script has set a shield's mesh on the unit (the name says so; FAF
      // sets other meshes on units too)
      bool shielded = false;
      if (u.mesh && u.mesh < frame.blueprints->size() && fractionComplete >= 1 && u.Seen()) {
        std::string set = (*frame.blueprints)[u.mesh];
        for (char& c : set) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        shielded = set.find("shield") != std::string::npos;
      }
      // (a shield's last hit, within the second it shows: its age rides on what is built, 3 and a
      // little more, and where it struck on the place a ghost's tint has, which a shielded unit has not)
      // (a mod's own look for its personal shield, Display.PersonalShield: only round its hits, and on a
      // capsule round the unit instead of along its outline)
      const PersonalShieldLook* own = nullptr;
      if ((shielded || u.hasShield) && !blueprint.empty() && !asRemembered) {  // ([fogwar] not on a remembered one)
        const PersonalShieldLook& look = models.PersonalShield(blueprint);
        if (look.any) own = &look;
      }
      auto hits = shieldHits_.find(u.id);
      const ShieldHit* latest = hits == shieldHits_.end() || hits->second.empty() ? nullptr : &hits->second.back();
      const double latestAge = latest ? clock_ - latest->clock : 2;
      // ([shieldhit] personalShieldsAtRest off, the modern look: every shield along its outline only while hit, and what
      // it shows fading with the hit - `built` 5 and a little more, MeshRenderer::Add)
      const bool onlyHits = ShieldOnlyHits(blueprint);
      // [warpin] a commander warping in (warpInColours): a shield's mesh on it and no shield hanging on it (a personal
      // shield that is up has its entity, FrameUnit::hasShield; the UEF's and the Aeon's commanders' shield upgrades set
      // the very mesh their warp-in does), for the six seconds from when the mesh was first seen on it. How far it is
      // rides on what is built, 7 and nine tenths of its share (MeshRenderer::Add); it is no shield: it shows whatever
      // the shields' rest setting says, on a mod's commander too, and nothing of a hit.
      float warpAge = -1;
      if (shielded && !u.hasShield && warpInColours && look == Look::Modern && effectsLook == EffectsLook::Remastered && !asRemembered) {
        auto since = warpSince_.try_emplace(u.id, clock_).first;
        if (clock_ < since->second) since->second = clock_;  // (the clock set back: a replay wound back)
        const double age = clock_ - since->second;
        if (age < kWarpInSeconds) warpAge = static_cast<float>(age / kWarpInSeconds);
        else if (age < kWarpInSeconds + 1) shielded = false;  // (faded out; the script's switch back comes with its beat)
      } else if (!shielded && !warpSince_.empty()) {
        warpSince_.erase(u.id);
      }
      const bool warping = warpAge >= 0;
      if (warping) {
      } else if (own && own->capsule) shielded = false;                            // (the hull shows nothing of it)
      else if (own && own->hitOnly) shielded = latestAge >= 0 && latestAge < 1;  // (along its outline while hit)
      else if (shielded && onlyHits) shielded = latestAge >= 0 && latestAge < 1;
      float built = warping ? 7.0f + 0.9f * warpAge : shielded ? 3.0f : fractionComplete;
      const Vec3* hitAt = nullptr;
      if (shielded && !warping && latestAge >= 0 && latestAge < 1) {
        built = (onlyHits ? 5.001f : 3.001f) + 0.9f * static_cast<float>(latestAge);
        hitAt = &latest->at;
      }
      if (own && own->capsule && own->lattice && hits != shieldHits_.end()) {  // its shell, lit round its hits
        const Model* shell = models.ShieldShell(model, *own);
        shellHits_.clear();
        for (const ShieldHit& h : hits->second) {
          const double age = clock_ - h.clock;
          if (age < 0 || age >= 1) continue;
          const Vec3 on = OnShell(*shell, h.at);  // (where the shot met the game's shield, onto the shell)
          shellHits_.push_back({on.x, on.y, on.z, static_cast<float>(age)});
        }
        if (!shellHits_.empty()) meshes.AddShell(shell, Mat4::FromTRS(drawAt, drawTurn, model->scale), shellHits_);
      }
      // [stealthlook] cloaked: the script set its cloak mesh (FAF's '<mesh>_cloak'): its body a see-through shimmer in its
      // faction's shield colour (the original's ShieldCybran for the UEF and Cybran, ShieldAeon for the Aeon and Seraphim)
      bool cloaked = false;
      if (cloakLook && u.mesh && u.mesh < frame.blueprints->size() && fractionComplete >= 1 && !asRemembered) {
        std::string set = (*frame.blueprints)[u.mesh];
        for (char& c : set) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        cloaked = set.size() > 6 && set.compare(set.size() - 6, 6, "_cloak") == 0;
      }
      const std::string side = cloaked || stealthLook ? UnitFaction(blueprint) : std::string();
      if (cloaked) {
        ++cloakedNow_;
        const Vec3 tint = side == "aeon" || side == "seraphim" ? Vec3{0.75f, 1.0f, 0.7f} : Vec3{0.6f, 0.82f, 1.0f};
        meshes.Add(model, Mat4::FromTRS(drawAt, drawTurn, model->scale), color, 1.25f, &pose, &tint, nullptr, u.id + 1);
        meshes.SeeThroughGhost();
      } else {
      // (and its treads' scroll, where its mesh's treads scroll and its shield's hit does not take the place)
      // ([fogwar] a remembered one with the pose, the treads' scroll and the wear it had when last seen)
      meshes.Add(model, Mat4::FromTRS(drawAt, drawTurn, model->scale), color, built, asRemembered && !photo ? nullptr : &pose, hitAt,
                 model->scrolls && !hitAt ? (photo ? photo->treads : u.treads) : nullptr, u.id + 1);
      meshes.Belt(belt);  // [machinery]
      if (asRemembered && fractionComplete < 1) meshes.StillBuild();  // [fogbuild] (a photo: its build look not at work)
      }
      // [stealthlook] hidden from radar (its stealth on, or within an allied stealth field), the player's side alone: a faint
      // shimmering veil a little out from its body
      bool veiled = stealthLook && !cloaked && !asRemembered && fractionComplete >= 1 && u.Seen() && ourSide(u) && (u.intelOn & 1) != 0;
      if (stealthLook && !veiled && !cloaked && !asRemembered && fractionComplete >= 1 && u.Seen() && ourSide(u))
        for (const StealthField& f : stealthFields_)
          if ((u.position[0] - f.x) * (u.position[0] - f.x) + (u.position[2] - f.z) * (u.position[2] - f.z) < f.radius * f.radius) {
            veiled = true;
            break;
          }
      // (its battle scars and snow: unit_wear.h; [groundfix] its dirt from the first frame it is drawn: its box from the
      // model drawn, which WatchWear, before the models are made, finds only from the next frame)
      if (cloaked) {
        // ([stealthlook] the last model added is its shimmer: no wear on it)
      } else if (photo) {
        Remembered& r = remembered_[u.id];
        if (!r.wearKept) {  // (taken at the first frame it is not seen: one frame on from the last it was)
          const WearLook* w = WearOf(u.id, model);
          r.hasWear = w != nullptr;
          if (w) r.wear = *w;
          r.wearKept = true;
        }
        if (r.hasWear) meshes.Wear(r.wear);
      } else if (const WearLook* w = WearOf(u.id, model)) {
        meshes.Wear(*w);
      }
      // (its outline where it is hidden, hidden_outlines.h: only what the player may see of it now - his own, an ally's,
      // an enemy's in sight - and finished; never a building drawn as it was remembered. Before its veil: MarkLast marks
      // the model added last, and the veil, see-through, writes no outline: a veiled unit had none.)
      if (outlinesNow_ && u.Seen() && !asRemembered && fractionComplete >= 1 && !allHidden)
        MarkOutline(model, drawAt, IconColor(u.Army(), color), building, u.id, u.blueprint < iconKinds_.size() ? iconKinds_[u.blueprint] : nullptr);
      if (veiled) {  // ([stealthlook] its veil, after its wear: Wear takes the model added last)
        const Vec3 tint{0.35f, 0.62f, 0.78f};
        // ([veilshake] no key of its own: Chris's second test, 2026-11-04, "stealth overlay shakes units and structures up and
        // down very fast". The veil is the unit's own model again, so with the unit's key it took the unit's place in the
        // motion's memory (MeshRenderer::Prepare keeps one place per key, the last model added under it: the veil, 1.035 the
        // size). The frame after, the unit's body was "last seen" 3.5 % larger about its feet, its top 3.5 % of its height
        // higher, and every pixel of it carried that motion: the upscaler's history pulled from above it and the generated
        // frames drew it halfway, a shake up and down at the screen's rate. The veil is see-through and draws no motion of
        // its own (DrawMotion takes the solid models alone), so it needs no key.)
        meshes.Add(model, Mat4::FromTRS(drawAt, drawTurn, model->scale * 1.035f), color, 1.375f, &pose, &tint, nullptr, 0);
        meshes.SeeThroughGhost();
      }
      if (u.fractionComplete >= 1 && u.Seen() && !cloaked) AddLamps(model, drawAt, drawTurn, &pose, &color, u.id, building);  // ([stealthlook] a cloaked one: none)  // ([lift] riding the waves with it)
      // (movement dust: what the army sees moving raises dust, spray or contrails)
      if (dustNow_ && u.Seen() && !asRemembered && fractionComplete >= 1) dust.AddUnit(u.id, blueprint, model, position, orientation);
      // ([surf] a ship at speed throws spray off its bow)
      // ([hulls] riding the lifted waves: off the sea where it meets the bow)
      if (surfNow_ && u.Seen() && !asRemembered && fractionComplete >= 1)
        water.surf.AddShip(u.id, blueprint, model, position, orientation, riding ? rode.rise : 0.0f, riding ? rode.slopeX : 0.0f, riding ? rode.slopeZ : 0.0f);
      // ripples where it floats, hovers or wades in the water (what the army sees)
      if (u.Seen() && !asRemembered && !extraView) water.ripples.AddHull(u.id, position, orientation, model->boundsMin * model->scale, model->boundsMax * model->scale);
      // [floatfoam] (what floats, for the modern water: no foam slab under its floats)
      if (u.Seen() && !asRemembered) water.AddAfloat(position, orientation, model->boundsMin * model->scale, model->boundsMax * model->scale);
      // a wake, when it goes on the water: by how far it has come since the frame before
      if (haveMap_ && map.water.present && u.Seen() && std::fabs(position.y - map.water.elevation) < 0.4f) {
        Moving& m = moving_[u.id];
        const double dt = clock_ - m.clock;
        if (dt > 1e-4 && dt < 1.0) m.speed += (Length(position - m.place) / static_cast<float>(dt) - m.speed) * 0.2f;
        if (dt > 1e-4 && dt < 1.0) m.odometer += Length(Vec3{position.x - m.place.x, 0, position.z - m.place.z});  // [wakes]
        if (dt >= 1.0 || dt < 0) {
          m.speed = 0;
          m.trailCount = 0;
        }
        if (dt != 0) {
          m.place = position;
          m.clock = clock_;
        }
        const bool overWater = map.Height(Clamp(position.x, 0, static_cast<float>(map.width)), Clamp(position.z, 0, static_cast<float>(map.height))) < map.water.elevation - 0.15f;
        if (m.speed > 0.4f && overWater) {
          const Vec3 back = Rotate(orientation, {0, 0, -1});
          const Vec3 level = Normalize(Vec3{back.x, 0, back.z});
          const float halfX = (model->boundsMax.x - model->boundsMin.x) * 0.5f * model->scale, halfZ = (model->boundsMax.z - model->boundsMin.z) * 0.5f * model->scale;
          const Vec3 stern{position.x + level.x * halfZ * 0.6f, map.water.elevation + 0.03f, position.z + level.z * halfZ * 0.6f};
          // where it has been: a place kept every third of a second, the wake laid along them
          // ([wakes] the remade wake: where its middle has been, at least a tenth of the way the wake needs from the last,
          // so that the twelve kept reach the wake's end behind a large slow ship too)
          const float length = std::min(m.speed * 1.8f, 14.0f) + halfZ, strength = std::min(m.speed / 2.5f, 1.0f);
          const bool remade = wakesRemade && look == Look::Modern;
          const Vec3 kept = remade ? Vec3{position.x, stern.y, position.z} : stern;
          const bool spaced = !remade || m.trailCount == 0 ||
                              Length(Vec3{kept.x - m.trail[0].x, 0, kept.z - m.trail[0].z}) >= std::max(0.5f, (length + halfZ * 0.6f) * 0.1f);
          if ((clock_ - m.trailClock > 0.33 && spaced) || m.trailCount == 0) {
            for (int k = std::min(m.trailCount, 11); k > 0; --k) m.trail[k] = m.trail[k - 1];
            m.trail[0] = kept;
            m.trailCount = std::min(m.trailCount + 1, 12);
            m.trailClock = clock_;
          }
          Vec3 places[13];
          places[0] = kept;
          for (int k = 0; k < m.trailCount; ++k) places[k + 1] = m.trail[k];
          if (remade) wakes.AddPathRemade(places, m.trailCount + 1, halfZ * 0.6f, level, length, halfX * 0.7f, strength, m.odometer);
          else if (m.trailCount >= 2) wakes.AddPath(places, m.trailCount + 1, length, halfX * 0.7f, strength);
          else wakes.Add(stern, level, length, halfX * 0.7f, strength);
        }
      }
      // [hulls] a ship sunk a little into the lifted sea: where the waves meet its hull (the water's shader: the white water
      // round it, its hull under the surface hidden by the sea; ocean.h, Ocean::AddHull), its waterline as its box's
      if (riding && sunk > 0 && u.Seen() && fractionComplete >= 1) ListHull(u.id, model, position, orientation, rode);
      centre = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
      radius = Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale;
      // [pick] its selection box (the original's: the top share of the mesh's box its blueprint picks, scaled; a naval factory's
      // model reaches deep under the water and picks its top 0.15)
      const Selection& sel = SelectionOf(u.blueprint, blueprint);
      Vec3 lo = model->boundsMin * model->scale, hi = model->boundsMax * model->scale;
      if (sel.top < 1) lo.y = hi.y - (hi.y - lo.y) * sel.top;
      const float midX = (lo.x + hi.x) * 0.5f, midZ = (lo.z + hi.z) * 0.5f;
      lo.x = midX + (lo.x - midX) * sel.scaleX, hi.x = midX + (hi.x - midX) * sel.scaleX;
      lo.z = midZ + (lo.z - midZ) * sel.scaleZ, hi.z = midZ + (hi.z - midZ) * sel.scaleZ;
      pickBox = true, pickLo = lo, pickHi = hi;
    } else {  // not known what it is, or its files cannot be read: a box in the army's colour
      auto byte = [](float v) { return static_cast<uint32_t>(Clamp(v, 0, 1) * 255.0f); };
      draw.Box(centre, {0.4f, 0.4f, 0.4f}, 0xff000000u | byte(color.z) << 16 | byte(color.y) << 8 | byte(color.x));
    }
    if (!allHidden) {  // ([pick] a unit whose whole mesh its script hid cannot be pointed at)
      Pickable p{u.id, centre, radius};
      p.boxed = pickBox, p.at = position, p.lo = pickLo, p.hi = pickHi, p.turn = orientation;
      pickables_.push_back(p);
    }
  }
  // remembered buildings that are gone (destroyed where the army did not see it): each stays, as
  // it was last seen, until the army sees the place again and finds it gone
  // (what one army remembers is not another's: a new focus army begins with none)
  if (noFocus || focusArmy != rememberedFor_) remembered_.clear();
  rememberedFor_ = focusArmy;
  for (auto it = remembered_.begin(); it != remembered_.end();) {
    const Remembered& r = it->second;
    if (r.stamp == frameStamp_) {
      ++it;
      continue;
    }
    const bool seenNow = !haveMap_ || !sight.On() || sight.SeenAt(r.position.x / static_cast<float>(map.width), r.position.z / static_cast<float>(map.height));
    if (seenNow || r.blueprint >= frame.blueprints->size()) {
      it = remembered_.erase(it);
      continue;
    }
    const std::string& blueprint = (*frame.blueprints)[r.blueprint];
    if (!blueprint.empty() && !OutsidePlayable(r.position.x, r.position.z)) {
      if (r.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[r.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const Vec3 iconColor = IconColor((it->first >> 20) & 0xff, r.color);
      icons.Add(kind, r.position, iconColor, false, false, true);
      rememberedGone.push_back({kind, r.position, iconColor});
      const Model* model = models.Unit(blueprint);
      if (model && model->ok && !placedDraw.TakeModel(model, Mat4::FromTRS(r.position, r.orientation, model->scale))) {  // [pings]
        // ([fogwar] as it was last seen: its pose and its wear then)
        Pose pose;
        pose.now = pose.next = r.bones.data();
        pose.nowCount = pose.nextCount = static_cast<uint32_t>(r.bones.size());
        pose.blend = 0;
        meshes.Add(model, Mat4::FromTRS(r.position, r.orientation, model->scale), r.color, r.fractionComplete, &pose, nullptr,
                   model->scrolls ? r.treads : nullptr, it->first + 1);
        if (r.wearKept && r.hasWear) meshes.Wear(r.wear);
      }
      if (model && model->ok) ClearUnderStructure(it->first, model, r.position, r.orientation);  // [underfoot] (as it was last seen)
    }
    ++it;
  }
  ReleaseClearings();  // [underfoot] (the structures not listed this frame: what they stood over stands again)
  CpuTimes::Part("projectiles, entities, beams listed");
  // ripples: what met the water, as the simulation says it (REQUESTS 36; below, after the shots). A shell's splash
  // is as large as its model, found among this frame's shots (-1: not drawn)
  std::unordered_map<uint32_t, float> waterShots;
  water.ripples.impactsGiven = combat_.impactsKept;
  if (water.ripples.On() && combat_.impactsKept)
    for (const FrameImpact& m : combat_.impacts)
      if (m.seen && m.kind == FrameImpact::Projectile && (m.type == "Water" || m.type == "Underwater")) waterShots.emplace(m.source, -1.0f);
  // projectiles: one with a model as that model; the others are effects in the game - where the
  // game's own effect is not on it, a short soft glowing band along the flight stands in
  // (debris, blastDebris: a death's debris shells are the debris's to draw, as torn pieces of what threw them; debris.h)
  const bool debrisShells = blastDebris && look == Look::Modern && haveMap_ && !sidePicture;
  if (debrisShells) debris.BeginShells();
  for (const FrameProjectile& p : frame.projectiles) {
    if (!p.seen) continue;
    const Vec3 position{p.position[0], p.position[1], p.position[2]};
    const std::string& blueprint = (*frame.blueprints)[p.blueprint];
    // (a shot's model is an effect's: the remastered look takes the visuals track's where there is one)
    if (shotsLook_ != effectsLook) {
      shotModels_.clear();
      shotsByUnit_.clear();
      shotsLook_ = effectsLook;
    }
    const bool original = effectsLook == EffectsLook::Original || p.original;
    const Model* model = Known(p.original ? originalShotModels_ : shotModels_, p.blueprint, frame.blueprints->size(), [&]() -> const Model* {
      // (an effect entity - the UEF's build cube, a nuke's fireball - may take the visuals track's
      // textures of any path; a weapon's shot under /projectiles/ only its /effects/ ones)
      const bool effect = moho::Vfs::Normalize(blueprint).compare(0, 9, "/effects/") == 0;
      return blueprint.empty() ? nullptr : models.Blueprint(blueprint, original, std::string(), effect);
    });
    // [byunit] the shell of the unit that fired it, where the visuals track made one (the Pillar's larger Gauss shell)
    if (!original && p.launcher && p.launcher < frame.blueprints->size() && !blueprint.empty()) {
      const uint64_t key = (static_cast<uint64_t>(p.blueprint) << 32) | p.launcher;
      auto own = shotsByUnit_.find(key);
      if (own == shotsByUnit_.end()) own = shotsByUnit_.emplace(key, models.ShotByUnit(blueprint, (*frame.blueprints)[p.launcher])).first;
      if (own->second && own->second->ok) model = own->second;
    }
    // (ripples: its splash where it lands in the water, or goes down through it)
    const float shotSize = model && model->ok ? Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale : 0.0f;
    if (!extraView) water.ripples.AddShot(p.id, position, p.stopped, shotSize);  // [views]
    // [rockhits] (a shot that hit something this beat, where the game keeps no impacts: once, the first frame it is seen so)
    if (rocksNow_ && p.stopped && !combat_.impactsKept && !extraView) {
      rockShotsStopped_.push_back(p.id);
      if (std::find(rockShotsBefore_.begin(), rockShotsBefore_.end(), p.id) == rockShotsBefore_.end()) RockHit(position, 0.25f, Length(Vec3{p.velocity[0], p.velocity[1], p.velocity[2]}) > 0.01f ? Normalize(Vec3{p.velocity[0], p.velocity[1], p.velocity[2]}) : Vec3{});
    }
    if (!waterShots.empty())
      if (auto it = waterShots.find(p.id); it != waterShots.end()) it->second = shotSize;
    if (debrisShells && !p.original && model && model->ok &&
        debris.Shell(p.id, p.blueprint, blueprint, position, {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]},
                     {p.velocity[0], p.velocity[1], p.velocity[2]}, p.sized ? p.size[0] : model->scale, p.stopped))
      continue;
    if (model && model->ok) {
      const Quat orientation{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
      // in its own colours: the original gives every mesh that is not a unit's opaque white where a
      // unit gets its army's colour (docs/engine-answers.md, render 7)
      // (at the size the simulation holds for it: its blueprint's at launch, grown by its scale
      // velocity, or what a script set: the UEF's box of light round what it builds, a nuke's ball)
      Mat4 at = Mat4::FromTRS(position, orientation, p.sized ? 1.0f : model->scale);
      for (int row = 0; row < 3 && p.sized; ++row)
        for (int k = 0; k < 3; ++k) at.m[row][k] *= std::max(p.size[k], 0.0f);
      if (placedDraw.TakeModel(model, at)) continue;  // [pings] (one of the CommandFeedback family: placed.h)
      // what fades with its age (the UEF's build slices: whole for two seconds, then gone at
      // 0.145 a second, the original's numbers) says how much of it is left
      float left = 1;
      if (!model->lods.empty() && model->lods[0].fades) left = Clamp(1 - (p.age - 2.0f) * 0.145f, 0, 1);
      // [glitches] (finishedSlabFade) the UEF's build glow slab over a structure no longer under construction: gone within
      // finishedSlabFade seconds of it, not left for the rest of its six-second lifetime
      if (left > 0 && finishedSlabFade > 0 && !model->lods.empty() && model->lods[0].fades && BuildGlowSlab(blueprint)) {
        if (buildingNowStamp_ != frameStamp_) {  // (the frame's units under construction, once a frame and only when asked)
          buildingNowStamp_ = frameStamp_;
          buildingNow_.clear();
          for (const FrameUnit& u : frame.units)
            if (u.fractionComplete < 1) buildingNow_.push_back({{u.position[0], u.position[1], u.position[2]}, 0.0f});
        }
        // (the slab lies centred on its structure, as wide as the structure's mesh and a twentieth more: a unit under
        // construction whose place is within it is the one it is laid over; a slice the build cube left higher up, too)
        const float hx = 0.5f * std::max(p.sized ? p.size[0] : 1.0f, 1.0f) + 0.5f, hz = 0.5f * std::max(p.sized ? p.size[2] : 1.0f, 1.0f) + 0.5f;
        bool under = false;
        for (const auto& b : buildingNow_)
          if (std::fabs(b.first.x - position.x) <= hx && std::fabs(b.first.z - position.z) <= hz) {
            under = true;
            break;
          }
        if (under) {
          leftSlabs_.erase(p.id);
        } else {
          LeftSlab& slab = leftSlabs_.try_emplace(p.id, LeftSlab{clock_, frameStamp_}).first->second;
          slab.stamp = frameStamp_;
          left *= Clamp(1 - static_cast<float>(clock_ - slab.since) / finishedSlabFade, 0, 1);
        }
      }
      // (a nuke's ball of fire is drawn by its age)
      const bool fire = !model->lods.empty() && model->lods[0].nuke;
      if (left > 0) meshes.Add(model, at, fire ? Vec3{p.age, 0, 0} : Vec3{1, 1, 1}, left);
      // a remastered missile's engine lights the ground under it (its lights list, as a unit's lamps)
      if (left > 0 && !model->lights.empty()) AddLamps(model, position, orientation, nullptr, nullptr);
    } else if (!effects.OnEntity(p.id)) {
      const Vec3 velocity{p.velocity[0], p.velocity[1], p.velocity[2]};
      ribbons.PlainBeam(position - velocity * 0.06f, position, {1.0f, 0.95f, 0.78f}, 0.12f);
    }
  }
  if (rocksNow_ && !extraView) {  // [rockhits] (the shots seen stopped this frame, for the next)
    rockShotsBefore_.swap(rockShotsStopped_);
    rockShotsStopped_.clear();
  }
  // [glitches] (finishedSlabFade) the slabs no longer listed forgotten
  for (auto it = leftSlabs_.begin(); it != leftSlabs_.end();) it = it->second.stamp != frameStamp_ ? leftSlabs_.erase(it) : std::next(it);
  {  // ripples from the blasts new this frame over the water (a ship's death, debris falling in); where the simulation's
     // impacts make the shells' splashes (below), shell-sized blasts are theirs already and left out
    std::vector<Blasts::Blast> fresh;
    blasts.TakeNew(&fresh);
    if (rocksNow_)  // [rockhits] (and the rocks the blasts on land reach: scorched, chipped; a scorch laid without heat, a warp-in's, none)
      for (const Blasts::Blast& b : fresh)
        if (!b.splash && !b.water && b.hot) RockHit(b.at, b.size);
    if (water.ripples.On())
      for (const Blasts::Blast& b : fresh) {
        if (b.splash) water.AddRipple(b.at.x, b.at.z, std::clamp(b.size, 0.2f, 0.8f), 0.5f);
        else if (b.water && (!combat_.impactsKept || b.size >= 1.5f))
          water.AddRipple(b.at.x, b.at.z, std::clamp(b.size * 0.5f, 0.4f, 3.0f), std::clamp(b.size * 0.6f, 0.5f, 4.0f));
      }
    // [dents] where the simulation does not say where its shots struck (the effects stand; a host without its impacts),
    // the blasts on the ground dent it, by their scorch (a flash alone: its own size)
    if (deepNow_ && !extraView && !combat_.impactsKept)
      for (const Blasts::Blast& b : fresh)
        if (!b.water && !b.splash) DentGround(b.at, b.scorch > 0 ? b.scorch : b.size);
  }
  // [dents] shell holes where the simulation's shots struck the ground (seen only: a hole must not give away a shot the army
  // did not see), by their weapon's numbers; an aircraft crashing on land by its size and speed. Not where a shot met a unit
  // or anything else, nor a beam's.
  if (deepNow_ && !extraView && combat_.impactsKept)
    for (const FrameImpact& m : combat_.impacts) {
      if (!m.seen || m.type != "Terrain") continue;
      float size = 0;
      if (m.kind == FrameImpact::Projectile) {
        size = ShotDentSize(m.weapon);
      } else if (m.kind == FrameImpact::Crash) {
        const Model* body = m.projectile.empty() ? nullptr : models.Unit(m.projectile);
        const Vec3 half = body && body->ok ? (body->boundsMax - body->boundsMin) * (0.5f * body->scale) : Vec3{1, 1, 1};
        size = std::clamp(std::max(half.x, half.z) * 1.6f, 0.4f, 8.0f) * std::clamp(m.speed / 15, 0.35f, 1.0f);
      }
      if (size > 0) DentGround({m.position[0], m.position[1], m.position[2]}, size);
    }
  // ripples from what met the water (seen only): a shell's splash by its size, a torpedo going off below by its depth
  // (none deeper than 4), a crashing aircraft's by its size and speed, a beam's a little one. (The strengths are
  // judged by eye, not measured against anything of the game's.)
  if (water.ripples.On() && combat_.impactsKept)
    for (const FrameImpact& m : combat_.impacts) {
      const bool surface = m.type == "Water", under = m.type == "Underwater";
      if (!m.seen || (!surface && !under)) continue;
      const float x = m.position[0], z = m.position[2];
      if (m.kind == FrameImpact::Crash) {
        if (!surface) continue;
        const Model* body = m.projectile.empty() ? nullptr : models.Unit(m.projectile);
        const Vec3 half = body && body->ok ? (body->boundsMax - body->boundsMin) * (0.5f * body->scale) : Vec3{1, 1, 1};
        water.AddRipple(x, z, std::clamp(std::max(half.x, half.z), 0.5f, 3.0f), std::clamp(m.speed / 6, 0.5f, 4.0f));
      } else if (m.kind == FrameImpact::Beam) {
        if (surface) water.AddRipple(x, z, 0.4f, 0.3f);
      } else if (surface) {
        const auto shot = waterShots.find(m.source);
        const float size = shot != waterShots.end() && shot->second >= 0 ? shot->second : 0.17f;  // (not drawn: a usual shell's)
        water.AddRipple(x, z, std::clamp(0.4f + 0.6f * size, 0.4f, 1.2f), 1);
      } else {
        const float depth = haveMap_ ? map.water.elevation - m.position[1] : 0;
        if (depth < 4) water.AddRipple(x, z, 0.8f, 0.5f * (1 - std::max(depth, 0.0f) / 4));
      }
    }
  // what scripts made that has a mesh: shields' bubbles and the like, each its mesh blueprint's model
  // (in the effects' look: the remastered one reads the visuals track's textures. A shield's bubble and
  // the flash where it is hit read those of the shield's faction first: the flash's mesh is shared by
  // three factions. The faction is the bubble's, and a flash is made at its bubble's centre.)
  impactBornNext_.clear();
  if (entitiesLook_ != effectsLook) {
    entityModels_.clear();
    entitiesLook_ = effectsLook;
  }
  bubbles_.clear();
  if (effectsLook == EffectsLook::Remastered)
    for (const FrameEntity& f : frame.entities)
      if (f.shield && !f.original && f.mesh < frame.blueprints->size()) {
        std::string faction = MeshFaction((*frame.blueprints)[f.mesh]);
        if (!faction.empty()) bubbles_.push_back({{f.position[0], f.position[1], f.position[2]}, std::move(faction)});
      }
  for (const FrameEntity& f : frame.entities) {
    if (!f.seen || f.mesh >= frame.blueprints->size()) continue;
    const std::string& mesh = (*frame.blueprints)[f.mesh];
    const bool original = effectsLook == EffectsLook::Original || f.original;
    std::string faction;
    if (!original && !bubbles_.empty()) {
      if (f.shield) {
        faction = MeshFaction(mesh);
      } else if (moho::Vfs::Normalize(mesh).find("shieldsection") != std::string::npos) {
        const Vec3 at{f.position[0], f.position[1], f.position[2]};
        float best = 1.0f;  // (made at its bubble's centre)
        for (const auto& b : bubbles_)
          if (Length(b.first - at) < best) {
            best = Length(b.first - at);
            faction = b.second;
          }
      }
    }
    const Model* model = nullptr;
    if (!faction.empty()) {
      if (!mesh.empty() && vfs_->Exists(moho::Vfs::Normalize(mesh + ".bp"))) model = models.Blueprint(mesh + ".bp", false, faction, true);
    } else {
      model = Known(original ? originalEntityModels_ : entityModels_, f.mesh, frame.blueprints->size(), [&]() -> const Model* {
        if (mesh.empty() || !vfs_->Exists(moho::Vfs::Normalize(mesh + ".bp"))) return nullptr;  // (a script may write the name in any case)
        return models.Blueprint(mesh + ".bp", original, std::string(), true);
      });
    }
    if (!model || !model->ok) continue;
    Mat4 at = Mat4::FromTRS({f.position[0], f.position[1], f.position[2]}, Quat{f.orientation[0], f.orientation[1], f.orientation[2], f.orientation[3]}, model->scale);
    for (int row = 0; row < 3; ++row)
      for (int k = 0; k < 3; ++k) at.m[row][k] *= f.scale[k];
    // [pings] a mesh of the CommandFeedback family (FAF's pings): drawn by that shader's rules after the water (placed.h)
    if (placedDraw.TakeModel(model, at)) continue;
    // [energy shields] a bubble of energy (energy_shields.h) in place of the plain one; its flashes give it its hits (below)
    const bool energyHere = energyShields && look == Look::Modern && !f.original && !model->lods.empty();
    if (energyHere && model->lods[0].shield) {
      energy.AddBubble(model, at, moho::Vfs::Normalize(mesh).find("/effects/entities/shield01/") != std::string::npos);  // [litshell] (the UEF's)
      continue;
    }
    // (in its own colours where its look has no team colour of its own: the original gives every
    // mesh that is not a unit's opaque white; an adjacency's node was a cube in the army's colour)
    const bool teamed = !model->lods.empty() && model->lods[0].teamMask;
    if (!model->lods.empty() && model->lods[0].impact) {
      // a shield's flash where it is hit: drawn by its age (and, the Cybran's, a step sideways that
      // differs from hit to hit, as the original takes it from the time it was made)
      const auto known = impactBorn_.find(f.id);
      const double born = known == impactBorn_.end() ? clock_ : known->second;
      impactBornNext_[f.id] = born;
      meshes.Add(model, at, Vec3{static_cast<float>(clock_ - born), static_cast<float>(std::fmod(born, 100.0)) * -0.01f, 0});
      // [energy shields] a hit: the flash is made at its bubble's centre, its +z toward where it struck (shield.lua)
      if (energyHere)
        energy.AddHit({f.position[0], f.position[1], f.position[2]},
                      Rotate(Quat{f.orientation[0], f.orientation[1], f.orientation[2], f.orientation[3]}, Vec3{0, 0, 1}), static_cast<float>(clock_ - born));
      continue;
    }
    // (keyed apart from the units: the top bit set)
    meshes.Add(model, at, f.army == 0xff || !teamed ? Vec3{1, 1, 1} : colorOf(f.army), 1, nullptr, nullptr, nullptr, (f.id + 1) | 0x80000000u);
  }
  impactBorn_.swap(impactBornNext_);
  if (energy.Any())  // [energy shields] how much of each shield is left: its unit's
    for (const FrameUnit& u : frame.units)
      if (u.hasShield && !u.dead) energy.AddOwner({u.position[0], u.position[1], u.position[2]}, u.shieldRatio);
  // a weapon's beam without the game's own effect on it: a plain band in a lightened army colour
  for (const FrameBeam& b : frame.beams) {
    if (effects.OnEntity(b.id) || !b.seen) continue;  // ([fogbeam] not one the army does not see)
    const Vec3 c = colorOf(b.army) * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
    ribbons.PlainBeam({b.start[0], b.start[1], b.start[2]}, {b.end[0], b.end[1], b.end[2]}, c, 0.25f);
  }
  textures.DropFetched();  // (what was read ahead and not used)
  if (wildlife && look == Look::Modern) wildlifeDraw.Listen(frame, combat_);  // (the fighting the birds hear: wildlife.h)
  combat_.clear();         // (handed over above to the looks that answer it: AddCombat)
  CpuTimes::Part(nullptr);
}

void World::PressUnit(const FrameUnit& u, const Frame& frame, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation) {
  if (blueprint.empty() || !model || !model->ok) return;
  // (a walker by the bones that stand for its feet, where its pose has them)
  const std::string& path = models.UnitBlueprintPath(blueprint);
  Vec3 feet[8];
  float turns[8] = {};  // [hdprints] (how far each foot is turned from the unit's way, from its rest: its own sole turned with it)
  int footCount = 0;
  for (uint32_t b : trackField.FootBones(model, path)) {
    // (at rest, unless the pose moves it; the way from this beat's pose to the next's, as the model is drawn)
    Vec3 now = model->bones[b].position * model->scale, next = now;
    Quat turned = model->bones[b].rotation;
    for (uint32_t i = u.firstBone; i < u.firstBone + u.boneCount && i < frame.bones.size(); ++i)
      if (frame.bones[i].bone == b) {
        now = next = {frame.bones[i].position[0], frame.bones[i].position[1], frame.bones[i].position[2]};
        turned = {frame.bones[i].orientation[0], frame.bones[i].orientation[1], frame.bones[i].orientation[2], frame.bones[i].orientation[3]};
      }
    for (uint32_t i = u.nextFirstBone; i < u.nextFirstBone + u.nextBoneCount && i < frame.bones.size(); ++i)
      if (frame.bones[i].bone == b) next = {frame.bones[i].position[0], frame.bones[i].position[1], frame.bones[i].position[2]};
    const Vec3 way = Rotate(Mul(turned, Conjugate(model->bones[b].rotation)), Vec3{0, 0, 1});
    turns[footCount] = std::fabs(way.x) + std::fabs(way.z) > 1e-3f ? std::atan2(way.x, way.z) : 0.0f;
    feet[footCount++] = position + Rotate(orientation, now + (next - now) * frame.blend);
  }
  trackField.AddUnit(u.id, path, position, orientation, (model->boundsMax - model->boundsMin) * model->scale, feet, footCount, model, turns);
}

// [hitches] (readAhead, world.h) Each frame: the next tiers' models and the pads' materials a step on (at most a texture or
// a model each), and the pipelines a game wants later made the first frame their looks are on (each made once: after
// that these are checks alone). As each look draws, so its pipeline is made here: with the same settings.
void World::PrepareAhead() {
  decals.tarmacs.readAhead = readAhead;
  models.readAhead = readAhead && haveMap_ && !sidePicture;
  if (!readAhead || !haveMap_ || sidePicture) return;
  if (aheadRoomFrames_++ % 60 == 0) {  // (room on the card for what is made ahead: aheadRoom_)
    uint64_t budget = 0, used = 0;
    aheadRoom_ = !device_->MemoryBudget(&budget, &used) || budget == 0 || used + (uint64_t{1} << 30) < budget / 100 * 85;
  }
  AheadBegun();  // [spikes]
  textures.MakeAheadStep(1.0);  // [spikes] (what was made ahead off the frames' thread, finished: about a millisecond at most)
  {  // [spikes] (the kinds made ahead: models made while they take less than a millisecond in all, one at least)
    const auto start = std::chrono::steady_clock::now();
    while (models.AheadStep() && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < 1.0) {
    }
  }
  if (decals.remasteredTarmacs) decals.tarmacs.Ahead();
  // [prebuild] the temporal upscaler's (FSR 3) and frame generation's compute pipelines, where they are on (as World::Begin
  // asks for them): a game's load draws its warm frames as photos (World::photo), for which Begin makes neither, so that
  // the game's first frames made them on the frames' thread (Chris's second test, 2026-11-04: 1.53 s and 1.03 s, the
  // driver's first time). Made here on the device's own thread, Begin then finds them made (fsr3State_, framegenState_)
  // and only switches them on; the same Init, so the same pipelines. (Begin's own Init stays for where this has not run:
  // --no-read-ahead, a side picture.) One that the device's thread cannot make draws nothing, as every pipeline asked for
  // in the background; its messages say so.
  if (gSceneSamples == 1 && motionView <= 0 && ((temporalUpscale && fsr3State_ == 0) || (frameGeneration && framegenState_ == 0))) {
    device_->BeginBackgroundPipelines();
    std::string error;
    if (temporalUpscale && fsr3State_ == 0) {
      fsr3State_ = fsr3.Init(&error) ? 1 : -1;
      if (fsr3State_ < 0) fprintf(stderr, "render: FSR 3 cannot be used (%s); FSR 1 stays\n", error.c_str());
    }
    if (frameGeneration && framegenState_ == 0) {
      framegenState_ = framegen.Init(&error) ? 1 : -1;
      if (framegenState_ < 0) fprintf(stderr, "render: FSR 3 frame generation cannot be used (%s)\n", error.c_str());
    }
    device_->EndBackgroundPipelines();
  }
  if (look != Look::Modern) return;
  // [session] what the sky and the weather make the first frame they are on (a game's sky comes with its time of day, which
  // the map's warm frames may not have had): the clouds' shaders with a nuke's ring, the weather on the horizon, the light
  // shafts; made on the device's own thread while not wanted yet (each once: after that these return at once)
  {
    device_->BeginBackgroundPipelines();
    if (volumetricClouds && volumeExplosions) clouds.PrepareRings();
    if (horizonWeather && terrain.beyondReach > 0) horizon.PrepareAhead();
    if (lightShafts && volumetricFog) sunlight.PrepareAhead();
    // [zoomhitch] what a zoom out made the first time it reached a height: the clouds laid over the scene (the eye in them)
    // and the dry ground's matt variants (the mud's: its first patch in view)
    if (cloudZoom && volumetricClouds) cloudZoomDraw.PrepareAhead();
    terrain.PrepareMatt();
    device_->EndBackgroundPipelines();
  }
  if (buildNow_) build.PrepareSparks();
  if (groundCover && (ao.strength > 0 || contactShadows)) ao.PrepareCover();
  if (halfOcclusion && (ao.strength > 0 || contactShadows)) ao.PrepareHalf();  // [step2b]
  if (groundCover && grassMade_ && plantsFar) grass.PrepareAhead(plantsReact);  // [plantlod] (the 3D plants' and the far cover's)
  // [prebuild] (Chris's second test, 2026-11-04: "light stuttering early on") what its hitch log had the game's first
  // minutes make the first frame each was wanted: the scene's motion of the ground cover and of what sways (a game's load
  // draws its warm frames as photos, which draw no motion), the ground cover with the blasts' waves, the drives' plumes,
  // the ships' wakes and the wash on their hulls (none of which a load's warm frames have). Each made here the first frame
  // its look is on, on the device's own thread (in a game's load: its warm frames, so the map waits for them as for the
  // rest, WorldPicture::MapReady), by the very code its first use ran, so the picture is the same; after that, checks.
  {
    const bool moving = (motionOn || motionView > 0 || temporalUpscale || frameGeneration) && gSceneSamples == 1;  // (as Begin's `moving`)
    const bool lifted = oceanWaves && oceanLift && map.water.present;  // (the ocean lifting the sea: World::Begin's)
    device_->BeginBackgroundPipelines();
    if (moving && plantMotion && motionView != 4) meshes.PrepareSwayMotion();
    if (groundCover && grassMade_) grass.PrepareVariants(plantsReact, blastWaves, moving && plantMotion && motionView != 4);
    if (exhaustsNow_) exhausts.Prepare();
    if (waterFxMirror && map.water.present) {  // [fxmirror] (the mirrored effects' pipelines: wanted with the first shot over water)
      particles.MirrorReady();
      ribbons.MirrorReady();
      if (volumes.Made()) volumes.MirrorReady();
      if (energyShields) energy.MirrorReady();
    }
    if (map.water.present) wakes.Prepare(lifted);
    if (lifted) hullWash.Prepare();
    device_->EndBackgroundPipelines();
  }
  if (surf && map.water.present) water.surf.PrepareSpray();
  if (litRain) falling.PrepareLit();
  if (rainSplashes) falling.PrepareSplashes();
  // [snowhitch] the snow's (before the wet ground's: its set read with the snow's) and the wet ground's, their pipelines made on
  // the device's own thread while they are not wanted yet (gfx.h, BeginBackgroundPipelines: a new build's driver takes up to
  // two seconds for each of these, which the first frame would wait for), at once when they are
  if (snowGround && timeOfDay >= 0 && SnowAhead()) {
    const bool now = snow > 0 || snowLying > 0;
    if (!now) device_->BeginBackgroundPipelines();
    PrepareSnow();
    if (!now) device_->EndBackgroundPipelines();
  }
  if (rainGround) {
    const bool now = rain > 0 || wet > 0;
    if (!now) device_->BeginBackgroundPipelines();
    terrain.PrepareWet();
    if (!now) device_->EndBackgroundPipelines();
  }
  // [whitecaps] the ocean's (World::oceanWaves, ocean.h): its compute pipelines and the water's ocean variants for the options
  // on now, made on the device's own thread while it is off, so that switching it on never pauses for the driver (at once,
  // World::Begin, while it is on). Nothing else of it is made while it is off (its fields, its foam: at its first frame).
  if (map.water.present && !oceanWaves) {
    device_->BeginBackgroundPipelines();
    water.ocean.PreparePipelines();
    water.PrepareOcean(waterRipples, winterWater, surf, waterDetail, oceanLift);
    device_->EndBackgroundPipelines();
  }
  // [snowhitch] the low plants under the snow ([undersnow]: the models' nine pipelines with UNDER_SNOW, made the first frame the
  // option is on with a time of day, as World::Render makes them): likewise on the device's own thread while no snow lies
  if (plantsUnderSnow && timeOfDay >= 0) {
    const bool now = snowLying > 0;
    if (!now) device_->BeginBackgroundPipelines();
    meshes.PrepareUnderSnow();
    if (!now) device_->EndBackgroundPipelines();
  }
}

// [plants] Plants that react (plants.h): what stands on the ground near the camera presses them (every model drawn that
// is not scenery, nor a shield's bubble or the like, and stands on the ground: its box from its bounds, turned as it is),
// the game's blasts and its lasting fires burn them; the field and the burnt picture made before the scene's pass, and
// handed to the grass's and the models' shaders (theirs made the first frame it is on).
void World::PreparePlants() {
  plantsNow_ = false;
  meshes.plantsConstants = nullptr;
  grass.plants = nullptr;
  if (!plantsReact || look != Look::Modern || !haveMap_ || sidePicture) return;
  device_->Mark("plants react");
  CpuTimes::Part("plants react");
  if (!meshes.PreparePlants()) return;
  if (groundCover && grassMade_) grass.PreparePlants();
  std::vector<float> boxes;
  // (the field's square: only what stands in it presses. The boxes were once taken in the order the models were added, the
  // first 256 of the whole map, and the field's own cull came after: in a large game the units under the camera were
  // past the 256th, and pressed nothing. No square: the camera too far for anything to be pressed, no boxes at all.)
  float square[4];
  const bool field = plantLife.FieldSquare(camera, square);
  // (a model's box on the ground, from its bounds, turned as it is: `row0..2` the world matrix's top three rows)
  auto press = [&](const Model* model, const float* row0, const float* row1, const float* row2) {
    const Vec3 lo = model->boundsMin, hi = model->boundsMax;
    const float cx = 0.5f * (lo.x + hi.x), cz = 0.5f * (lo.z + hi.z);
    const float x = row0[0] * cx + row0[2] * cz + row0[3], z = row2[0] * cx + row2[2] * cz + row2[3];
    // (its forward and sideways ways on the ground, and its half length and width in the world)
    const Vec3 forward{row0[2], row1[2], row2[2]}, side{row0[0], row1[0], row2[0]};
    const float halfLength = 0.5f * (hi.z - lo.z) * Length(forward) * 0.95f, halfWidth = 0.5f * (hi.x - lo.x) * Length(side) * 0.95f;
    const float r = halfLength + halfWidth;
    if (x + r < square[0] || z + r < square[1] || x - r > square[2] || z - r > square[3]) return;  // (not in the field)
    const float bottom = row1[0] * cx + row1[1] * lo.y + row1[2] * cz + row1[3];
    if (bottom > GroundHeight(x, z) + 1.0f) return;  // (in the air)
    const float flat = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (flat < 1e-5f) return;
    boxes.insert(boxes.end(), {x, z, forward.x / flat, forward.z / flat, halfLength, halfWidth, 0, 0});
  };
  if (field) meshes.ForEachAdded([&](const Model* model, const float* row0, const float* row1, const float* row2, const float*, bool scenery) {
    if (scenery || !model || !model->ok || model->lods.empty()) return;
    const ModelLod& l = model->lods[0];
    if (l.glass || l.shield || l.impact || l.frame || l.nuke || l.fades || l.unlit || l.shell) return;  // (light, not a body)
    press(model, row0, row1, row2);
  });
  // [groundfix] the game's wrecks are scenery (left out above), yet a wreck lies on what grows under it: each unit's wreck
  // in the squares under the field presses it as a unit's box does, for as long as it lies (gone, reclaimed: the plants
  // rise again). Through the props' squares, not every made prop: a game breaks its tree groups up into thousands.
  if (!propCells_.empty() && field) {
    const int cx0 = std::clamp(static_cast<int>((square[0] - propReach_) / kPropCell), 0, propCellsX_ - 1);
    const int cz0 = std::clamp(static_cast<int>((square[1] - propReach_) / kPropCell), 0, propCellsZ_ - 1);
    const int cx1 = std::clamp(static_cast<int>((square[2] + propReach_) / kPropCell), 0, propCellsX_ - 1);
    const int cz1 = std::clamp(static_cast<int>((square[3] + propReach_) / kPropCell), 0, propCellsZ_ - 1);
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (uint32_t id : propCells_[static_cast<size_t>(cz) * static_cast<size_t>(propCellsX_) + static_cast<size_t>(cx)].made) {
          auto made = madeProps_.find(id);
          if (made == madeProps_.end() || !made->second.unitWreck || !made->second.model || !made->second.model->ok) continue;
          const Mat4& m = made->second.world;
          press(made->second.model, m.m[0], m.m[1], m.m[2]);
        }
  }
  // (more in the field than the pass takes, PlantLife::kMostBoxes: the nearest to the field's middle are kept)
  Mark("plants: pressing boxes in the field", boxes.size() / 8, PlantLife::kMostBoxes);
  if (boxes.size() > 8 * static_cast<size_t>(PlantLife::kMostBoxes)) {
    const float mx = 0.5f * (square[0] + square[2]), mz = 0.5f * (square[1] + square[3]);
    std::vector<std::pair<float, uint32_t>> order(boxes.size() / 8);
    for (size_t i = 0; i < order.size(); ++i) {
      const float dx = boxes[i * 8] - mx, dz = boxes[i * 8 + 1] - mz;
      order[i] = {dx * dx + dz * dz, static_cast<uint32_t>(i)};
    }
    std::nth_element(order.begin(), order.begin() + PlantLife::kMostBoxes, order.end());
    std::vector<float> kept(8 * static_cast<size_t>(PlantLife::kMostBoxes));
    for (int i = 0; i < PlantLife::kMostBoxes; ++i) std::copy_n(&boxes[order[i].second * 8], 8, &kept[static_cast<size_t>(i) * 8]);
    boxes.swap(kept);
  }
  plantLife.WatchBlasts(blasts, clock_, decals.groundHeight);
  particles.HotEmitters(camera.target, 64, &plantFires_);
  plantLife.WatchFires(plantFires_, clock_, decals.groundHeight);
  plantLife.Prepare(view, camera, boxes, clock_);
  if (!plantLife.Ready()) return;
  plantsNow_ = true;
  meshes.plantsConstants = plantLife.Constants();
  meshes.plantsPress = plantLife.Press();
  meshes.plantsBurnt = plantLife.Burnt();
  grass.plants = &plantLife;
}

void World::PressGround(const Frame& frame) {
  if (!deepNow_ || !frame.blueprints) return;
  for (const FrameUnit& u : frame.units) {
    if (u.dead || !u.Seen() || u.fractionComplete < 1 || u.blueprint >= frame.blueprints->size()) continue;
    const std::string& blueprint = (*frame.blueprints)[u.blueprint];
    PressUnit(u, frame, blueprint, blueprint.empty() ? nullptr : models.Unit(blueprint), {u.position[0], u.position[1], u.position[2]},
              Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]});
  }
}

void World::DentGround(Vec3 at, float size) {
  if (!deepNow_ || !haveMap_ || !(size >= 0.1f)) return;
  const float ground = map.Height(at.x, at.z);
  if (map.water.present && ground < map.water.elevation - 0.05f) return;  // (under the water: the water's ripples)
  if (std::fabs(at.y - ground) > 0.5f + 0.5f * size) return;              // (in the air: an air burst, a dying aircraft)
  trackField.PressCrater({at.x, ground, at.z}, size);
}

// (as the game sizes the scorch it lays for a shot's impact, Blasts::FromImpact: 0.75 to 0.95 of the weapon's DamageRadius
// across, less below 100 damage; a direct hit's by its damage, at most 0.6. A weapon not known: a middling shell's.)
float World::ShotDentSize(const std::string& weapon) {
  auto found = dentSizes_.find(weapon);
  if (found != dentSizes_.end()) return found->second;
  float size = 0.3f;
  const size_t dash = weapon.find('-');
  if (dash != std::string::npos && dash > 0) {
    const std::string id = weapon.substr(0, dash);
    const int index = std::atoi(weapon.c_str() + dash + 1);  // (1-based, as FAF's weapon ids count)
    const size_t second = weapon.find('-', dash + 1);
    const std::string label = second == std::string::npos ? std::string() : weapon.substr(second + 1);
    BpValue bp;
    auto file = textures.ReadFile(models.UnitBlueprintPath(id));
    if (file && ReadBlueprintFile(*file, &bp)) {
      const std::vector<BpValue>& weapons = bp["Weapon"].items;
      const BpValue* w = index >= 1 && static_cast<size_t>(index) <= weapons.size() ? &weapons[static_cast<size_t>(index) - 1] : nullptr;
      if (!w || (!label.empty() && (*w)["Label"].StringOr(label) != label))  // (by its label, where the count does not match)
        for (const BpValue& v : weapons)
          if (v["Label"].StringOr(std::string()) == label) w = &v;
      if (w) {
        const float radius = static_cast<float>((*w)["DamageRadius"].NumberOr(0)), damage = static_cast<float>((*w)["Damage"].NumberOr(100));
        // ([dents] Chris, 2026-11-08: "too small", sized by the weapon - a big shell's hole much larger than small arms':
        // 1.3 x its radius at full damage, up to 1.4 x for the heaviest; a direct hit's from 0.3 to 1.2 by its damage)
        size = radius > 0 ? 1.3f * radius * std::clamp(0.01f * damage, 0.35f, 1.4f) : std::min(0.3f + damage / 500, 1.2f);
      }
    }
  }
  if (dentSizes_.size() > 4096) dentSizes_.clear();
  dentSizes_[weapon] = size;
  return size;
}

void World::Render() {
  const bool shadowed = haveMap_ && shadowsOn;
  const Camera& placing = Placing();  // (photomode: a photo's tile places as the whole photo; else the camera itself)
  post.glow = bloom ? 0.5f * std::max(bloomIntensity, 0.0f) / 1.5f : 0.0f;  // (the player's bloom options)
  // (and the original look's own bloom, as the original makes it: the same options, the map's Bloom)
  post.originalBloom.on = bloom && look == Look::Original;
  post.originalBloom.blurScale = std::max(bloomIntensity, 0.0f);
  post.originalBloom.copyAdd = haveMap_ ? map.lighting.bloom : 0.0f;
  // (a stage of the frame: timed on the graphics card and on the processor, for --timings)
  auto Stage = [this](const char* name) {
    device_->Mark(name);
    CpuTimes::Part(name);
  };
  Stage("snow tracks");
  // (the tracks' picture first: its own pass; deep tracks' field in its place while they are on, the kinds of ground
  // made the first time: it dispatches)
  // ([views] a view besides the first draws with the tracks the first one's frame pressed: none pressed again)
  if (deepNow_) {
    if (!extraView) {
      Stage("deep tracks");
      const bool weatherOn = timeOfDay >= 0;
      trackField.Draw(weatherOn ? snow : 0, weatherOn ? rain : 0, terrain.GroundKinds());
    }
  } else if (!extraView) {
    if (!sidePicture) trackField.Draw(0, 0, {});  // (off: nothing kept meanwhile)
    if (haveMap_) tracks.Draw(tracksCovering_, snowLying <= 0 || look != Look::Modern || timeOfDay < 0);
  }
  tracksCovering_ = 0;
  CpuTimes::Part("lights and lamp shadows placed");
  FlushLamps();  // [nightlight] (steadyLamps: the frame's lamps lit, once all are in)
  lights.steady = steadyLamps;
  lampShadowsDrawn = shadowed && lampShadows && look == Look::Modern ? lights.PlaceShadows(placing, view) : 0;
  // the light of flashes and explosions (the remastered look's: particles.h, Flash), fading over its time
  for (Particles::Flash& f : particles.flashes) {
    const float age = static_cast<float>(clock_ - f.born) / std::max(f.seconds, 1e-3f);
    if (age < 0 || age >= 1) continue;
    if (!f.colored) {
      const Vec3 c = lights.RampGlow(f.ramp);
      const float most = std::max({c.x, c.y, c.z, 1e-3f});
      f.color = c * (std::min(most * 3, 1.0f) / most);  // (as bright as its colour is strong, in its own hue)
      f.colored = true;
    }
    const float fade = (1 - age) * (1 - age);
    lights.Add(f.position + Vec3{0, 0.6f, 0}, f.radius, f.color * (1.6f * f.strength * fade * lights.night));
  }
  // lit smoke (particles.h): what burns on and on (a fire on a damaged unit, a burning wreck) is a small light too,
  // flickering, so that the smoke over it glows (the remastered effects' look, as the flashes; not a missile's
  // exhaust, whose shot throws its own)
  litSmokeNow_ = litSmoke && look == Look::Modern && haveMap_;
  particles.litSmoke = litSmokeNow_;
  particles.airLight = litSmokeNow_ && smokeAirLight != 0;  // [smoke air light] (-1: with lit smoke)
  particles.heat = heatShimmer && look == Look::Modern && haveMap_ && !sidePicture;
  if (litSmokeNow_ && particles.remastered && fireLight > 0) {
    particles.HotEmitters(placing.target, 48, &hot_);
    // the fires of one burning thing are one light: a wreck burns in a dozen places a pace apart, and a light for
    // each would add up to a glare (gathered within 3 units; a few together a little brighter than one, at most
    // twice). By day a fire's light is little beside the sun's: a third of it at noon.
    struct Fire {
      Vec3 position, color;
      float size, weight;
      uint32_t id;
      int count;
    };
    std::vector<Fire> fires;
    // (the light a flame throws is deeper than the colour its particles show: a texture's hot core is near white, and
    // their glow taken as the light made the smoke round a fire olive-yellow; mostly a flame's own orange, linear)
    const Vec3 flame{1.0f, 0.36f, 0.08f};
    for (const Particles::Hot& h : hot_) {
      if (h.moving) continue;
      const float most = std::max({h.color.x, h.color.y, h.color.z, 1e-3f});
      const Vec3 color = (h.color * (1 / most) * 0.25f + flame * 0.75f) * std::min(most * 3, 1.0f);
      auto joins = std::find_if(fires.begin(), fires.end(), [&](const Fire& f) { return Length(f.position - h.position) < 3.0f; });
      if (joins == fires.end()) {
        fires.push_back({h.position, color, h.size, h.size, h.id, 1});
        continue;
      }
      const float w = joins->weight + h.size;
      joins->position = (joins->position * joins->weight + h.position * h.size) * (1 / w);
      joins->color = (joins->color * joins->weight + color * h.size) * (1 / w);
      joins->size = std::max(joins->size, h.size);
      joins->weight = w;
      ++joins->count;
    }
    const float daylight = timeOfDay >= 0 && look == Look::Modern ? skyLight.daylight : 1.0f;
    const float byDay = 1 - 0.65f * daylight;
    for (const Fire& f : fires) {
      const float t = static_cast<float>(clock_) * 9.0f + static_cast<float>(f.id % 64) * 1.7f;
      const float flicker = 0.8f + 0.13f * std::sin(t) + 0.07f * std::sin(t * 2.3f + 1.1f);
      const float together = std::min(1 + 0.35f * static_cast<float>(f.count - 1), 2.0f);
      lights.Add(f.position + Vec3{0, std::min(f.size * 0.5f, 1.5f), 0}, std::clamp(f.size * 4.0f, 2.5f, 10.0f),
                 f.color * (fireLight * flicker * together * byDay * lights.night));
    }
  }
  // hot metal (hot_metal.h): the hottest few wrecks' and craters' faint light, the cooling wrecks' thin smoke
  if (hotOn_ && !sidePicture) {
    if (!extraView)  // [views] (a view besides the first: the first's glows, their lights alone; the smoke is the first's)
      decals.ForEachHot(clock_, [&](Vec3 centre, float across, float age, float seconds, uint32_t id) {
        heat.Glowing((uint64_t{2} << 40) | id, centre, across * 0.25f, age, seconds, false);
      });
    if (!extraView && blastDebris)  // [bakeddebris] (a baked death's hot chunks: their light, as a wreck's; no smoke of their own)
      debris.baked.ForEachHot([&](Vec3 centre, float radius, float age, float seconds, uint32_t id) {
        heat.Glowing((uint64_t{3} << 40) | id, centre, radius, age, seconds, false);
      });
    heat.End(&lights, extraView ? nullptr : &particles, placing.target, lights.night);
  }
  // (volume explosions) which are drawn this frame, their light while hot, a nuke's flash and the ground's glow under it;
  // its ring through the clouds
  volumesDrawn_ = false;
  clouds.ringsWanted = volumesOn_;
  clouds.ringStrength = 0;
  if (volumesOn_ && !sidePicture) {
    volumesDrawn_ = volumes.Update(clock_, camera, view) && hide.find("particles") == std::string::npos;
    volumes.AddLights(&lights, lights.night, timeOfDay >= 0 && look == Look::Modern ? skyLight.daylight : 1.0f);
    volumes.CloudRing(clock_, &clouds.ringX, &clouds.ringZ, &clouds.ringRadius, &clouds.ringWidth, &clouds.ringStrength);
  }
  if (buildNow_) build.AddLights(&lights, lights.night);  // (buildfx: the build beams' lights, the sparks thrown)
  if (exhaustsNow_) exhausts.AddLights(&lights, lights.night);  // (unitsfx: a light per drive)
  lights.shade = lightShade && look == Look::Modern;  // [lightshade] (the larger lights behind the map's ridges)
  lights.ground = haveMap_ ? &map : nullptr;
  lights.Build(&view);  // (after everything has said what lights there are; before any pass reads the view)
  // blasts (blasts.h): the waves and the dust under way near what the camera sees; the debris's chunks moved on and listed
  blastsNow_ = grassBlasts_ = false;
  if (look == Look::Modern && haveMap_ && !sidePicture && (blastWaves || blastDust)) {
    // (what sways, and the grass, are drawn by shaders of their own while waves are under way; they and the blasts' own
    // are made the first frame the options are on, not at the first blast: no stall in the middle of a fight)
    const bool waves = blastWaves && meshes.PrepareBlasts();
    if (waves && groundCover) grass.PrepareBlasts();
    blasts.Prepare(clock_, view, placing.target, waves, blastDust);
    blastsNow_ = blasts.Active() > 0;
    if (blastsNow_ && blastDust) terrain.GroundColours();  // (made the first time here: it dispatches, so not in a pass)
  }
  meshes.blastConstants = blastsNow_ && blasts.Waves() ? blasts.Constants() : nullptr;
  meshes.blastConstantsSize = meshes.blastConstants ? blasts.ConstantsSize() : 0;
  if (dustNow_) {  // movement dust (movement_dust.h): what is over let go of, the rest to the card; the ground's colours
                   // made the first time (it dispatches, so not in a pass)
    CpuTimes::Part("movement dust listed");
    dust.Prepare(view);
    if (dust.DustDrawn() > 0) terrain.GroundColours();
  }
  if (surfNow_) {  // [surf] the spray: the rocks' where waves arrive now, what is over let go of, the rest to the card
    CpuTimes::Part("surf's spray listed");
    water.surf.Prepare(view);
  }
  if ((blastDebris || rocksNow_) && look == Look::Modern && haveMap_ && !sidePicture) {  // ([rockhits] the rocks' chips are the debris's)
    CpuTimes::Part("debris moved");
    debris.rubbleLasts = battleScars ? 0.0f : 120.0f;  // (kept as the scorches are)
    debris.hot = hotOn_;            // (fresh pieces' torn faces glow with hot metal on)
    debris.smoke = &particles;      // (the biggest trail smoke)
    // [whitecaps] (on the lifted waves: where they have the surface, as what floats rides them, Ocean::BobAt)
    if (water.ocean.Riding()) {
      debris.waterRise = [this](float x, float z) {
        Ocean::Bob bob;
        return water.ocean.BobAt(x, z, 0.3f, &bob) ? bob.rise : 0.0f;
      };
    } else {
      debris.waterRise = nullptr;
    }
    debris.Update(clock_, view.eye, &meshes, &blasts);
  }
  {  // the bones of posed models, once for all the frame's passes
    std::vector<const View*> views{&view};
    if (shadowed)
      for (int i = 0; i < shadows.Count(); ++i)
        if (shadows.Draws(i)) views.push_back(&shadows.CascadeView(i));  // ([cardcost] a far cascade kept: not drawn)
    for (int i = 0; i < lampShadowsDrawn; ++i) views.push_back(&lights.ShadowView(i));
    CpuTimes::Part("bones posed");
    meshes.Prepare(views);
  }
  sight.Upload();  // (the fog of war's picture, where a new grid came since the last frame: sight.h)
  if (!extraView) PreparePlants();  // [plants] ([views] the first view's press and burns kept for the others) (plants that react: what presses and burns them; it draws and dispatches, before the scene's pass)
  if ((rayView || bounceNow_ || reflectNow_ || lightRaysNow_) && RaysReady()) {  // the ray tracing scene, from the models just prepared
    Stage("ray tracing scene");
    rays.refit = rayRefit;  // [cardcost]
    rays.Build(meshes);
    terrain.GroundColours();  // (made the first time here: it dispatches, so not in a pass)
  }
  // ground cover: what presses the grass flat (what stands on the ground near the camera: every model drawn that is
  // not scenery, its footprint from its bounds), then where it lies this frame (its shader made the first time)
  grassNow_ = false;
  if (groundCover && look == Look::Modern && !sidePicture && haveMap_) {
    grass.plantsFar = plantsFar;  // [plantlod] (and with it the plants from any distance; before Init: its shaders for them)
    grass.plantsRound = plantsRound;
    grass.plantModels = plantModels;
    if (!grassTried_) {
      grassTried_ = true;
      std::string why;
      grassMade_ = grass.Init(&why);
      if (!grassMade_) fprintf(stderr, "render: ground cover cannot be drawn (%s)\n", why.c_str());
    }
    // (cinecam: a cinematic camera by the eye's height over the ground under it, grass.cpp)
    const Vec3 eye = placing.Eye();
    if (grassMade_ && ((placing.cinematic ? eye.y - GroundHeight(eye.x, eye.z) : placing.distance) <= grass.farthest || grass.plantsFar)) {
      std::vector<float> discs;
      // [nightlight] (steadyLamps: all of them first, then the 4,096 nearest what the camera looks at, nearest first, so that
      // the near field's own 256 (GroundCover::Prepare) are the nearest too; before, the first 1,024 in the order the models
      // came in: in a late game some 3,000 stand on the ground, and which of them cleared the plants round them changed as
      // units came and went, plants near groups of units popping in and out, and units past the first 1,024 standing in
      // plants)
      const size_t discsMost = steadyLamps ? std::numeric_limits<size_t>::max() : 3 * 1024;
      meshes.ForEachAdded([&](const Model* model, const float* row0, const float* row1, const float* row2, const float*, bool scenery) {
        if (scenery || !model || discs.size() >= discsMost) return;
        const float x = row0[3], z = row2[3];
        const float scale = std::sqrt(row0[0] * row0[0] + row1[0] * row1[0] + row2[0] * row2[0]);
        if (row1[3] + model->boundsMin.y * scale > GroundHeight(x, z) + 1.5f) return;  // (in the air)
        const float half = std::max({std::fabs(model->boundsMin.x), std::fabs(model->boundsMax.x), std::fabs(model->boundsMin.z),
                                     std::fabs(model->boundsMax.z)});
        discs.insert(discs.end(), {x, z, half * scale * 0.9f + 0.1f});
      });
      if (debugNight_) nightDiscs_ = std::max(nightDiscs_, static_cast<int>(discs.size() / 3));  // (MOHO64_DEBUG_NIGHTLIGHT)
      if (steadyLamps && discs.size() > 3) {  // [nightlight] (the nearest first; ties by place, not by the order they came in)
        const size_t count = discs.size() / 3, kept = std::min<size_t>(count, 4096);
        std::vector<std::pair<float, uint32_t>> by(count);
        for (size_t i = 0; i < count; ++i) {
          const float dx = discs[i * 3] - placing.target.x, dz = discs[i * 3 + 1] - placing.target.z;
          by[i] = {dx * dx + dz * dz, static_cast<uint32_t>(i)};
        }
        std::partial_sort(by.begin(), by.begin() + static_cast<std::ptrdiff_t>(kept), by.end());
        std::vector<float> nearest;
        nearest.reserve(3 * kept);
        for (size_t k = 0; k < kept; ++k) nearest.insert(nearest.end(), discs.begin() + by[k].second * 3, discs.begin() + by[k].second * 3 + 3);
        discs.swap(nearest);
      }
      Stage("ground cover pressed flat");
      // (and where the plants grow, made the first time: it dispatches, so before the scene's pass)
      const bool coverMade = plantsReady_ && !coverA_;  // [cover]
      if (plantsReady_ && !coverA_) terrain.CoverDensity(treeMask_, &coverA_, &coverB_);
      // [cover] where nothing grows: the streams, the trails through the meadow (made the first time: it dispatches)
      grass.bare = grass.bareMap.Picture(plantsReady_ ? coverB_ : gfx::Texture{}, terrain.CoverMask(), coverMade);
      grass.tracks = deepNow_ ? trackField.Atlas() : gfx::Texture{};  // (deep tracks: the grass laid flat along them)
      grass.tracksField = deepNow_ ? trackField.Constants() : TrackFieldConstants{};
      grass.Prepare(view, placing, discs);
      grassNow_ = true;
      // [tarmac] none where a remastered pad lies: its coverage over the grass's square (between passes: it draws)
      grass.cleared = {};
      // ([plantlod] with plantsFar, the pads and the waves only where the grass's square is laid out this frame)
      const bool square = !grass.plantsFar || grass.FieldsReady();
      if (decals.remasteredTarmacs && square) {
        decals.TarmacPads(clock_, &padsNear_);
        grass.cleared = decals.tarmacs.ClearCover(view, grass.PadArea(), padsNear_);  // ([foliage] on its own grid)
      }
      if (blastsNow_ && blasts.Waves() && square && grass.PrepareBlasts()) grassBlasts_ = blasts.PrepareField(grass.Area());  // (the blasts' waves over it)
      if (grass.plantsFar) {  // [plantlod] the plants' tiles over the view, what clears them beyond the square, the models' placing
        const float water = map.water.present ? map.water.elevation : -1e9f;
        grass.PrepareFar(view, placing, terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours(), static_cast<float>(map.width),
                         static_cast<float>(map.height), water, plantsReady_ ? coverA_ : gfx::Texture{}, plantsReady_ ? coverB_ : gfx::Texture{});
        bool padsDrawn = false;
        if (decals.remasteredTarmacs && grass.FarField()) {
          if (!square) decals.TarmacPads(clock_, &padsNear_);
          padsDrawn = static_cast<bool>(decals.tarmacs.ClearCover(view, grass.FarArea(), padsNear_, grass.FarField(), GroundCover::kFarField));
        }
        grass.PlaceFar(view, placing, terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width), static_cast<float>(map.height), water,
                       padsDrawn);
      }
    }
  }
  if (bounceNow_) {  // bounce light: the probes' rays and their blending
    Stage("bounce light");
    lights.Bind();  // (what the rays meet takes the small lights too)
    bounce.Update(view, rays, terrain.NormalMap(), terrain.GroundColours(), sky_);
  }
  if (extraView) water.ocean.PlaceGridFor(camera, view);  // [views] (the first view's sea, its grid for this view)
  if (water.ocean.On() && !extraView) {  // [ocean] ([views] worked out once a frame, by the first view) the wind sea worked out for this frame, its foam moved on (it dispatches: before the scene's pass)
    Stage("ocean");
    water.ocean.Update(camera, view, terrain.HeightTexture(), terrain.HeightUnit());
  }
  if (water.ripples.On() && !extraView) {  // ripples on the water ([views] once a frame): their field moved on (it dispatches: before the scene's pass)
    Stage("water ripples");
    water.ripples.Update(camera, view.height, terrain.HeightTexture(), terrain.HeightUnit());
  }
  // wildlife (wildlife.h): the flocks moved on and what is drawn chosen (it dispatches: before the scene's pass)
  wildlifeNow_ = false;
  if (wildlife && look == Look::Modern && haveMap_ && !sidePicture) {
    Stage("wildlife");
    wildlifeNow_ = wildlifeDraw.Update(view, camera, time_, timeOfDay >= 0 ? skyLight.body.y : 1.0f, rain, snowLying, particles.flashes,
                                       terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours());
  } else if (!sidePicture) {
    wildlifeDraw.Pause();
  }
  // (beyondland) the land far off (beyond_land.h): made for the map the first time it is wanted, its colours from the
  // map's ground (that dispatches: before the scene's pass)
  distantLand.Prepare(device_, beyondLand && view.constants.beyond[2] > 0 && !sidePicture, terrain);
  // (surround) the map's sculpted surround: what is drawn made the first time, with the map's colours (that dispatches)
  surround.Prepare(device_, mapSurround && !terrain.beyondCopies && view.constants.beyond[2] > 0, terrain, distantLand);
  // [seasons] leaves in the autumn wind (seasons.h), on a map with an autumn look: which of its broadleaf trees shed this
  // frame (those still standing as they were), and the squares of ground round the camera where its cover grows (the
  // cover's density made the first time, as the ground cover makes it: it dispatches)
  leavesNow_ = false;
  if (fallingLeaves && seasonOpen_ == Season::Autumn && terrain.SeasonOf().listed && look == Look::Modern && haveMap_ && !sidePicture &&
      (leaves.Trees() || plantsReady_)) {
    if (plantsReady_ && !coverA_) terrain.CoverDensity(treeMask_, &coverA_, &coverB_);
    const bool weather = timeOfDay >= 0;
    leavesNow_ = leaves.Prepare(view, camera, weather ? rain : 0.0f, weather ? snow : 0.0f, [this](uint32_t key) {
      if (key >= props_.size()) return false;
      const PlacedProp& p = props_[key];
      if (!p.model || p.model->lods.empty() || p.model->lods[0].sway <= 0) return false;  // (gone, or burnt)
      const float up = std::sqrt(p.world.m[0][1] * p.world.m[0][1] + p.world.m[1][1] * p.world.m[1][1] + p.world.m[2][1] * p.world.m[2][1]);
      return p.world.m[1][1] > 0.85f * up;  // (not felled)
    }, plantsReady_ ? coverA_ : gfx::Texture{});
  }
  CpuTimes::Part("particles and bands moved");
  KeepMapEffects();  // [mapfx]
  particles.Update(clock_, &view);
  ribbons.Update(clock_);

  // [snow ground] the snow's shelter, made the first time snow lies with the option on (it dispatches: before the scene's pass)
  // (the props' raster let go once it is made: made again, should the shelter be)
  if (snowGround && look == Look::Modern && haveMap_ && timeOfDay >= 0 && snowLying > 0 && terrain.SnowShelterWanted()) {
    terrain.SnowShelter(SnowProps());
    if (snowProps_) device_->Destroy(snowProps_);
    snowProps_ = {};
    snowPropsMade_ = false;
  }
  // [decalsnow] the marks' pipelines that lay the same snow (made the first time, before the scene's pass)
  if (snowGround && look == Look::Modern && haveMap_ && timeOfDay >= 0 && snowLying > 0) decals.PrepareSnowGround();
  // [undersnow] the low plants under lying snow (under_snow.h): their shaders made the first frame the option is on with the
  // weather (not when the first snow lies: no stall then), and while snow lies the snow over them as the ground cover has
  // it, for every pass of the models from here on
  meshes.underSnow.depth = 0;
  meshes.underSnow.shelter = {};
  if (plantsUnderSnow && look == Look::Modern && haveMap_ && timeOfDay >= 0 && meshes.PrepareUnderSnow() && snowLying > 0) {
    meshes.underSnow.depth = grass.snowDepth;
    meshes.underSnow.shelter = terrain.SnowShelterPicture();
    meshes.underSnow.materials = terrain.SnowMaterialsLaid();
    meshes.underSnow.groundNormals = terrain.NormalMap();
    meshes.underSnow.mapSize[0] = static_cast<float>(map.width);
    meshes.underSnow.mapSize[1] = static_cast<float>(map.height);
    meshes.underSnow.wind[0] = terrain.snowWind[0];
    meshes.underSnow.wind[1] = terrain.snowWind[1];
    meshes.underSnow.tune[0] = terrain.snowTune[1];
    meshes.underSnow.tune[1] = terrain.snowTune[3];
  }
  // the sun's shadow maps first, then the scene into its own picture, then that onto the window
  Stage("sun shadows");
  if (shadowed) {
    for (int i = 0; i < shadows.Count(); ++i) {
      if (!shadows.Draws(i)) continue;  // [cardcost] (a far cascade kept from a frame before: shadows.h, keepEvery)
      device_->BeginPass(shadows.Pass(i));
      terrain.Draw(shadows.CascadeView(i), true);
      meshes.Draw(shadows.CascadeView(i), environment_, true);
      if (wildlifeNow_) wildlifeDraw.DrawBirds(shadows.CascadeView(i), true);  // (the birds' shadows)
      shadows.Drawn(i);  // [cardcost]
    }
  }
  // the lamps' shadow maps: units and scenery as each lamp sees them
  Stage("lamp shadows");
  for (int i = 0; i < lampShadowsDrawn; ++i) {
    device_->BeginPass(lights.ShadowPass(i));
    meshes.Draw(lights.ShadowView(i), environment_, true);
  }
  if (volumesDrawn_) {  // (volume explosions) each drawn blast's volume at this moment, before the smoke's shadow takes it
    Stage("volume blasts filled");
    volumes.Fill(view);
  }
  // smoke's shadow (smoke.h): the smoke as the sun sees it, for the smoke lit as a volume and (smokeShadows) for what
  // the sun lights under it; before the light in the air and the scene read it
  Stage("smoke's shadow");
  smokeNow_ = false;
  if ((litSmokeNow_ || (smokeShadows && look == Look::Modern && haveMap_)) && !sidePicture && hide.find("particles") == std::string::npos &&
      smokeMap.Place(placing, Vec3{view.constants.sunDirection[0], view.constants.sunDirection[1], view.constants.sunDirection[2]}, &view,
                     smokeShadows && look == Look::Modern)) {
    particles.Gather();
    device_->SetTexture(kSmokeShadowSlot, {});
    device_->BeginPass(smokeMap.Pass());
    particles.DrawSmokeShadow(smokeMap.SunView());
    if (volumesDrawn_) volumes.DrawShadow(smokeMap.SunView());  // (volume explosions)
    device_->SetTexture(kSmokeShadowSlot, smokeMap.Map());
    smokeNow_ = true;
  }
  // the volumetric clouds: where they are and their shadows, before the light in the air and the scene read them
  Stage("clouds' shadows");
  // (cloudzoom) the zoom through them (cloud_zoom.h): whether the layer stays where it is this frame, and how thin
  // (sky) and with it on, the layer lifted by the map's size (CloudZoom::Place)
  cloudZoomDraw.Place(&clouds, cloudZoom && volumetricClouds && look == Look::Modern && haveMap_, static_cast<float>(map.width), static_cast<float>(map.height));
  clouds.zoom = cloudZoomDraw.For(camera, clouds, cloudZoom && volumetricClouds && look == Look::Modern && haveMap_);
  {  // [stormcells] the storm the clouds show: rain over 0.6 (a rain spell's is 0.5, a storm's 1)
    const float s = std::clamp((rain - 0.6f) / 0.35f, 0.0f, 1.0f);
    clouds.storm = stormCells ? s * s * (3 - 2 * s) : 0.0f;
  }
  if (sidePicture) clouds.Skip(&view);  // (a frame for something else: the game's frames before kept)
  else clouds.Prepare(&view, camera, volumetricClouds && look == Look::Modern && haveMap_);
  // [sunlight] the drifting cloud shadows' picture (sunlight.h), in the clouds' own shadows' place from here on
  gfx::Texture cloudShadowsNow = clouds.Shadows();
  if (driftingShadows && !sidePicture) Stage("drifting cloud shadows");
  if (!sidePicture && sunlight.Drift(&view, clouds.Shadows())) {
    cloudShadowsNow = sunlight.DriftPicture();
    device_->SetTexture(kCloudShadowSlot, cloudShadowsNow);
  }
  // light in the air: its volume, between the shadow maps and the scene that reads it
  Stage("light in the air");
  air.sunlight = view.constants.shaftBand[0] > 0;  // [sunlight] (the light shafts' band and hazes: sunlight.h)
  air.heights = terrain.HeightTexture();
  air.woods = sunlight.Woods();
  air.heightUnit = haveMap_ ? terrain.HeightUnit() : 0.0f;  // (airfix: no air under the ground, air.h)
  air.mapWidth = static_cast<float>(map.width);
  air.mapHeight = static_cast<float>(map.height);
  air.waterLevel = haveMap_ && map.water.present ? map.water.elevation : -1e9f;
  if (sidePicture) air.Skip(&view);
  else air.Build(&view, camera, shadows.Map(), lights.ShadowMap(), &lights, volumetricFog && look == Look::Modern && haveMap_, groundMiddle_);
  if (volumesDrawn_) {  // (volume explosions) the light in each volume, after the shadows it takes, before the scene
    Stage("volume blasts lit");
    device_->SetTexture(kEnvironmentSlot, sky_);
    volumes.Light(view, shadows.Map(), cloudShadowsNow, smokeNow_ ? smokeMap.Map() : gfx::Texture{}, &lights);
  }
  // the sky from tables (when on): its view from the eye this frame, before the sky's pass and the surfaces read it
  bool skyTable = false;
  if (view.constants.skyTable[0] > 0.5f && view.constants.skyZenith[3] > 0.5f) {
    Stage("sky's view");
    skyTable = skyDraw.BuildTables(view, skyLight);
  }
  if (!skyTable) view.constants.skyTable[0] = 0;  // (where it cannot be built: the fitted sky, as without it)
  // [shadowmask] the sun's shadow on the ground worked out once a visible pixel (sun_mask.h): the ground's depth alone (with
  // what lies beyond the map's edge), and the mask from it, which the ground's pass and the contact shadows read
  view.constants.shadowMask[0] = 0;
  terrain.sunMaskRows = {};
  if (shadowMask && shadowed && look == Look::Modern && !sidePicture && sunMask.Size(view.width, view.height)) {
    Stage("sun shadow mask: the ground's depth");
    device_->BeginPass(sunMask.DepthPass());
    terrain.Draw(view, true, true);
    Stage("sun shadow mask");
    device_->SetTexture(kShadowMapSlot, shadows.Map());
    sunMask.groundNormals = terrain.NormalMap();
    sunMask.groundSize[0] = static_cast<float>(map.width);
    sunMask.groundSize[1] = static_cast<float>(map.height);
    sunMask.Compute(&view);
    terrain.sunMaskRows = sunMask.Rows();
  }
  const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
  post.BeginScene(view.width, view.height, clear);
  device_->SetTexture(kShadowMapSlot, shadows.Map());
  device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
  device_->SetTexture(kEnvironmentSlot, sky_);
  device_->SetTexture(kAirSlot, air.Volume());
  device_->SetTexture(kCloudShadowSlot, cloudShadowsNow);  // [sunlight] (the clouds' own, or the drifting shadows' with them)
  device_->SetTexture(kSkyTableSlot, skyTable ? skyDraw.SkyView() : gfx::Texture{});
  if (bounceNow_) bounce.Bind();
  if (reflectNow_ || lightRaysNow_) rays.Bind(terrain.NormalMap(), terrain.GroundColours());  // (for the surfaces that trace)
  lights.Bind();
  if (haveMap_) {
    Stage("ground");
    terrain.Draw(view);
    surround.Draw(view);  // (surround) the land past the map's edges, where the map brings it (not drawn: nothing)
    Stage("marks on the ground");
    // decals on the ground, before anything stands on it: they read the ground's depth
    // ([decalsnow] the snow the ground has just laid from its shelter, for the marks to lay the same)
    decals.snowGround.shelter = terrain.SnowShelterLaid();
    decals.snowGround.materials = terrain.SnowMaterialsLaid();
    std::copy(terrain.snowWind, terrain.snowWind + 2, decals.snowGround.wind);
    std::copy(terrain.snowTune, terrain.snowTune + 4, decals.snowGround.tune);
    decals.snowGround.mapWidth = static_cast<float>(map.width);
    decals.snowGround.mapHeight = static_cast<float>(map.height);
    // [churn] (the shell holes the ground has just drawn: the game's flat marks give way where one is; with the holes
    // added up, as before, the marks lie over them as before)
    decals.holes.field = deepNow_ && churnedHoles ? trackField.Atlas() : gfx::Texture{};
    decals.holes.kinds = decals.holes.field ? terrain.GroundKinds() : gfx::Texture{};
    decals.holes.constants = deepNow_ ? trackField.Constants() : TrackFieldConstants{};
    if (decals.Count() && hide.find("decals") == std::string::npos) {
      // [settled] (the settled scars in view laid into their own picture, in passes of its own; Draw lights it once)
      decals.LaySettled(view, camera, post.Depth(), clock_);
      post.BeginColorOnly();
      lights.Bind();
      decals.Draw(view, camera, post.Depth(), clock_);
      post.ContinueScene();
      lights.Bind();
    }
    if (plantsNow_ && plantLife.AnyBurnt()) {  // [plants] the ground where it burnt, darker (as the marks: it reads the ground's depth)
      Stage("burnt ground");
      post.BeginColorOnly();
      plantLife.DrawGround(view, camera, post.Depth());
      post.ContinueScene();
      lights.Bind();
    }
    // [plantlod] where the ground cover's plants are no longer drawn, their colour on the ground (it reads the ground's depth)
    if (grassNow_ && grass.FarCoverWanted()) {
      Stage("ground cover's far cover");
      post.BeginColorOnly();
      grass.DrawFarCover(view, camera, post.Depth());
      post.ContinueScene();
      lights.Bind();
    }
    // ground cover near the camera (prepared before the pass)
    if (grassNow_) {
      Stage("ground cover");
      // [coverdark] left out of the ambient occlusion (ambient_occlusion.h, CoverBefore): the ground's depth kept before it
      const bool coverOut = look == Look::Modern && (ao.strength > 0 || contactShadows);
      if (coverOut) {
        ao.CoverBefore(view, post.Depth());
        post.ContinueScene();
        lights.Bind();
      }
      // [leftover] under lying snow: with the snow ground, its shelter (deeper in hollows and the lee, thinner under trees)
      grass.snowShelter = terrain.SnowShelterPicture();
      grass.snowMaterials = terrain.SnowMaterialsLaid();  // [plantlod] (the snow's mottled margin)
      grass.snowWind[0] = terrain.snowWind[0];
      grass.snowWind[1] = terrain.snowWind[1];
      std::copy(terrain.snowTune, terrain.snowTune + 4, grass.snowTune);
      grass.Draw(view, camera, terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours(), terrain.NormalMap(),
                 static_cast<float>(map.width), static_cast<float>(map.height), map.water.present ? map.water.elevation : -1e9f,
                 plantsReady_ ? coverA_ : gfx::Texture{}, plantsReady_ ? coverB_ : gfx::Texture{}, grassBlasts_ ? blasts.Field() : gfx::Texture{});
      if (coverOut) {  // [coverdark] (and where it lies)
        ao.CoverAfter(view, post.Depth());
        post.ContinueScene();
        lights.Bind();
      }
    }
    // range rings on the land, before anything stands on it (their picture is made from the
    // ground's depth; over the water they are laid after the water)
    Stage("units and scenery");
    // (movement dust, movement_dust.h: over the scenery and under the units, which are drawn over it: it never hides them)
    const bool dustUnder = dustNow_ && dust.Under();
    auto drawDustUnder = [&] {
      Stage("movement dust: under the units");
      lights.Bind();
      dust.DrawUnder(view, post.Depth(), camera.zNear, terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours(), static_cast<float>(map.width),
                     static_cast<float>(map.height), litSmokeNow_);
      Stage("units, over the movement dust");
    };
    if (ranges.Any()) {
      ranges.waterLevel = map.water.present ? map.water.elevation : -1e9f;
      ranges.night = lights.night;
      ranges.Build(view, camera, post.Depth());
      // (the trees and rocks first, the lines over them, then the units over the lines: in the
      // original a ring shows across a wood and is hidden where a unit stands)
      post.ContinueScene();
      lights.Bind();
      meshes.Draw(view, environment_, false, false, 1);
      post.BeginColorOnly();
      ranges.Lay(view, camera, post.Depth(), false);
      if (dustUnder) drawDustUnder();
      post.ContinueScene();
      lights.Bind();
      meshes.Draw(view, environment_, false, false, 2);
    } else if (dustUnder) {  // (movement dust: the scenery, the dust over it, the units over the dust)
      meshes.Draw(view, environment_, false, false, 1);
      post.BeginColorOnly();
      drawDustUnder();
      post.ContinueScene();
      lights.Bind();
      meshes.Draw(view, environment_, false, false, 2);
    } else {
      meshes.Draw(view, environment_);
    }
    draw.Flush(view);
    if (wildlifeNow_) {  // the birds, with what is solid
      Stage("wildlife: birds");
      wildlifeDraw.DrawBirds(view);
    }
    if (horizonNow_) {  // [horizon] the icebergs far out, with what is solid (before the water, which foams round them)
      Stage("horizon: icebergs");
      horizon.DrawSolid(view);
    }
    // [shadowmask] (the ground's pass has read the mask; of what comes after only the contact shadows do, where they see the
    // ground: AmbientOcclusion::sunMask)
    view.constants.shadowMask[0] = 0;
    ao.sunMask = terrain.sunMaskRows;
    ao.sunMaskDepth = terrain.sunMaskRows ? sunMask.Depth() : gfx::Texture{};
    ao.sunMaskWords = sunMask.Words();
    ao.sunMaskHeight = sunMask.Height();
    terrain.sunMaskRows = {};
    // ambient occlusion: from the depth of everything solid, laid over it before the water
    Stage("ambient occlusion");
    // ((dusk turn) they darken toward the falling light whatever its colour: none at the moment it changes sides)
    ao.contactShadows = contactShadows ? 0.8f * (timeOfDay >= 0 && look == Look::Modern ? skyLight.fallIn : 1.0f) : 0.0f;
    ao.pixelScale = photoPixel;  // (photofix2: a photo's frame reaches as far as the window's)
    ao.half = halfOcclusion;     // [step2b]
    if (look == Look::Modern && (ao.strength > 0 || ao.contactShadows > 0)) {
      ao.Compute(view, camera, post.Depth());
      post.ContinueScene();
      ao.Apply(view);
    }
    if (distantLand.On()) {  // (beyondland) the land far off: after the map's own (no occlusion out there), before the clouds and the sky
      Stage("land far off");
      lights.Bind();
      distantLand.Draw(view);
    }
    if (leavesNow_) {  // [seasons] the falling leaves, with what is solid (after the ambient occlusion: no halo round them)
      Stage("falling leaves");
      lights.Bind();
      leaves.Draw(view, terrain.HeightTexture(), terrain.HeightUnit());
    }
    // the clouds as volumes (when on): from the depth of everything solid, before the sky lays them over itself
    if (clouds.Shadows()) {
      Stage("clouds");
      clouds.Build(view, camera, skyLight, post.Depth());
      post.ContinueScene();
      lights.Bind();
    }
    Stage("sky");
    skyDraw.starry = starryNight ? 1.0f : 0.0f;  // [starry]
    skyDraw.Draw(view, camera, skyLight, haveMap_ ? sky_ : gfx::Texture{}, clouds.Picture());  // where nothing solid is
    if (streamNow_) {  // [stream] the streams as water (stream_water.h): over what is solid, under the sea, seeing through to a copy
      Stage("stream water");
      // (only with a stream in the view: the whole scene was copied for them every frame, anywhere on a map that has one)
      const std::function<bool(uint32_t)> standing = [this](uint32_t key) { return key < props_.size() && props_[key].model != nullptr; };
      if (!streams.InView(view, standing)) {
        streams.drawn = streams.triangles = 0;
      } else if (post.CopyBehind()) {
        post.BeginColorOnly();
        lights.Bind();
        device_->SetTexture(kShadowMapSlot, shadows.Map());
        device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
        streams.Draw(view, camera, post.Behind(), post.Depth(), terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width),
                     static_cast<float>(map.height), standing);
        post.ContinueScene();
        lights.Bind();
      }
    }
    // [fxmirror] the effects' mirror image for the water (fx_mirror.h): what is drawn after the water, drawn mirrored about
    // its level into a picture of its own first, in the order the scene draws them (the shields' bubbles, beams and
    // trails, the volume blasts, the particles); only while there is something of them, and the eye is above the water
    water.fxMirror = {};
    if (waterFxMirror && look == Look::Modern && map.water.present && !sidePicture && view.eye.y > map.water.elevation + 0.05f) {
      const bool bands = hide.find("bands") == std::string::npos && ribbons.Any();
      const bool sparks = hide.find("particles") == std::string::npos && particles.Gathered() > 0;
      const bool bubbles = energy.Any();
      if (bands || sparks || volumesDrawn_ || bubbles) {
        Stage("effects mirrored in the water");
        const View mirrored = fxMirror.Begin(view, map.water.elevation, post.Depth(), camera.zNear);
        lights.Bind();
        if (bubbles) energy.DrawMirror(mirrored, fxMirror);
        if (bands) ribbons.DrawMirror(mirrored, fxMirror);
        if (volumesDrawn_) volumes.DrawMirror(mirrored, fxMirror);
        if (sparks) {
          if (litSmokeNow_) {  // (lit smoke reads the lights, the shadow maps, the clouds' and the smoke's shadows: as its own stage)
            lights.Bind();
            device_->SetTexture(kShadowMapSlot, shadows.Map());
            device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
            device_->SetTexture(kCloudShadowSlot, cloudShadowsNow);
            device_->SetTexture(kSmokeShadowSlot, smokeNow_ ? smokeMap.Map() : gfx::Texture{});
          }
          particles.DrawMirror(mirrored, fxMirror);
        }
        fxMirror.End();
        post.ContinueScene();
        lights.Bind();
        water.fxMirror = fxMirror.Picture();
      }
    }
    Stage("water");
    // the water: the modern look's sees through to a copy of the picture as it is now
    if (look == Look::Modern && map.water.present && post.CopyBehind()) {
      post.BeginColorOnly();
      lights.Bind();
      device_->SetTexture(kShadowMapSlot, shadows.Map());
      device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
      water.SetGround(terrain.HeightTexture(), terrain.HeightUnit());  // [floatfoam] (no foam slab under what floats)
      water.DrawModern(view, camera, post.Behind(), post.Depth());
      // [hulls] (what the waves leave on the ships' hulls above the water: hull_wash.h; only with the lifted ocean and its hulls)
      hullWash.Draw(view, camera, post.Depth(), water.ocean, terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width),
                    static_cast<float>(map.height), map.water.elevation);
      post.ContinueScene();
      lights.Bind();
    } else {
      water.Draw(view);
    }
    if (streamNow_) {  // [stream mouth] the streams' white water, after the sea: it lies on the sea where a stream runs into it
      Stage("stream white water");
      post.BeginColorOnly();
      lights.Bind();
      device_->SetTexture(kShadowMapSlot, shadows.Map());
      device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
      streams.Draw(view, camera, gfx::Texture{}, post.Depth(), terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width),
                   static_cast<float>(map.height), [this](uint32_t key) { return key < props_.size() && props_[key].model != nullptr; }, true);
      post.ContinueScene();
      lights.Bind();
    }
    // the campaign's objective areas over the water: on its surface (Decals::AddWater)
    if (decals.AnyWater() && map.water.present && hide.find("decals") == std::string::npos) {
      post.BeginColorOnly();
      lights.Bind();
      decals.ride = water.ocean.Lifted() ? &water.ocean : nullptr;  // [whitecaps] (on the lifted waves)
      decals.Draw(view, camera, post.Depth(), clock_, true);
      post.ContinueScene();
      lights.Bind();
    }
    if (ranges.Any() && map.water.present) {
      post.BeginColorOnly();
      ranges.Lay(view, camera, post.Depth(), true);
      post.ContinueScene();
      lights.Bind();
    }
    if (horizonNow_) {  // [horizon] on the far sea: the floes and the clouds' shadows, then the fog banks and the showers
      Stage("horizon: on the water");
      horizon.ride = water.ocean.Lifted() ? &water.ocean : nullptr;  // [whitecaps] (the floes on the lifted waves)
      horizon.DrawOnWater(view);
      Stage("horizon: in the air");
      post.BeginColorOnly();
      horizon.DrawInAir(view, post.Depth(), terrain.HeightTexture(), terrain.HeightUnit());
      post.ContinueScene();
      lights.Bind();
    }
    if (sunlight.ShaftsNow() && air.Volume()) {  // [sunlight] the light shafts in the low band over the ground (sunlight.h)
      Stage("light shafts");
      sunlight.March(view, camera, post.Depth(), terrain.HeightTexture(), shadows.Map(), air.Volume(), cloudShadowsNow,
                     smokeNow_ ? smokeMap.Map() : gfx::Texture{}, air.DustNow(), groundMiddle_, air.thinsOver, air.sunInAir, air.sunForward);
      Stage("light shafts: laid");
      post.BeginColorOnly();
      sunlight.Lay(view, post.Depth());
      post.ContinueScene();
      lights.Bind();
    }
  } else {
    draw.Flush(view);
    skyDraw.starry = starryNight ? 1.0f : 0.0f;  // [starry]
    skyDraw.Draw(view, camera, skyLight, haveMap_ ? sky_ : gfx::Texture{});
  }
  // motion: from the depth of everything solid and the water, and the models that moved of themselves
  if (meshes.motion) {
    Stage("motion");
    motion.Build(view, post.Depth(), motionView == 4 ? nullptr : &meshes);  // (4: for checking, the camera's part alone)
    if (grassNow_ && plantMotion && motionView != 4)  // [leftover] the ground cover's own motion, over it
      grass.DrawMotion(view, motion.Texture(), post.Depth(), motion.Now(), motion.Before(), motion.TimeBefore());
    if (leavesNow_ && motionView != 4)  // [groundfix] the falling leaves' own motion, over it
      leaves.DrawMotion(view, motion.Texture(), post.Depth(), motion.Now(), motion.Before(), terrain.HeightTexture(), terrain.HeightUnit());
    // (the solid scene and the water, for the upscaler's reactive mask: only where something reads it, the upscaler or
    // the mask's own view; the motion alone, for the blur or the generated frames, copied the whole scene for nothing)
    if (temporalNow_ || motionView == 5) post.KeepOpaque();
    post.ContinueScene();
    lights.Bind();
  }
  // what is see-through (shields' bubbles, frames of light): over everything solid and the water
  if (energy.Any()) {  // [energy shields] the bubbles of energy first (energy_shields.h): they bend what is behind them
    Stage("energy shields");
    energy.SetGround(haveMap_ ? terrain.HeightTexture() : gfx::Texture{}, static_cast<float>(map.width), static_cast<float>(map.height),
                     terrain.HeightUnit());  // [clearshell] (what stands off it is not bent)
    energy.Draw(view, &post);
    lights.Bind();
  }
  Stage("see-through (shields)");
  // [stealthlook] a cloaked unit's body bends the scene as it stands now
  if (cloakedNow_ && post.CopyBehind()) {
    post.ContinueScene();
    lights.Bind();
    meshes.cloakBehind = post.Behind();
  }
  meshes.Draw(view, environment_, false, true);
  meshes.cloakBehind = {};
  if (!placed.empty() || placedDraw.AnyTaken())  // [pings] (and the CommandFeedback models taken: placed.h)
    placedDraw.Draw(view, placed, lights.night, PlacedMeshes::DepthPull(camera.zNear, placing.distance));
  Stage("wakes, beams, trails");
  // beams and trails, then particles: over everything solid and the water, by the scene's depth
  wakes.ride = water.ocean.Lifted() ? &water.ocean : nullptr;  // [lift] (the wakes ride the lifted waves)
  wakes.churn = water.ocean.churn;  // [foamloop] (with the foam patches)
  wakes.churnShare = water.ocean.patches ? water.ocean.foamPatches : 0.0f;
  wakes.Draw(view);
  ribbons.pixelScale = photoPixel;  // (photofix: a photo's frame as the window shows it)
  if (hide.find("bands") == std::string::npos) ribbons.Draw(view);
  if (wildlifeNow_) {  // the fireflies, with what glows
    Stage("wildlife: fireflies");
    wildlifeDraw.DrawFireflies(view);
  }
  if (buildNow_ && build.Busy() && !extraView) {  // ([views] once a frame) (buildfx: the build sparks moved on, bouncing off the scene's depth: it dispatches)
    Stage("build sparks");
    build.Simulate(view, camera, post.Depth(), terrain.HeightTexture(), terrain.HeightUnit(), static_cast<float>(map.width),
                   static_cast<float>(map.height), haveMap_ && map.water.present ? map.water.elevation : -1e9f);
  }
  {
    // lamps' shafts and particles go into the scene's colour alone: they test the scene's depth
    // themselves and fade where they meet something solid
    post.BeginColorOnly();
    if (volumesDrawn_) {  // (volume explosions) the volumes, before the particles: a blast's sparks and smoke over them
      Stage("volume blasts");
      volumes.Draw(view, post.Depth(), camera.zNear);
    }
    if (buildNow_ && hide.find("particles") == std::string::npos) build.Draw(view, post.Depth(), camera);  // (buildfx: the sparks)
    Stage("lamp shafts");
    shafts.Draw(view, post.Depth(), camera.zNear);
    if (exhaustsNow_ && hide.find("bands") == std::string::npos) {  // (unitsfx: the real exhausts' plumes)
      Stage("exhausts");
      exhausts.Draw(view, post.Depth(), camera.zNear);
    }
    if (blastsNow_ && blastDust) {  // the blasts' dust, under the game's own fire and smoke
      Stage("blasts' dust");
      blasts.DrawDust(view, post.Depth(), camera.zNear, terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours(),
                      static_cast<float>(map.width), static_cast<float>(map.height));
    }
    if (dustNow_ && dust.Above()) {  // movement dust's spray and contrails (movement_dust.h), over the water and the sky
      Stage("movement dust: spray, contrails");
      if (litSmokeNow_) lights.Bind();
      dust.DrawAbove(view, post.Depth(), camera.zNear, terrain.HeightTexture(), terrain.HeightUnit(), terrain.GroundColours(),
                     static_cast<float>(map.width), static_cast<float>(map.height), litSmokeNow_);
    }
    if (surfNow_ && water.surf.AnySpray()) {  // [surf] the spray off the rocks and the ships' bows (surf.h), over the water
      Stage("surf: spray");
      if (litSmokeNow_) lights.Bind();
      water.surf.DrawSpray(view, post.Depth(), camera.zNear, litSmokeNow_);
    }
    Stage("particles");
    if (hide.find("particles") == std::string::npos) {
      if (litSmokeNow_) {  // (lit smoke reads the lights, the shadow maps, the clouds' and the smoke's shadows)
        lights.Bind();
        device_->SetTexture(kShadowMapSlot, shadows.Map());
        device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
        device_->SetTexture(kCloudShadowSlot, cloudShadowsNow);  // [sunlight]
        device_->SetTexture(kSmokeShadowSlot, smokeNow_ ? smokeMap.Map() : gfx::Texture{});
      }
      particles.Draw(view, post.Depth(), camera.zNear);
      // those that bend what is behind them (the shimmer over heat): with the picture as it is now
      if (particles.Bending() && post.CopyBehind()) {
        post.BeginColorOnly();
        particles.DrawBending(view, post.Behind(), post.Depth(), camera.zNear);
      }
      // heat shimmer: the air over what is hot, bending the picture as it is now
      if (particles.heat && particles.PrepareHeat(view)) {
        Stage("heat shimmer");
        if (post.CopyBehind()) {
          post.BeginColorOnly();
          particles.DrawHeat(view, post.Behind(), post.Depth(), camera.zNear);
        }
      }
    }
    if (exhaustsNow_ && exhausts.PrepareHaze(view)) {  // (unitsfx: the heat haze behind the drives, bending the picture as it is now)
      Stage("exhausts' haze");
      if (post.CopyBehind()) {
        post.BeginColorOnly();
        exhausts.DrawHaze(view, post.Behind(), post.Depth(), camera.zNear);
      }
    }
    post.ContinueScene();
    lights.Bind();
  }
  Stage("rain, snow");
  // (cinecam) what falls is drawn in a box round what the game's camera looks at, as large as that is far off; a
  // cinematic camera's target may lie far ahead or in the air: its box is round the eye instead, a little ahead of
  // it, as large as the eye is high over the ground
  // (photomode: a photo's tile has the box of the whole photo, measured in its own pixels)
  const Camera fall = [&] {
    Camera c = placing;
    c.fovY = camera.fovY;
    if (!placing.cinematic) return c;
    const Vec3 eye = placing.Eye();
    const float ground = haveMap_ ? std::min(GroundHeight(eye.x, eye.z), eye.y) : eye.y;
    c.distance = std::clamp((eye.y - ground) * 1.5f + 20.0f, 25.0f, 560.0f);
    c.target = {eye.x - std::sin(placing.heading) * c.distance * 0.3f, ground, eye.z - std::cos(placing.heading) * c.distance * 0.3f};
    return c;
  }();
  falling.pixelScale = photoPixel;  // (photofix: in a photo's frame as the window shows it; else 1, 1)
  falling.underClouds = stormSunNow_;  // [rainunder] a storm's rain from under its cover (its pockets of sun dry)
  falling.pictureShare = photoShare;
  // [fallsize] (under an upscaler the widths in the window's pixels) [fallpace]
  falling.windowShare = fallWindowSize && screenHeight_ > view.height ? static_cast<float>(view.height) / static_cast<float>(screenHeight_) : 1.0f;
  falling.steadyPace = fallSteadyPace;
  falling.highFade = fallFadeHigh;  // [fallhigh]
  if (view.constants.skyZenith[3] > 0.5f) falling.Draw(view, fall, rain, snow, litRain && look == Look::Modern);
  if (rainSplashes && look == Look::Modern && view.constants.skyZenith[3] > 0.5f && rain > 0) {  // (reading the depth)
    post.BeginColorOnly();
    lights.Bind();
    falling.DrawSplashes(view, fall, rain, post.Depth());
  }
  if (clouds.Zoomed()) {  // (cloudzoom) the clouds the eye has risen into or above, over all that lies under them (cloud_zoom.h)
    Stage("clouds over the scene (cloud zoom)");
    post.BeginColorOnly();
    cloudZoomDraw.Lay(view, clouds.Picture(), post.Depth());
  }
  Stage("fog of war");
  if (haveMap_ && sight.On() && sightShown) {  // the fog of war: what is not seen, darkened
    post.BeginColorOnly();
    sight.neverSeen = unexplored ? 0.04f : sight.darkness;
    sight.waterLevel = map.water.present ? map.water.elevation : -1e9f;
    sight.Draw(view, camera, post.Depth(), static_cast<float>(map.width), static_cast<float>(map.height));
    // (the same over the picture the temporal upscaler's reactive mask compares with: the fog of war stands still)
    if (temporalNow_ && post.BeginKept()) sight.Draw(view, camera, post.Depth(), static_cast<float>(map.width), static_cast<float>(map.height));
  }
  if (haveMap_ && playable[2] > playable[0]) {  // what lies beyond the part of the map that can be played now
    post.BeginColorOnly();
    sight.DrawBoundary(view, camera, post.Depth(), playable, static_cast<float>(map.width), static_cast<float>(map.height), look == Look::Modern && worldBeyond);
    if (temporalNow_ && post.BeginKept())
      sight.DrawBoundary(view, camera, post.Depth(), playable, static_cast<float>(map.width), static_cast<float>(map.height), look == Look::Modern && worldBeyond);
  }
  if (outlinesNow_ && outlines.marked) {  // units seen behind what hides them (hidden_outlines.h): what of them is hidden
    Stage("hidden outlines' masks");
    outlines.SetGround(haveMap_ ? terrain.HeightTexture() : gfx::Texture{}, static_cast<float>(map.width), static_cast<float>(map.height), terrain.HeightUnit());
    outlines.Masks(&meshes, screenView_, post.Depth(), view.width, view.height, camera.zNear, temporalNow_);
  }
  Stage("finishing (exposure, bloom)");
  post.shown = !meshes.motion ? gfx::Texture{} : motionView == 5 ? post.Reactive([&] { falling.DrawReactive(view); }) : motion.Check(motionView, view, post.Color());
  if (rayView && rays.Available() && !post.shown) {  // the ray tracing scene's check view (RaysReady above)
    Stage("ray tracing check view");
    lights.Bind();
    post.shown = rays.Check(view, terrain.NormalMap(), terrain.GroundColours());
  }
  if (temporalNow_ && !post.shown) {  // FSR 3: up to the screen's size from this frame and the ones before
    Stage("temporal upscale (FSR 3)");
    Fsr3::Inputs in;
    in.motion = motion.Texture();
    if (fallNoLocks && falling.WantsComposition())  // [falltrail] (while it snows; before Reactive, which ends the fall's masks)
      in.composition = post.Composition([&] { falling.DrawComposition(view); });
    in.reactive = post.Reactive([&] { falling.DrawReactive(view); });  // ([rainreactive])
    in.jitterX = motion.jitterX;
    in.jitterY = motion.jitterY;
    in.exposure = view.constants.look > 0.5f ? view.constants.modern[2] : 1.0f;
    in.cameraNear = camera.zNear;
    in.fovY = camera.fovY;
    in.frameSeconds = frameSeconds_;
    in.reset = temporalCut_;
    in.sharpness = upscaler.sharpness;
    post.UpscaleTemporal(&fsr3, in, screenWidth_, screenHeight_);
    Stage("finishing at the screen's size (exposure, bloom)");
  }
  // (cinecam) the lens's depth of field, on the finished light, before the glow and the tone curve (Post::beforeFinish)
  if (lens.on && lens.strength > 0.001f && !sidePicture && !extraView && !dof.Failed()) {  // ([views] the first view's alone)
    dof.waterLevel = haveMap_ && map.water.present ? map.water.elevation : -1e9f;
    post.beforeFinish = [this](gfx::Texture picture, int width, int height, gfx::Texture depth, int depthWidth, int depthHeight) {
      return dof.Apply(lens, camera, picture, width, height, depth, depthWidth, depthHeight, frameSeconds_, lensSnap_);
    };
  } else if (post.beforeFinish) {
    post.beforeFinish = nullptr;
  }
  // (photolook) the photographic look: its eye moved on only in the game's own frames (not in a photo or a picture
  // drawn between them), set at once after a cut or when the look comes on; no lens touches on a picture drawn
  // between (the minimap's ground), nor on a tile of a photo drawn in tiles (photoTiled: a vignette in each tile's
  // corners would be a pattern over the whole photo)
  const bool photoLookBefore = post.photoFinish != nullptr;
  post.photoFinish = nullptr;
  if (photoLook.on && look == Look::Modern && !photoFinish.Failed()) {
    // (photofix: a photo whose moment the window had not settled into yet meters its own frames, the eye set at once)
    photoFinish.Set(photoLook, photoMeter ? 1 / 60.0f : sidePicture || photo || extraView ? 0.0f : frameSeconds_,  // ([views] the first view's eye)
                    photoMeter || (lensSnap_ && !photo) || !photoLookBefore, !sidePicture && !photoTiled);
    post.photoFinish = &photoFinish;
  }
  post.Present(view, &upscaler, screenWidth_, screenHeight_);
  if (framegenNow_ && !sidePicture && !photo) {  // frame generation: the finished picture kept before the icons and the interface, and the rest
    Stage("frame generation: the finished picture kept");
    framegenIn_ = FrameGeneration::Inputs{};
    framegenIn_.hudless = post.KeepFinished(device_->WindowPicture(), device_->Width(), device_->Height());
    framegenIn_.depth = post.Depth();
    framegenIn_.motion = motion.Texture();
    framegenIn_.renderWidth = view.width;
    framegenIn_.renderHeight = view.height;
    framegenIn_.jitterX = temporalNow_ ? motion.jitterX : 0;
    framegenIn_.jitterY = temporalNow_ ? motion.jitterY : 0;
    framegenIn_.cameraNear = camera.zNear;
    framegenIn_.fovY = camera.fovY;
    framegenIn_.frameSeconds = frameSeconds_;
    framegenIn_.reset = framegenCut_;
    framegenReady_ = framegenIn_.hudless && framegenIn_.depth && framegenIn_.motion;
  }
  if (outlines.drawn && !post.shown) {  // the hidden units' outlines, on the finished picture (after frame generation's kept one)
    Stage("hidden outlines");
    outlines.Draw();
  }
  if ((!sidePicture || extraView) && !photo && (!orders.legs.empty() || !orders.marks.empty())) {  // ([views] in every view)  // the order lines, over the world, under the icons
    Stage("order lines");
    orders.Draw(screenView_, clock_ * 10);
  }
  Stage("icons, bars");
  // (cinecam: no strategic symbols for a cinematic camera: every unit far off toward the horizon would stand there as one)
  icons.showSymbols = iconsOn && !camera.cinematic;
  if (!photo || warmAll) icons.Draw();  // on the finished picture, as the original draws them (and the life bars; cinecam: none on a photo; [session] warmAll)
  TakeMomentOut();  // (photomode: between frames the game's own hour and weather again)
  CpuTimes::Part(nullptr);
}

// At the frame's end (the device calls it, gfx.h): the picture between the frame before and this one, from what
// Render kept and the window's finished picture with the icons and the interface on it. False: the frame is shown
// alone (no world drawn this frame, or a cut).
bool World::MakeBetween() {
  if (!framegenReady_) return false;
  framegenReady_ = false;
  const gfx::Texture between = device_->BetweenPicture();
  if (!between) return false;
  FrameGeneration::Inputs in = framegenIn_;
  in.present = device_->WindowPicture();
  device_->Mark("frame generation");
  framegen.Run(in, between, device_->Width(), device_->Height());
  return !in.reset;
}

bool World::RaysReady() {
  if (!raysTried_) {  // (its shaders, the first time: where the device has no ray tracing, never again)
    raysTried_ = true;
    std::string why;
    if (!rays.Init(&why)) fprintf(stderr, "render: ray tracing cannot be used (%s)\n", why.c_str());
  }
  if (rays.Available() && !raysGround_ && haveMap_) {
    rays.SetGround(map);
    raysGround_ = true;
  }
  return rays.Available();
}

bool World::Project(Vec3 p, float* x, float* y) const {
  const Mat4& m = screenView_.constants.viewProj;  // (the scene's, without the temporal upscaler's jitter)
  const float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
  if (w <= 1e-4f) return false;
  *x = ((m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3]) / w * 0.5f + 0.5f) * static_cast<float>(screenWidth_);
  *y = (0.5f - (m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3]) / w * 0.5f) * static_cast<float>(screenHeight_);
  return true;
}

bool World::GroundAt(float x, float y, Vec3* hit) const {
  if (screenHeight_ <= 0) return false;
  const Ray ray = camera.PixelRay(x, y, static_cast<float>(screenWidth_), static_cast<float>(screenHeight_));
  if (haveMap_ && HitGround(map, ray, true, hit)) return true;
  return HitPlaneY(ray, camera.target.y, hit);
}

const World::Selection& World::SelectionOf(uint32_t blueprint, const std::string& id) {
  if (blueprint >= selections_.size()) selections_.resize(static_cast<size_t>(blueprint) + 1);
  Selection& s = selections_[blueprint];
  if (!s.known) {
    s.known = true;
    BpValue bp;
    if (!id.empty() && dust.readBlueprint && dust.readBlueprint(id, &bp)) {
      const float top = static_cast<float>(bp["SelectionMeshUseTopAmount"].NumberOr(1));
      s.top = top > 0 && top < 1 ? top : 1.0f;
      s.scaleX = std::max(0.05f, static_cast<float>(bp["SelectionMeshScaleX"].NumberOr(1)));
      s.scaleZ = std::max(0.05f, static_cast<float>(bp["SelectionMeshScaleZ"].NumberOr(1)));
    }
  }
  return s;
}

void World::UnitsAt(float x, float y, float tolerance, std::vector<uint32_t>* out) const {
  out->clear();
  if (screenHeight_ <= 0) return;
  std::vector<std::pair<float, uint32_t>> hits;
  for (const Pickable& p : pickables_) {
    bool hit = false;
    float sx, sy;
    const bool shown = Project(p.at, &sx, &sy);
    if (p.boxed) {  // its selection box's corners on the screen: their bounds within the tolerance of the point
      float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
      int seen = 0;
      for (int k = 0; k < 8; ++k) {
        const Vec3 c{(k & 1) ? p.hi.x : p.lo.x, (k & 2) ? p.hi.y : p.lo.y, (k & 4) ? p.hi.z : p.lo.z};
        float cx, cy;
        if (!Project(p.at + Rotate(p.turn, c), &cx, &cy)) continue;
        ++seen;
        x0 = std::min(x0, cx), y0 = std::min(y0, cy), x1 = std::max(x1, cx), y1 = std::max(y1, cy);
      }
      hit = seen == 8 && x >= x0 - tolerance && x <= x1 + tolerance && y >= y0 - tolerance && y <= y1 + tolerance;
    }
    // far off, a unit is its strategic icon: its middle within about ten pixels (and the tolerance)
    float mx, my;
    if (!hit && Project(p.centre, &mx, &my)) {
      const float reach = 10 + tolerance;
      hit = (mx - x) * (mx - x) + (my - y) * (my - y) <= reach * reach;
      if (!shown) sx = mx, sy = my;
    }
    if (!hit) continue;
    if (!shown && !Project(p.centre, &sx, &sy)) continue;
    hits.push_back({(sx - x) * (sx - x) + (sy - y) * (sy - y), p.id});
  }
  std::sort(hits.begin(), hits.end());
  for (const auto& h : hits)
    if (std::find(out->begin(), out->end(), h.second) == out->end()) out->push_back(h.second);
}

uint32_t World::UnitAt(float x, float y) const {
  if (screenHeight_ <= 0) return 0xffffffffu;
  // [pick] (ui request 43) the original's way: the selection boxes, the nearest to the point on the screen; the sphere test
  // below only where no unit's box is near (a unit without a box: one whose files cannot be read)
  {
    std::vector<uint32_t> hits;
    UnitsAt(x, y, 10, &hits);
    if (!hits.empty()) return hits.front();
  }
  const Ray ray = camera.PixelRay(x, y, static_cast<float>(screenWidth_), static_cast<float>(screenHeight_));
  // a far-away unit is its strategic icon: it can be hit within about ten pixels of its middle
  const float worldPerPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(screenHeight_);
  uint32_t best = 0xffffffffu;
  float bestAlong = 1e30f;
  for (const Pickable& p : pickables_) {
    if (p.boxed) continue;  // ([pick] those with a box are UnitsAt's alone)
    const Vec3 to = p.centre - ray.origin;
    const float along = Dot(to, ray.direction);
    if (along <= 0) continue;
    const float radius = std::max(p.radius * 0.8f, along * worldPerPixel * 10);
    const Vec3 off = to - ray.direction * along;
    if (Dot(off, off) > radius * radius) continue;
    // the nearest to the camera, measured where the ray enters the unit's sphere
    const float enters = along - std::sqrt(radius * radius - Dot(off, off));
    if (enters < bestAlong) {
      bestAlong = enters;
      best = p.id;
    }
  }
  return best;
}

void World::UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const {
  out->clear();
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  for (const Pickable& p : pickables_) {
    float x, y;
    if (Project(p.centre, &x, &y) && x >= x0 && x <= x1 && y >= y0 && y <= y1) out->push_back(p.id);
  }
}

}  // namespace render
