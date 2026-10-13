#include "world.h"

#include "dds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/vfs.h"
#include "ground.h"

namespace render {

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
      skyDraw(device),
      falling(device),
      sight(device),
      shafts(device),
      wakes(device),
      ranges(device),
      placedDraw(device, &models),
      shadows(device),
      device_(device),
      vfs_(vfs) {}

bool World::Open(const std::string& mapPath, const Options& options, std::string* error) {
  if (!draw.Init(error) || !meshes.Init(error) || !particles.Init(error) || !ribbons.Init(error) || !decals.Init(error) || !icons.Init(error) ||
      !ao.Init(error) || !post.Init(error) || !skyDraw.Init(error) || !falling.Init(error) || !sight.Init(error) || !shafts.Init(error) || !wakes.Init(error) || !ranges.Init(error) || !placedDraw.Init(error)) {
    *error = "shader: " + *error;
    return false;
  }
  effects.verbose = options.verbose;
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
    if (!terrain.Load(&map, &why)) {
      *error = "terrain: " + why;
      return false;
    }
    if (options.lodPixels > 0) terrain.quadPixels = options.lodPixels;
    if (!water.Load(&map, &why)) {
      *error = "water: " + why;
      return false;
    }
    haveMap_ = true;
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
    // the map's own decals: albedo ones (pictures over the ground's colour) and normal ones (its sculpting)
    if (options.mapDecals != "off") {
      int laid = 0;
      for (const MapDecal& d : map.decals) {
        if (d.type != 1 && d.type != 2) continue;
        if (!options.decalFilter.empty() && d.textures[0].find(options.decalFilter) == std::string::npos) continue;
        if ((options.mapDecals == "albedo" && d.type != 1) || (options.mapDecals == "normals" && d.type != 2)) continue;  // one kind alone
        const float heading = d.rotation.y * options.decalTurn;
        Vec3 centre = d.position;
        if (options.mapDecals == "corner") {  // the position as the corner the decal is turned about
          const float c = std::cos(heading), s = std::sin(heading), hx = d.scale.x * 0.5f, hz = d.scale.z * 0.5f;
          centre.x += hx * c + hz * s;
          centre.z += -hx * s + hz * c;
        }
        centre.y = map.Height(Clamp(centre.x, 0, static_cast<float>(map.width)), Clamp(centre.z, 0, static_cast<float>(map.height)));
        const bool ok = d.type == 2 ? decals.AddNormal(d.textures[0], centre, d.scale.x, d.scale.z, heading, d.cutoffLod * options.decalRange)
                                    : decals.Add(d.textures[0], centre, d.scale.x, d.scale.z, heading, d.cutoffLod * options.decalRange);
        if (ok) ++laid;
      }
      printf("render: %d of the map's %zu decals laid\n", laid, map.decals.size());
    }
    for (const auto& cube : map.environmentCubemaps)
      if (cube.first == "<default>") environment_ = textures.Get(cube.second);
    // the map's props (trees, rocks, wrecks): where each stands does not change
    for (const MapProp& p : map.props) {
      const Model* model = models.Blueprint(p.blueprint);
      if (!model->ok) {  // (kept in the list all the same: the simulation names a prop by its place in it)
        props_.push_back({nullptr, Mat4::Identity()});
        continue;
      }
      const Vec3 s = p.scale * model->scale;
      Mat4 world = {{{p.axisX.x * s.x, p.axisY.x * s.y, p.axisZ.x * s.z, p.position.x},
                     {p.axisX.y * s.x, p.axisY.y * s.y, p.axisZ.y * s.z, p.position.y},
                     {p.axisX.z * s.x, p.axisY.z * s.y, p.axisZ.z * s.z, p.position.z},
                     {0, 0, 0, 1}}};
      props_.push_back({model, world});
    }
    // ... listed by squares of the map
    propCellsX_ = std::max(1, static_cast<int>(std::ceil(static_cast<float>(map.width) / kPropCell)));
    propCellsZ_ = std::max(1, static_cast<int>(std::ceil(static_cast<float>(map.height) / kPropCell)));
    propCells_.assign(static_cast<size_t>(propCellsX_) * static_cast<size_t>(propCellsZ_), {});
    for (size_t i = 0; i < props_.size(); ++i) ListProp(&props_[i], static_cast<uint32_t>(i), false);
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
  return true;
}

void World::SetLooks(const std::vector<BlueprintLook>& looks) {
  int units = 0;
  for (const BlueprintLook& look : looks) {
    models.SetLook(look);
    if (look.kind != 0) continue;
    icons.Define(models.UnitBlueprintPath(look.id), look.icon, look.iconPriority, look.iconFadeIn, look.selectionX, look.selectionZ, look.lifeBarSize,
                 look.lifeBarHeight, look.lifeBarOffset);
    ++units;
  }
  if (!looks.empty()) printf("render: %zu blueprints' looks from the simulation (%d of them units)\n", looks.size(), units);
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

// Four corner pieces about a unit's box on the ground. A piece is as large as nine tenths of the
// box's shorter half side, and never so small that its line is thinner than three pixels (the
// game's ren_SelectBracketMinPixelSize; its line is about a ninth of a piece).
void World::AddBrackets(int kind, Vec3 position, const Quat& orientation, float halfX, float halfZ) {
  const Vec3 eye = camera.target + (camera.PixelRay(view.width * 0.5f, view.height * 0.5f, static_cast<float>(view.width), static_cast<float>(view.height)).direction * -camera.distance);
  const float perPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1)) * Length(position - eye);
  const float piece = std::max(std::min(halfX, halfZ) * 0.9f, 27 * perPixel);
  halfX = std::max(halfX, piece);
  halfZ = std::max(halfZ, piece);
  const Vec3 forward = Rotate(orientation, {0, 0, 1});
  const float heading = std::atan2(forward.x, forward.z);
  for (int c = 0; c < 4; ++c) {
    const float sx = (c & 1) ? 1.0f : -1.0f, sz = (c >> 1) ? 1.0f : -1.0f;  // (a mark's picture has its top at the unit's back)
    const Vec3 at = position + Rotate(orientation, {sx * (halfX - piece * 0.5f), 0, sz * (halfZ - piece * 0.5f)});
    decals.AddMark(brackets_[kind][c], at, piece, piece, heading);
  }
}

