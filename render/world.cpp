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
      shadows(device),
      device_(device),
      vfs_(vfs) {}

bool World::Open(const std::string& mapPath, const Options& options, std::string* error) {
  if (!draw.Init(error) || !meshes.Init(error) || !particles.Init(error) || !ribbons.Init(error) || !decals.Init(error) || !icons.Init(error) ||
      !ao.Init(error) || !post.Init(error) || !skyDraw.Init(error) || !falling.Init(error) || !sight.Init(error)) {
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
      if (!model->ok) continue;
      const Vec3 s = p.scale * model->scale;
      Mat4 world = {{{p.axisX.x * s.x, p.axisY.x * s.y, p.axisZ.x * s.z, p.position.x},
                     {p.axisX.y * s.x, p.axisY.y * s.y, p.axisZ.y * s.z, p.position.y},
                     {p.axisX.z * s.x, p.axisY.z * s.y, p.axisZ.z * s.z, p.position.z},
                     {0, 0, 0, 1}}};
      props_.push_back({model, world});
    }
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
    icons.Define(models.UnitBlueprintPath(look.id), look.icon, look.iconPriority, look.iconFadeIn, look.selectionX, look.selectionZ);
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
      tint = tint * (1 / std::max({tint.x, tint.y, tint.z, 0.05f}));
    }
    if (light.cone > 0)
      lights.AddBeam(at, light.radius * model->scale, tint * on, Rotate(orientation, Rotate(boneTurn, light.direction)), light.cone, light.glow);
    else
      lights.Add(at, light.radius * model->scale, tint * on);
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
  lampUnitsBefore_ = lampUnits_;
  lampUnits_ = 0;
  lampSquares_.clear();
  // a time of day: the computed sky's light in place of the map's (modern look)
  Vec3 lightDirection = sunDirection;
  if (timeOfDay >= 0 && look == Look::Modern) {
    // the weather: what it is by itself, its clouds, and what it has done to surfaces by now
    float flash = 0;
    if (weatherAuto) {
      const WeatherNow now = WeatherAt(weatherSeed, clock, climate, weatherSpell);
      cloudCover = now.clouds;
      rain = now.rain;
      snow = now.snow;
      fog = now.fog;
      flash = now.flash;
    } else {
      flash = LightningAt(weatherSeed, clock, rain);
    }
    const float dt = weatherTime_ < 0 ? 0 : std::clamp(time - weatherTime_, 0.0f, 0.5f);
    weatherTime_ = time;
    wet = std::clamp(wet + (rain > 0 ? dt / 15 : -dt / 120), 0.0f, 1.0f);
    snowLying = std::clamp(snowLying + (snow > 0 ? dt / 60 : -dt / 180), 0.0f, 1.0f);
    const float cover = std::max({cloudCover, rain > 0 ? 0.8f + 0.2f * rain : 0.0f, snow > 0 ? 0.75f + 0.2f * snow : 0.0f, fog * 0.6f});
    skyLight = SkyAt(timeOfDay, cover);
    if (flash > 0) {  // lightning: for a moment the clouds light everything (less to an eye opened for the night)
      const Vec3 bolt = Vec3{0.75f, 0.82f, 1.0f} * (flash * 1.1f / std::pow(skyLight.exposure, 0.6f));
      skyLight.zenith += bolt;
      skyLight.horizon += bolt * 0.6f;
    }
    lampsOn_ = std::clamp((0.30f - skyLight.daylight) / 0.18f, 0.0f, 1.0f);
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
    SetSky(skyLight, &c);
    lightDirection = skyLight.direction;
  }
  if (haveMap_ && shadowsOn) shadows.Place(camera, lightDirection, &view);

  meshes.Begin();
  icons.Begin(view);
  lights.Begin(clock);
  pickables_.clear();
  for (const PlacedProp& p : props_) meshes.Add(p.model, p.world, {1, 1, 1});
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
  // (0xAARRGGBB to the debug drawing's 0xAABBGGRR)
  auto swapped = [](uint32_t c) { return (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16); };
  const Vec3 lift{0, 0.15f, 0};
  for (const Line& l : lines) draw.Line(l.a + lift, l.b + lift, swapped(l.color));
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

