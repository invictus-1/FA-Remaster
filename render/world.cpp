#include "world.h"

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
      shadows(device),
      device_(device),
      vfs_(vfs) {}

bool World::Open(const std::string& mapPath, const Options& options, std::string* error) {
  if (!draw.Init(error) || !meshes.Init(error) || !particles.Init(error) || !ribbons.Init(error) || !decals.Init(error) || !icons.Init(error) ||
      !ao.Init(error) || !post.Init(error)) {
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
  }
  return true;
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
  if (haveMap_ && shadowsOn) shadows.Place(camera, sunDirection, &view);

  meshes.Begin();
  icons.Begin(view);
  lights.Begin(clock);
  pickables_.clear();
  for (const PlacedProp& p : props_) meshes.Add(p.model, p.world, {1, 1, 1});
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
    const Vec3 position{u.position[0], u.position[1], u.position[2]};
    const Quat orientation{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    const Vec3 color = colorOf(u.Army());
    const std::string& blueprint = (*frame.blueprints)[u.blueprint];
    if (!blueprint.empty()) {  // its strategic icon, when it is far enough away
      if (u.blueprint >= iconKinds_.size()) iconKinds_.resize(frame.blueprints->size(), nullptr);
      const StrategicIcons::Kind*& kind = iconKinds_[u.blueprint];
      if (!kind) kind = icons.ForBlueprint(models.UnitBlueprintPath(blueprint));
      const bool isSelected = !selected.empty() && selected.count(u.id) != 0;
      icons.Add(kind, position, color, isSelected);
      if (isSelected) {  // the ring on the ground under it, turned as the unit is
        const Vec3 forward = Rotate(orientation, {0, 0, 1});
        decals.AddMark(selectionRing_, position, kind->selectionX * 2.2f, kind->selectionZ * 2.2f, std::atan2(forward.x, forward.z));
      }
    }
    const Model* model = blueprint.empty() ? nullptr : models.Unit(blueprint);
    float radius = 0.5f;
    Vec3 centre = position + Vec3{0, 0.4f, 0};
    if (model && model->ok) {
      Pose pose;
      pose.now = frame.bones.data() + u.firstBone;
      pose.nowCount = u.boneCount;
      pose.next = frame.bones.data() + u.nextFirstBone;
      pose.nextCount = u.nextBoneCount;
      pose.blend = frame.blend;
      meshes.Add(model, Mat4::FromTRS(position, orientation, model->scale), color, u.fractionComplete, &pose);
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
    water.Draw(view);
  } else {
    draw.Flush(view);
  }
  // beams and trails, then particles: over everything solid and the water, by the scene's depth
  if (hide.find("bands") == std::string::npos) ribbons.Draw(view);
  if (hide.find("particles") == std::string::npos) particles.Draw(view);
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