void World::AddLamps(const Model* model, Vec3 position, const Quat& orientation, const Pose* pose, const Vec3* armyColor) {
  if (lampsOn_ <= 0 || !model || model->lights.empty()) return;
  // (only near what the camera looks at: a lamp lights a few units of ground)
  if (Length(position - camera.target) > camera.distance * 2 + 80) return;
  // with many units lit, each lights only the first of its lamps (a list has the lamps that matter
  // most first): about 6,000 lights in all, by the number of units of the frame before
  ++lampUnits_;
  const size_t most = static_cast<size_t>(std::clamp(6000 / std::max(lampUnitsBefore_, 1), 3, 16));
  size_t lit = 0;
  uint64_t taken[16];
  bool takenBeam[16];
  size_t takenCount = 0;
  for (const ModelLight& light : model->lights) {
    // (a lamp on a part the script has hidden is not there: a commander's upgrade not bought yet)
    if (light.bone >= 0 && pose) {
      bool hidden = false;
      for (size_t i = 0; pose->now && i < pose->nowCount; ++i)
        if (pose->now[i].bone == static_cast<uint32_t>(light.bone)) {
          hidden = pose->now[i].hidden;
          break;
        }
      if (hidden) continue;
    }
    if (lit++ >= most) break;
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
    {
      const uint64_t square = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(at.x / 2.5f)))) << 32) |
                              static_cast<uint32_t>(static_cast<int32_t>(std::floor(at.z / 2.5f)));
      // (by the lamps of the units before this one: a unit's own lamps do not cut each other)
      const auto found = lampSquares_.find(square);
      const uint32_t count = found == lampSquares_.end() ? 0 : found->second;
      const uint32_t beams = count & 0xffffu, others = count >> 16;
      if (light.cone > 0 ? beams >= 3 : others >= 2) continue;
      if (takenCount < 16) {
        taken[takenCount] = square;
        takenBeam[takenCount++] = light.cone > 0;
      }
    }
    float on = lampsOn_ * light.strength * 0.5f;  // (1 in the list: a strong lamp to an eye opened for the night)
    if (light.blink > 0) {  // (each lamp on its own beat, by where it is)
      const float beat = time_ / light.blink + position.x * 0.37f + position.z * 0.53f;
      if (beat - std::floor(beat) > 0.5f) on *= 0.12f;
    }
    // the unit's own glow: in its army's colour (as light, its strongest part 1), so that one sees
    // at night whose it is
    Vec3 tint = light.color;
    if (light.hull && armyColor) {
      tint = {std::pow(std::max(armyColor->x, 0.0f), 2.2f), std::pow(std::max(armyColor->y, 0.0f), 2.2f), std::pow(std::max(armyColor->z, 0.0f), 2.2f)};
      // (a fifth weaker than at first: Chris, 2026-10-13, "the red glow which is the army color is a bit strong")
      tint = tint * (0.8f / std::max({tint.x, tint.y, tint.z, 0.05f}));
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
      lights.AddBeam(at, light.radius * model->scale, tint * on, along, light.cone, light.glow, &middle);
      // the beam in the air: faint in clear night air, plainer in rain, snow and fog
      const float air = 0.03f + 0.16f * fog + 0.08f * rain + 0.09f * snow;
      const float reach = light.radius * model->scale * 0.75f;
      const float wide = std::tan(std::min(light.cone, 120.0f) * 0.5f * 0.45f * 3.14159265f / 180) * reach;  // (its bright middle only)
      shafts.Add(at, at + along * reach, 0.03f * model->scale, wide, tint * (on * air));
    } else {
      lights.Add(at, light.radius * model->scale, tint * on);
    }
  }
  for (size_t i = 0; i < takenCount; ++i) lampSquares_[taken[i]] += takenBeam[i] ? 1u : 0x10000u;
}

float World::GroundHeight(float x, float z) const {
  if (!haveMap_) return 0;
  const float y = map.Height(x, z);
  return map.water.present ? std::max(y, map.water.elevation) : y;
}