void World::AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors) {
  auto colorOf = [&](uint32_t army) { return armyColors.empty() ? Vec3{0.6f, 0.6f, 0.6f} : armyColors[army % armyColors.size()]; };
  for (const FrameUnit& u : frame.units) {
    if (u.dead) continue;
    // of another army's unit only what the army the picture is for knows: the unit when it is seen,
    // its blip when it is on radar, sonar or omni alone, nothing otherwise
    const bool blip = u.Blip();
    if (!u.Seen() && !blip) continue;
    const Vec3 position{u.position[0], u.position[1], u.position[2]};
    const Quat orientation{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    const Vec3 color = colorOf(u.Army());
    const std::string& blueprint = (*frame.blueprints)[u.blueprint];
    if (!blueprint.empty()) {  // its strategic icon, when it is far enough away
      if (u.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[u.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      icons.Add(kind, position, color, isSelected, blip);
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
      meshes.Add(model, Mat4::FromTRS(position, orientation, model->scale), color, u.fractionComplete, &pose);
      if (u.fractionComplete >= 1) AddLamps(model, position, orientation, &pose, &color);
      centre = position + Rotate(orientation, (model->boundsMin + model->boundsMax) * (0.5f * model->scale));
      radius = Length(model->boundsMax - model->boundsMin) * 0.5f * model->scale;
    } else {  // not known what it is, or its files cannot be read: a box in the army's colour
      auto byte = [](float v) { return static_cast<uint32_t>(Clamp(v, 0, 1) * 255.0f); };
      draw.Box(centre, {0.4f, 0.4f, 0.4f}, 0xff000000u | byte(color.z) << 16 | byte(color.y) << 8 | byte(color.x));
    }
    pickables_.push_back({u.id, centre, radius});
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
      meshes.Add(model, Mat4::FromTRS(position, orientation, model->scale), {1, 1, 1});
    } else if (!effects.OnEntity(p.id)) {
      const Vec3 velocity{p.velocity[0], p.velocity[1], p.velocity[2]};
      ribbons.PlainBeam(position - velocity * 0.06f, position, {1.0f, 0.95f, 0.78f}, 0.12f);
    }
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
  lights.Build(&view);  // (after everything has said what lights there are; before any pass reads the view)
  {  // the bones of posed models, once for all the frame's passes
    std::vector<const View*> views{&view};
    if (shadowed)
      for (int i = 0; i < shadows.Count(); ++i) views.push_back(&shadows.CascadeView(i));
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
  const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
  post.BeginScene(view.width, view.height, clear);
  device_->SetTexture(kShadowMapSlot, shadows.Map());
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
    meshes.Draw(view, environment_);
    draw.Flush(view);
    // ambient occlusion: from the depth of everything solid, laid over it before the water
    if (look == Look::Modern && ao.strength > 0) {
      ao.Compute(view, camera, post.Depth());
      post.ContinueScene();
      ao.Apply(view);
    }
    skyDraw.Draw(view, camera, skyLight);  // where nothing solid is
    water.Draw(view);
  } else {
    draw.Flush(view);
    skyDraw.Draw(view, camera, skyLight);
  }
  // beams and trails, then particles: over everything solid and the water, by the scene's depth
  if (hide.find("bands") == std::string::npos) ribbons.Draw(view);
  if (hide.find("particles") == std::string::npos) particles.Draw(view);
  if (view.constants.skyZenith[3] > 0.5f) falling.Draw(view, camera, rain, snow);
  if (haveMap_ && sight.On()) {  // the fog of war: what is not seen, darkened
    post.BeginColorOnly();
    sight.Draw(view, camera, post.Depth(), static_cast<float>(map.width), static_cast<float>(map.height));
  }
  post.Present(view);
  if (iconsOn) icons.Draw();  // on the finished picture, as the original draws them
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