void World::Begin(const Camera& cam, int width, int height, float time, double clock) {
  camera = cam;
  time_ = time;
  clock_ = clock;
  view = View::Make(camera, width, height, time);
  // the light: the map's sun, in the chosen look
  ViewConstants& c = view.constants;
  const Vec3 sunDirection = haveMap_ ? Normalize(map.lighting.sunDirection) : Normalize(Vec3{0.4f, 0.8f, 0.45f});
  auto set = [](float* to, Vec3 v) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
  };
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
  wakes.Begin();
  lampUnitsBefore_ = lampUnits_;
  lampUnits_ = 0;
  if (moving_.size() > 60000) moving_.clear();  // (entity ids pile up over a long game)
  lampSquares_.clear();
  // a time of day: the computed sky's light in place of the map's (modern look)
  Vec3 lightDirection = sunDirection;
  if (timeOfDay >= 0 && look == Look::Modern) {
    // the weather: what it is by itself, its clouds, and what it has done to surfaces by now
    float flash = 0;
    uint32_t stroke = 0;
    if (weatherAuto) {
      const WeatherNow now = WeatherAt(weatherSeed, clock, climate, weatherSpell);
      cloudCover = now.clouds;
      rain = now.rain;
      snow = now.snow;
      fog = now.fog;
      flash = LightningAt(weatherSeed, clock, rain, &stroke);
    } else {
      flash = LightningAt(weatherSeed, clock, rain, &stroke);
    }
    const float dt = weatherTime_ < 0 ? 0 : std::clamp(time - weatherTime_, 0.0f, 0.5f);
    weatherTime_ = time;
    wet = std::clamp(wet + (rain > 0 ? dt / 15 : -dt / 120), 0.0f, 1.0f);
    // (snow builds up over two and a half minutes of snowfall and is gone in five; while it melts,
    // the ground it leaves is wet)
    snowLying = std::clamp(snowLying + (snow > 0 ? dt / 150 : -dt / 300), 0.0f, 1.0f);
    if (snow <= 0 && snowLying > 0) wet = std::max(wet, std::min(snowLying * 3, 1.0f));
    const float cover = std::max({cloudCover, rain > 0 ? 0.8f + 0.2f * rain : 0.0f, snow > 0 ? 0.75f + 0.2f * snow : 0.0f, fog * 0.6f});
    skyLight = SkyAt(timeOfDay, cover);
    if (flash > 0) {  // lightning: for a moment the clouds light everything (less to an eye opened for the night)
      const Vec3 bolt = Vec3{0.75f, 0.82f, 1.0f} * (flash * 1.1f / std::pow(skyLight.exposure, 0.6f));
      skyLight.zenith += bolt;
      skyLight.horizon += bolt * 0.6f;
    }
    lampsOn_ = std::clamp((0.30f - skyLight.daylight) / 0.18f, 0.0f, 1.0f);
    if (flash > 0.02f) {
      // the bolt itself: from the clouds to the ground somewhere round what the camera looks at (the
      // place and its zigzag follow from which stroke it is, so it stands still while it flashes),
      // and its light on the ground where it strikes
      const float reach = std::max(camera.distance * 0.4f, 25.0f);  // (near enough to be in the picture most times)
      Vec3 foot = camera.target + Vec3{(WeatherDraw(weatherSeed, stroke, 71) - 0.5f) * 2 * reach, 0, (WeatherDraw(weatherSeed, stroke, 73) - 0.5f) * 2 * reach};
      foot.y = haveMap_ ? GroundHeight(foot.x, foot.z) : camera.target.y;
      const Vec3 light = Vec3{0.8f, 0.86f, 1.0f} * (flash * 22 / skyLight.exposure);
      const float top = foot.y + 150, wide = std::max(0.3f, camera.distance * 0.009f);
      Vec3 from{foot.x + (WeatherDraw(weatherSeed, stroke, 79) - 0.5f) * 40, top, foot.z + (WeatherDraw(weatherSeed, stroke, 83) - 0.5f) * 40};
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
    c.weather[2] = 0.0035f * fog + 0.0007f * rain + 0.0010f * snow;
    c.weather[3] = 0.55f + 0.3f * fog;
    // the mist on low ground
    mistNow = mist;
    if (mistNow < 0) {
      auto rise = [](float from, float to, float h) {
        const float t = std::clamp((h - from) / (to - from), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
      };
      // by the hour: a third of it lies from nine in the evening, all of it from half past five
      // to seven, and the morning has burnt it off by half past nine
      const float h = timeOfDay;
      const float byHour = h < 12 ? (0.3f + 0.7f * rise(4, 5.5f, h)) * (1 - rise(7, 9.5f, h)) : 0.3f * rise(19, 21, h);
      mistNow = byHour * (climate == Climate::Dry ? 0.25f : 1.0f) + 0.5f * fog + (rain > 0 ? 0.0f : 0.4f * wet);
      mistNow = std::clamp(mistNow * (1 - 0.5f * rain), 0.0f, 1.0f) * (snow > 0 ? 0.0f : 1.0f);
    }
    if (haveMap_ && mistNow > 0) {
      // (seen straight down through all of it at its floor, in the middle of a bank, full mist hides a third)
      c.mist[0] = 0.23f * mistNow / mistDepth_;
      c.mist[1] = mistFloor_;
      c.mist[2] = 1 / mistDepth_;
      c.mist[3] = 0.6f;
    }
    SetSky(skyLight, &c);
    lightDirection = skyLight.direction;
  }
  if (haveMap_ && shadowsOn) shadows.Place(camera, lightDirection, &view);

  meshes.Begin();
  icons.Begin(view);
  lights.Begin(clock);
  pickables_.clear();
  // the scenery's changes that are due by now, then the scenery
  while (!propEvents_.empty() && static_cast<double>(propEvents_.front().tick) * 0.1 <= clock + 1e-6) {
    ApplyPropEvent(propEvents_.front());
    propEvents_.pop_front();
  }
  // (only the squares near what the camera sees: a square counts when it, widened by the largest
  // prop's reach and by room for a shadow thrown into the picture from outside it, is in view)
  propCellsSeen = propsAdded = 0;
  meshes.addingScenery = true;  // (trees and rocks: drawn before what is laid on the ground, Render)
  if (!propCells_.empty()) {
    const float margin = propReach_ + 30;
    for (int cz = 0; cz < propCellsZ_; ++cz)
      for (int cx = 0; cx < propCellsX_; ++cx) {
        const PropCell& cell = propCells_[static_cast<size_t>(cz) * static_cast<size_t>(propCellsX_) + static_cast<size_t>(cx)];
        if (cell.ofMap.empty() && cell.made.empty()) continue;
        const Vec3 lo{static_cast<float>(cx) * kPropCell - margin, ranges.low - 20, static_cast<float>(cz) * kPropCell - margin};
        const Vec3 hi{static_cast<float>(cx + 1) * kPropCell + margin, ranges.high + 60, static_cast<float>(cz + 1) * kPropCell + margin};
        if (!view.BoxVisible(lo, hi)) continue;
        ++propCellsSeen;
        for (uint32_t i : cell.ofMap) {
          meshes.Add(props_[i].model, props_[i].world, {1, 1, 1});
          ++propsAdded;
        }
        for (uint32_t id : cell.made) {
          auto made = madeProps_.find(id);
          if (made == madeProps_.end() || !made->second.model) continue;
          meshes.Add(made->second.model, made->second.world, {1, 1, 1});
          ++propsAdded;
        }
      }
  }
  meshes.addingScenery = false;
  if (haveMap_)
    for (const std::string& border : borders) {
      const bool whole = border.size() > 3 && border.compare(border.size() - 3, 3, ".bp") == 0;
      const Model* model = models.Blueprint(whole ? border : border + ".bp");
      if (!model->ok) continue;
      const Vec3 middle{static_cast<float>(map.width) * 0.5f, groundMiddle_, static_cast<float>(map.height) * 0.5f};
      meshes.Add(model, Mat4::FromTRS(middle, Quat{}, static_cast<float>(map.width) * model->scale), {1, 1, 1});
    }
  // the interface's ghosts, lines and rings
  for (const Ghost& g : ghosts) {
    const Model* model = models.Unit(g.blueprint);
    if (!model->ok) continue;
    const Quat turn{0, std::sin(g.heading * 0.5f), 0, std::cos(g.heading * 0.5f)};
    meshes.Add(model, Mat4::FromTRS(g.position, turn, model->scale), {0.6f, 0.6f, 0.6f}, 1, nullptr, &g.tint);
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
  // (0xAARRGGBB to the debug drawing's 0xAABBGGRR)
  auto swapped = [](uint32_t c) { return (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16); };
  const Vec3 lift{0, 0.15f, 0};
  for (const Line& l : lines) {
    if (l.width > 1.25f) draw.WideLine(l.a + lift, l.b + lift, swapped(l.color), l.width);
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
  (made ? cell.made : cell.ofMap).push_back(key);
  // how far it reaches from its place: its bounds' far corner, by the matrix's largest scale
  const Model* m = prop->model;
  float scale = 0;
  for (int k = 0; k < 3; ++k) scale = std::max(scale, Length(Vec3{prop->world.m[0][k], prop->world.m[1][k], prop->world.m[2][k]}));
  propReach_ = std::max(propReach_, std::max(Length(m->boundsMin), Length(m->boundsMax)) * scale);
}

void World::UnlistProp(PlacedProp* prop, uint32_t key, bool made) {
  if (prop->cell < 0 || static_cast<size_t>(prop->cell) >= propCells_.size()) return;
  PropCell& cell = propCells_[static_cast<size_t>(prop->cell)];
  std::vector<uint32_t>& list = made ? cell.made : cell.ofMap;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == key) {
      list[i] = list.back();
      list.pop_back();
      break;
    }
  prop->cell = -1;
}

void World::AddPropEvents(const std::vector<FramePropEvent>& events) {
  for (const FramePropEvent& e : events) propEvents_.push_back(e);
}

// The model of a prop as the simulation has it now: its blueprint's own, or the mesh a script set
// in its place. The game makes a burnt version of every mesh under the name "<mesh>_wreck": a dead
// unit's wreck is the prop DefaultWreckage with "/units/<id>/<id>_mesh_wreck", a burnt tree its
// own blueprint with its own mesh's wreck.
const Model* World::PropModel(const std::string& blueprint, const std::string& mesh) {
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
    return models.Wreck(models.Unit(id), false);
  }
  const Model* named = vfs_->Exists(moho::Vfs::Normalize(mesh + ".bp")) ? models.Blueprint(mesh + ".bp") : nullptr;
  return named && named->ok ? named : models.Blueprint(blueprint);
}

void World::ApplyPropEvent(const FramePropEvent& e) {
  const bool ofMap = e.mapIndex >= 0;
  if (ofMap && static_cast<size_t>(e.mapIndex) >= props_.size()) return;
  // (out of its square's list first; it is listed again where it is now)
  if (ofMap) {
    UnlistProp(&props_[static_cast<size_t>(e.mapIndex)], static_cast<uint32_t>(e.mapIndex), false);
  } else {
    auto had = madeProps_.find(e.id);
    if (had != madeProps_.end()) UnlistProp(&had->second, e.id, true);
  }
  if (e.kind == FramePropEvent::Destroy) {
    if (ofMap) props_[static_cast<size_t>(e.mapIndex)].model = nullptr;
    else madeProps_.erase(e.id);
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
    kept = placed;
    ListProp(&kept, e.id, true);
  }
  ++propsChanged;
}

void World::AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors) {
  auto colorOf = [&](uint32_t army) { return armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : armyColors[army % armyColors.size()]; };
  ++frameStamp_;
  rememberedGone.clear();
  dyingDrawn = 0;
  bool noFocus = false;
  for (const FrameUnit& u : frame.units) {
    if (u.recon == 0xff) noFocus = true;
    if (u.dead) {
      if (u.Seen()) remembered_.erase(u.id);  // (the army saw it go)
      // A unit that is dying is there until its death is over: a plane falling out of the sky, a ship
      // going down, a walker toppling. Drawn as the simulation moves and poses it, without anything
      // that belongs to a live unit (symbol, bars, lamps, wake; it cannot be pointed at).
      const std::string& dying = (*frame.blueprints)[u.blueprint];
      const Model* body = u.Seen() && !dying.empty() ? models.Unit(dying) : nullptr;
      if (body && body->ok) {
        Pose pose;
        pose.now = frame.bones.data() + u.firstBone;
        pose.nowCount = u.boneCount;
        pose.next = frame.bones.data() + u.nextFirstBone;
        pose.nextCount = u.nextBoneCount;
        pose.blend = frame.blend;
        meshes.Add(body, Mat4::FromTRS({u.position[0], u.position[1], u.position[2]}, Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]}, body->scale),
                   colorOf(u.Army()), u.fractionComplete, &pose);
        ++dyingDrawn;
      }
      continue;
    }
    // of another army's unit only what the army the picture is for knows: the unit when it is seen,
    // its blip when it is on radar, sonar or omni alone, nothing otherwise
    const std::string& blueprint = (*frame.blueprints)[u.blueprint];
    // (what the army has seen before it remembers: a building stays where it stood, drawn as it
    // was; a unit keeps its own icon when it is a blip. A building: by the third letter of its
    // id, "ueb0101", the game's own way of naming them - units of mods that name otherwise are
    // not remembered)
    const bool known = u.SeenBefore();
    const bool building = blueprint.size() > 2 && (blueprint[2] == 'b' || blueprint[2] == 'B');
    const bool remembered = known && building;
    const bool blip = u.Blip() && !remembered;
    Vec3 position{u.position[0], u.position[1], u.position[2]};
    Quat orientation{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    const Vec3 color = colorOf(u.Army());
    float fractionComplete = u.fractionComplete;
    // another army's building is kept as it is while it is seen (0x10: seen at some time, which
    // the army's own and its allies' never say), and drawn as it was kept while it is not: what
    // has become of it since, the army does not know
    bool asRemembered = false;
    if (building && u.recon != 0xff && (u.recon & 0x10)) {
      if (u.Seen()) {
        remembered_[u.id] = {u.blueprint, position, color, orientation, fractionComplete, frameStamp_};
      } else {
        auto kept = remembered_.find(u.id);
        if (kept != remembered_.end()) {
          kept->second.stamp = frameStamp_;
          position = kept->second.position;
          orientation = kept->second.orientation;
          fractionComplete = kept->second.fractionComplete;
          asRemembered = true;
        }
      }
    }
    if (!u.Seen() && !blip && !remembered) continue;
    if (!blueprint.empty()) {  // its strategic icon, when it is far enough away
      if (u.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[u.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      icons.Add(kind, position, color, isSelected, blip, known);
      if (isSelected && haveBrackets_) {  // the original's brackets about it, turned as the unit is
        AddBrackets(0, position, orientation, kind->selectionX * 1.1f, kind->selectionZ * 1.1f);
      } else if (isSelected) {  // (the game's pictures are not there: a ring)
        const Vec3 forward = Rotate(orientation, {0, 0, 1});
        decals.AddMark(selectionRing_, position, kind->selectionX * 2.2f, kind->selectionZ * 2.2f, std::atan2(forward.x, forward.z));
      } else if (u.id == hover && hoverKind >= 1 && hoverKind <= 3 && haveBrackets_) {
        AddBrackets(hoverKind, position, orientation, kind->selectionX * 1.1f, kind->selectionZ * 1.1f);
      }
    }
    const Model* model = blueprint.empty() || blip ? nullptr : models.Unit(blueprint);
    // Its bars, as the original draws them (Chris's pictures of it, 2026-10-13): the life bar under
    // the unit, about as wide as the unit, black with a filling that is green, yellow at middling
    // health and red when low, on every unit near enough to be a model (and on a selected or
    // pointed-at one at any distance); the work bar directly under it, black with an orange filling,
    // on the unit that does the work (of the army the picture is for: the others' work is not the
    // player's to see). And the game's pause mark over a paused unit, above its top.
    // (not on a unit whose script has hidden all of it: what a building is upgraded to stands in
    // the same place, unseen, until it is done, and the original shows no bar for it)
    bool allHidden = false;
    for (uint32_t i = u.firstBone; i < u.firstBone + u.boneCount && i < frame.bones.size(); ++i)
      if (frame.bones[i].bone == 0) {
        allHidden = frame.bones[i].hidden;
        break;
      }
    if (!blip && !blueprint.empty() && u.Seen() && !allHidden) {
      const StrategicIcons::Kind* kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool own = focusArmy < 0 || static_cast<int>((u.id >> 20) & 0xff) == focusArmy;
      const float away = Length(position - view.eye);
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      const bool life = lifeBars && u.maxHealth > 0 && (isSelected || u.id == hover || away < kind->fadeIn * 0.9f);
      const bool work = workBars && own && u.workProgress > 0 && u.workProgress < 1;
      if (life || work) {
        // (the size, height and place the unit's blueprint gives its life bar, in world units: the bar
        // lies that far from the unit's middle down the screen, along the ground)
        const float perPixel = std::max(2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1)) * away, 1e-5f);
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
        if (life) icons.AddBar(at, u.health / u.maxHealth, wide, false, 0, thick);
        if (work) icons.AddBar(at, u.workProgress, wide, true, life ? thick : 0, thick);
      }
      if (workBars && own && u.paused) {
        if (pauseMark_ == -2) pauseMark_ = icons.MarkIcon("pause");
        const float top = model && model->ok ? std::max(model->boundsMax.y * model->scale, 0.3f) + 0.35f : kind->selectionZ * 1.1f + 0.4f;
        icons.AddMark(pauseMark_, position + Vec3{0, top, 0}, 16);
      }
    }
    float radius = 0.5f;
    Vec3 centre = position + Vec3{0, 0.4f, 0};
    if (blip) {
      // (its blip alone, which can be pointed at)
    } else if (model && model->ok) {
      Pose pose;
      pose.now = frame.bones.data() + u.firstBone;
      pose.nowCount = u.boneCount;
      pose.next = frame.bones.data() + u.nextFirstBone;
      pose.nextCount = u.nextBoneCount;
      pose.blend = frame.blend;
      // a personal shield: the script has set a shield's mesh on the unit (the name says so; FAF
      // sets other meshes on units too)
      bool shielded = false;
      if (u.mesh && u.mesh < frame.blueprints->size() && fractionComplete >= 1 && u.Seen()) {
        std::string set = (*frame.blueprints)[u.mesh];
        for (char& c : set) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        shielded = set.find("shield") != std::string::npos;
      }
      meshes.Add(model, Mat4::FromTRS(position, orientation, model->scale), color, shielded ? 3.0f : fractionComplete, asRemembered ? nullptr : &pose);
      if (u.fractionComplete >= 1 && u.Seen()) AddLamps(model, position, orientation, &pose, &color);
      // a wake, when it goes on the water: by how far it has come since the frame before
      if (haveMap_ && map.water.present && u.Seen() && std::fabs(position.y - map.water.elevation) < 0.4f) {
        Moving& m = moving_[u.id];
        const double dt = clock_ - m.clock;
        if (dt > 1e-4 && dt < 1.0) m.speed += (Length(position - m.place) / static_cast<float>(dt) - m.speed) * 0.2f;
        else if (dt >= 1.0 || dt < 0) {
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
          if (clock_ - m.trailClock > 0.33 || m.trailCount == 0) {
            for (int k = std::min(m.trailCount, 11); k > 0; --k) m.trail[k] = m.trail[k - 1];
            m.trail[0] = stern;
            m.trailCount = std::min(m.trailCount + 1, 12);
            m.trailClock = clock_;
          }
          Vec3 places[13];
          places[0] = stern;
          for (int k = 0; k < m.trailCount; ++k) places[k + 1] = m.trail[k];
          const float length = std::min(m.speed * 1.8f, 14.0f) + halfZ, strength = std::min(m.speed / 2.5f, 1.0f);
          if (m.trailCount >= 2) wakes.AddPath(places, m.trailCount + 1, length, halfX * 0.7f, strength);
          else wakes.Add(stern, level, length, halfX * 0.7f, strength);
        }
      }
      centre = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
      radius = Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale;
    } else {  // not known what it is, or its files cannot be read: a box in the army's colour
      auto byte = [](float v) { return static_cast<uint32_t>(Clamp(v, 0, 1) * 255.0f); };
      draw.Box(centre, {0.4f, 0.4f, 0.4f}, 0xff000000u | byte(color.z) << 16 | byte(color.y) << 8 | byte(color.x));
    }
    pickables_.push_back({u.id, centre, radius});
  }
  // remembered buildings that are gone (destroyed where the army did not see it): each stays, as
  // it was last seen, until the army sees the place again and finds it gone
  if (noFocus) remembered_.clear();
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
    if (!blueprint.empty()) {
      if (r.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[r.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      icons.Add(kind, r.position, r.color, false, false, true);
      rememberedGone.push_back({kind, r.position, r.color});
      const Model* model = models.Unit(blueprint);
      if (model && model->ok) meshes.Add(model, Mat4::FromTRS(r.position, r.orientation, model->scale), r.color, r.fractionComplete);
    }
    ++it;
  }
  // projectiles: one with a model as that model; the others are effects in the game - where the
  // game's own effect is not on it, a short soft glowing band along the flight stands in
  for (const FrameProjectile& p : frame.projectiles) {
    if (!p.seen) continue;
    const Vec3 position{p.position[0], p.position[1], p.position[2]};
    const std::string& blueprint = (*frame.blueprints)[p.blueprint];
    const Model* model = blueprint.empty() ? nullptr : models.Blueprint(blueprint);
    if (model && model->ok) {
      const Quat orientation{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
      // in its own colours: the original gives every mesh that is not a unit's opaque white where a
      // unit gets its army's colour (docs/engine-answers.md, render 7)
      // (at the size the simulation holds for it: its blueprint's at launch, grown by its scale
      // velocity, or what a script set: the UEF's box of light round what it builds, a nuke's ball)
      Mat4 at = Mat4::FromTRS(position, orientation, p.sized ? 1.0f : model->scale);
      for (int row = 0; row < 3 && p.sized; ++row)
        for (int k = 0; k < 3; ++k) at.m[row][k] *= std::max(p.size[k], 0.0f);
      // what fades with its age (the UEF's build slices: whole for two seconds, then gone at
      // 0.145 a second, the original's numbers) says how much of it is left
      float left = 1;
      if (!model->lods.empty() && model->lods[0].fades) left = Clamp(1 - (p.age - 2.0f) * 0.145f, 0, 1);
      // (a nuke's ball of fire is drawn by its age)
      const bool fire = !model->lods.empty() && model->lods[0].nuke;
      if (left > 0) meshes.Add(model, at, fire ? Vec3{p.age, 0, 0} : Vec3{1, 1, 1}, left);
    } else if (!effects.OnEntity(p.id)) {
      const Vec3 velocity{p.velocity[0], p.velocity[1], p.velocity[2]};
      ribbons.PlainBeam(position - velocity * 0.06f, position, {1.0f, 0.95f, 0.78f}, 0.12f);
    }
  }
  // what scripts made that has a mesh: shields' bubbles and the like, each its mesh blueprint's model
  for (const FrameEntity& f : frame.entities) {
    if (!f.seen || f.mesh >= frame.blueprints->size()) continue;
    const std::string& mesh = (*frame.blueprints)[f.mesh];
    if (mesh.empty() || !vfs_->Exists(moho::Vfs::Normalize(mesh + ".bp"))) continue;  // (a script may write the name in any case)
    const Model* model = models.Blueprint(mesh + ".bp");
    if (!model->ok) continue;
    Mat4 at = Mat4::FromTRS({f.position[0], f.position[1], f.position[2]}, Quat{f.orientation[0], f.orientation[1], f.orientation[2], f.orientation[3]}, model->scale);
    for (int row = 0; row < 3; ++row)
      for (int k = 0; k < 3; ++k) at.m[row][k] *= f.scale[k];
    // (in its own colours where its look has no team colour of its own: the original gives every
    // mesh that is not a unit's opaque white; an adjacency's node was a cube in the army's colour)
    const bool teamed = !model->lods.empty() && model->lods[0].teamMask;
    meshes.Add(model, at, f.army == 0xff || !teamed ? Vec3{1, 1, 1} : colorOf(f.army));
  }
  // a weapon's beam without the game's own effect on it: a plain band in a lightened army colour
  for (const FrameBeam& b : frame.beams) {
    if (effects.OnEntity(b.id)) continue;
    const Vec3 c = colorOf(b.army) * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
    ribbons.PlainBeam({b.start[0], b.start[1], b.start[2]}, {b.end[0], b.end[1], b.end[2]}, c, 0.25f);
  }
}

void World::Render() {
  const bool shadowed = haveMap_ && shadowsOn;
  lampShadowsDrawn = shadowed && lampShadows && look == Look::Modern ? lights.PlaceShadows(camera, view) : 0;
  lights.Build(&view);  // (after everything has said what lights there are; before any pass reads the view)
  {  // the bones of posed models, once for all the frame's passes
    std::vector<const View*> views{&view};
    if (shadowed)
      for (int i = 0; i < shadows.Count(); ++i) views.push_back(&shadows.CascadeView(i));
    for (int i = 0; i < lampShadowsDrawn; ++i) views.push_back(&lights.ShadowView(i));
    meshes.Prepare(views);
  }
  particles.Update(clock_, &view);
  ribbons.Update(clock_);

  // the sun's shadow maps first, then the scene into its own picture, then that onto the window
  if (shadowed) {
    for (int i = 0; i < shadows.Count(); ++i) {
      device_->BeginPass(shadows.Pass(i));
      terrain.Draw(shadows.CascadeView(i), true);
      meshes.Draw(shadows.CascadeView(i), environment_, true);
    }
  }
  // the lamps' shadow maps: units and scenery as each lamp sees them
  for (int i = 0; i < lampShadowsDrawn; ++i) {
    device_->BeginPass(lights.ShadowPass(i));
    meshes.Draw(lights.ShadowView(i), environment_, true);
  }
  const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
  post.BeginScene(view.width, view.height, clear);
  device_->SetTexture(kShadowMapSlot, shadows.Map());
  device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
  device_->SetTexture(kEnvironmentSlot, sky_);
  lights.Bind();
  if (haveMap_) {
    terrain.Draw(view);
    // decals on the ground, before anything stands on it: they read the ground's depth
    if (decals.Count() && hide.find("decals") == std::string::npos) {
      post.BeginColorOnly();
      lights.Bind();
      decals.Draw(view, camera, post.Depth(), clock_);
      post.ContinueScene();
      lights.Bind();
    }
    // range rings on the land, before anything stands on it (their picture is made from the
    // ground's depth; over the water they are laid after the water)
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
      post.ContinueScene();
      lights.Bind();
      meshes.Draw(view, environment_, false, false, 2);
    } else {
      meshes.Draw(view, environment_);
    }
    draw.Flush(view);
    // ambient occlusion: from the depth of everything solid, laid over it before the water
    if (look == Look::Modern && ao.strength > 0) {
      ao.Compute(view, camera, post.Depth());
      post.ContinueScene();
      ao.Apply(view);
    }
    skyDraw.Draw(view, camera, skyLight);  // where nothing solid is
    // the water: the modern look's sees through to a copy of the picture as it is now
    if (look == Look::Modern && map.water.present && post.CopyBehind()) {
      post.BeginColorOnly();
      lights.Bind();
      device_->SetTexture(kShadowMapSlot, shadows.Map());
      device_->SetTexture(kLampShadowSlot, lights.ShadowMap());
      water.DrawModern(view, camera, post.Behind(), post.Depth());
      post.ContinueScene();
      lights.Bind();
    } else {
      water.Draw(view);
    }
    if (ranges.Any() && map.water.present) {
      post.BeginColorOnly();
      ranges.Lay(view, camera, post.Depth(), true);
      post.ContinueScene();
      lights.Bind();
    }
  } else {
    draw.Flush(view);
    skyDraw.Draw(view, camera, skyLight);
  }
  // what is see-through (shields' bubbles, frames of light): over everything solid and the water
  meshes.Draw(view, environment_, false, true);
  if (!placed.empty()) placedDraw.Draw(view, placed, lights.night);
  // beams and trails, then particles: over everything solid and the water, by the scene's depth
  wakes.Draw(view);
  if (hide.find("bands") == std::string::npos) ribbons.Draw(view);
  shafts.Draw(view);
  if (hide.find("particles") == std::string::npos) {
    // into the scene's colour alone: they test the scene's depth themselves and fade where they
    // meet something solid
    post.BeginColorOnly();
    particles.Draw(view, post.Depth(), camera.zNear);
    // those that bend what is behind them (the shimmer over heat): with the picture as it is now
    if (particles.Bending() && post.CopyBehind()) {
      post.BeginColorOnly();
      particles.DrawBending(view, post.Behind(), post.Depth(), camera.zNear);
    }
    post.ContinueScene();
    lights.Bind();
  }
  if (view.constants.skyZenith[3] > 0.5f) falling.Draw(view, camera, rain, snow);
  if (haveMap_ && sight.On() && sightShown) {  // the fog of war: what is not seen, darkened
    post.BeginColorOnly();
    sight.neverSeen = unexplored ? 0.04f : sight.darkness;
    sight.waterLevel = map.water.present ? map.water.elevation : -1e9f;
    sight.Draw(view, camera, post.Depth(), static_cast<float>(map.width), static_cast<float>(map.height));
  }
  post.Present(view);
  icons.showSymbols = iconsOn;
  icons.Draw();  // on the finished picture, as the original draws them (and the life bars)
}

bool World::Project(Vec3 p, float* x, float* y) const {
  const Mat4& m = view.constants.viewProj;
  const float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
  if (w <= 1e-4f) return false;
  *x = ((m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3]) / w * 0.5f + 0.5f) * static_cast<float>(view.width);
  *y = (0.5f - (m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3]) / w * 0.5f) * static_cast<float>(view.height);
  return true;
}

bool World::GroundAt(float x, float y, Vec3* hit) const {
  if (view.height <= 0) return false;
  const Ray ray = camera.PixelRay(x, y, static_cast<float>(view.width), static_cast<float>(view.height));
  if (haveMap_ && HitGround(map, ray, true, hit)) return true;
  return HitPlaneY(ray, camera.target.y, hit);
}

uint32_t World::UnitAt(float x, float y) const {
  if (view.height <= 0) return 0xffffffffu;
  const Ray ray = camera.PixelRay(x, y, static_cast<float>(view.width), static_cast<float>(view.height));
  // a far-away unit is its strategic icon: it can be hit within about ten pixels of its middle
  const float worldPerPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  uint32_t best = 0xffffffffu;
  float bestAlong = 1e30f;
  for (const Pickable& p : pickables_) {
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
