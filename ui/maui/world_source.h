// Where the world view gets the world's picture from, and the answers that belong to that picture.
//
// The interface does not draw the world: the renderer does (render/world_picture.h). The host
// that holds the simulation gives a session's state one of these; the world view control asks it
// for the picture as its camera sees it, for where places of the world are on that picture and
// for what is under a point of it. Without one (a host without the renderer) the world view is
// empty and answers with a plain straight-down view.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace moho::ui {

struct Image;
struct IntRect;

// The camera as the interface keeps it: the place it looks at, how far away it is, heading and
// pitch in radians (pitch pi/2 looks straight down), the upright field of view in radians.
struct WorldCamera {
  float target[3] = {0, 0, 0};
  float distance = 100;
  float heading = 0;
  float pitch = 1.5707963f;
  float fovY = 0.9f;
  // its LOD scale (the console's cam_SetLOD, GeomCamera3::SetLODScale 0x470b90: 1 at first; FAF sets 0.7 for the Level
  // of Detail "Extreme"): the models' levels of detail and the decals' and effects' cutoffs go by it (render::Camera)
  float lodScale = 1;
};

// What the interface adds to the world's picture: marks under the selected units, the building
// being placed (a ghost, green where it can stand and red where not), the lines of orders.
struct WorldGhost {
  std::string blueprint;
  float position[3] = {0, 0, 0};
  float heading = 0;
  bool free = true;
};
struct WorldLine {
  float a[3], b[3];
  uint32_t color;   // 0xAARRGGBB
  float width = 1;  // pixels on the screen, the same from near and far
};
// The order lines while Shift is held, as the original's command graph draws them (user_units.cpp
// OrderLinesForWorld works them out; WorldPicture::SetOrderLines draws them, renderer request 30):
// each leg between two of the graph's nodes, its curve's directions at both ends, its width in world
// units by the units on it, its colour by its state, its texture an index into the scene's
// orderTextures (-1: none) moving along it at animRate; each mark a waypoint's icon at a node.
struct WorldOrderLeg {
  float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  float ta[3] = {0, 0, 0}, tb[3] = {0, 0, 0};  // unit length
  float baseWidth = 1;          // CalculateWaypointLineWidth(units): min(sqrt(units), 10)
  uint32_t color = 0xffffffffu;  // 0xAARRGGBB
  int texture = -1;
  float uvAspect = 1, animRate = 0;
  bool operator==(const WorldOrderLeg&) const = default;
};
struct WorldOrderMark {
  float position[3] = {0, 0, 0};
  int texture = -1;              // -1: a solid square
  uint32_t color = 0xffffffffu;  // 0xAARRGGBB
  float size = 1;                // world units: the node's size times the order's waypoint scale
  bool operator==(const WorldOrderMark&) const = default;
};
// The original's console variables for them (ui_WaypointLineScale, ui_CurveSmoothness,
// ui_CurveSegments, ui_MinWaypointSize, ui_MaxWaypointSize): WorldPicture::SetOrderLineSettings.
struct WorldOrderSettings {
  float lineScale = 1, curveSmoothness = 35;
  int curveSegments = 32;
  float markMinPixels = 7, markMaxPixels = 100;
  bool operator==(const WorldOrderSettings&) const = default;
};
// Range rings: the rings of one of the game's range filters in one colour. They show as one
// outline round the ground they cover together; an inner radius cuts a hole.
struct WorldRangeRing {
  float centre[3];
  float radius;
  float innerRadius;  // 0: none
};
struct WorldRangeGroup {
  uint32_t color = 0xffffffffu;      // 0xAARRGGBB
  int glow = 0;                      // 0..255, the first byte of the script's colour
  // The outline's thickness at the nearest and the farthest zoom, and a hole's outline's (range_rings.cpp gives
  // this frame's thickness at both ends, worked out by the original's rule).
  float outer[2] = {0.04f, 4.0f};
  float inner[2] = {0.04f, 4.0f};
  bool fill = false;  // range_Fill: white at 1/8 over what the group covers inside its lines (RangeFill)
  std::vector<WorldRangeRing> rings;
};
// A mesh the game's script has placed in the world (its WorldMesh, or the mark where an order
// landed); how it is drawn goes by the shader's name.
struct WorldMeshView {
  std::string mesh, texture, shader;
  float uniformScale = 1;
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // a quaternion x, y, z, w
  float scale[3] = {1, 1, 1};
  uint32_t color = 0xffffffffu;
  bool hidden = false;
  float lifetime = 0, age = 0;  // seconds given with SetLifetimeParameter (0: none), and how many have passed
  float fractionComplete = 1;
  float lodCutoff = 1000;  // the description's LODCutoff (worldmesh.lua's DEFAULTCUTOFF when it gives none)
};
// A decal the interface's script lays on the ground (the game's UserDecal, FA exe ScriptedDecal):
// a texture of the game's files over a square of size[0] x size[1] world units (the script's
// scale x and z) centred on the position, north up.
struct WorldDecal {
  std::string texture;
  float position[3] = {0, 0, 0};
  float size[2] = {1, 1};
};
struct WorldScene {
  std::vector<uint32_t> selection;  // entity ids
  std::vector<WorldGhost> ghosts;
  std::vector<WorldLine> lines;
  // The order lines and their waypoints' marks (empty when Shift is not held), the textures they
  // name, and the console variables they are drawn by.
  std::vector<std::string> orderTextures;
  std::vector<WorldOrderLeg> orderLegs;
  std::vector<WorldOrderMark> orderMarks;
  WorldOrderSettings orderSettings;
  // The range rings, and the camera's distances at its nearest and its farthest zoom (the rings'
  // thickness goes between its two numbers with them; the world view leaves them as they are, its rings
  // have the same thickness at both ends).
  std::vector<WorldRangeGroup> ranges;
  float zoomNear = 10, zoomFar = 1000;
  std::vector<WorldMeshView> meshes;
  std::vector<WorldDecal> decals;  // (WorldPicture::SetUserDecals, renderer request 25)
  // The strategic icons' colours by army counted from 0 (TeamColorMode); empty: the armies' own
  // (WorldPicture::SetIconColors, renderer request 26).
  std::vector<uint32_t> iconColors;
  std::vector<uint32_t> armyColors;  // the units' own colours (Colour Vision; empty: the game's own)
  // The playable part of the map, x0, z0, x1, z1 (SyncPlayableRect; x1 <= x0: the whole map):
  // WorldPicture::SetPlayableRect (renderer request 27).
  float playableRect[4] = {0, 0, -1, -1};
  // The unit under the mouse, to be marked as the original marks it (brackets around it by how
  // its army stands to the player's): 0xffffffff for none. Kinds: 1 the player's own or an
  // ally's, 2 an enemy's, 3 a neutral's. A selected unit is not named here: it has its mark.
  uint32_t hover = 0xffffffffu;
  int hoverKind = 0;
  // The map's border: the mesh blueprints the game script named with MapBorderAdd (the machinery
  // around the map, by the interface's skin), to be drawn around the map as the original does.
  std::vector<std::string> borders;
  // Life bars over units (the selected ones, the one under the mouse, damaged ones near by): the
  // player's switch (UI_RenderUnitBars).
  bool lifeBars = false;
  // And over the enemy units the army sees now too: the player's option "Always Show Enemy
  // Lifebars" (the console's UI_ForceLifbarsOnEnemy). Off, as the original's: then only the
  // army's own and its allies' units have life bars.
  bool lifeBarsOnEnemies = false;
  // The icons on the mass and hydrocarbon deposits from far up: the view's EnableResourceRendering (FA exe: the
  // world view's drawing calls CWldSession::RenderResources, 0x862a80, at 0x86ee44 while it is on, +0x136, and the
  // console's UI_RenResources too; the minimap's go with its own view, which draws no scene: WorldSource::DrawMinimap).
  bool resourceIcons = true;
  // The camera jumped since the picture before (a snap, a minimap click), for the temporal upscaler.
  bool cameraCut = false;
};

// A game's pace, kept by the host (the original's client manager keeps it, FA exe 0x53e590):
// the speed in the steps the speed keys count (the game runs 10^(rate / 10) times as fast as
// normal: WLD_GetSimRate 0x88d0b0), and whether the players may change it (the game option
// GameSpeed = "adjustable"; "fast" starts at 4).
struct GamePace {
  int rate = 0;
  bool adjustable = false;
  bool ended = false;  // SessionEndGame has been asked for (the simulation stands from the next beat on)
};

// A moving picture for the front end's background (the host's: app/backdrop.h): the newest one
// of exactly that size, 0xAARRGGBB rows from the top, valid until the next call; nullptr while
// there is none of that size yet.
class BackdropSource {
 public:
  virtual ~BackdropSource() = default;
  virtual const uint32_t* Latest(int width, int height) = 0;
  // The way without a read-back (the game's way, WorldSource::DrawsToWindow): true when the window's
  // own picture shows the map under the interface. The movie then gives no pixels: each frame it
  // shows, it calls Show with its rectangle (the window's picture is fitted to it) and draws the
  // veil Show answers over it (black of that alpha, 0..255: the picture darker than the world is,
  // and coming in from black), and black where the window lies outside it.
  virtual bool DrawsToWindow() const { return false; }
  virtual int Show(float x0, float y0, float x1, float y1) {
    (void)x0, (void)y0, (void)x1, (void)y1;
    return 255;
  }
};

// A picture over the whole window drawn by the graphics card, under the interface (the renderer's
// WorldPicture::SetBackdrop, ui REQUESTS 38): the loading screen's faction film and FAF's "IN TRANSIT" cover at the
// game's start, which the software target would otherwise stretch over every pixel of the window each frame. The
// host's (GameSession), while the renderer's picture shows the window; a control that draws such a picture over the
// whole window as the first thing of a frame (nothing of the interface under it: DrawTarget::Blank) offers it here
// and, taken, draws nothing of it itself. Ours, not the original's (its device draws every quad on the card anyway).
class CardBackdrop {
 public:
  virtual ~CardBackdrop() = default;
  // `image`'s texels `src` (whole ones, inside it) stretched over the whole window at `alpha` (0..1, the control's),
  // for this frame. False: not taken (no picture of the renderer's shows the window): the control draws it itself.
  virtual bool Take(const Image& image, const IntRect& src, float alpha) = 0;
};

// [views] One view of the world in the picture besides the main one (the renderer's WorldPicture::SetViews, ui
// REQUESTS 35, render-answers "request 35"): its place in the window's pixels, its camera, and its kind: the main view
// (view 0, the world view of the camera "WorldCamera"), a second world view of the original's split screen (FAF's
// worldview.lua viewRight, the camera "WorldCamera2": a full view of its own) or the follow camera's picture in a
// corner (ours: drawn every `every`th frame, flatter). The world view control gathers them (maui/worldview.cpp,
// GatherViews).
struct WorldViewSpec {
  enum class Kind { Main, Split, Inset };
  int x = 0, y = 0, width = 0, height = 0;
  WorldCamera camera;
  Kind kind = Kind::Main;
  int every = 2;  // (an inset: drawn every that many frames)
  bool operator==(const WorldViewSpec& o) const {
    return x == o.x && y == o.y && width == o.width && height == o.height && kind == o.kind && every == o.every &&
           camera.target[0] == o.camera.target[0] && camera.target[1] == o.camera.target[1] && camera.target[2] == o.camera.target[2] &&
           camera.distance == o.camera.distance && camera.heading == o.camera.heading && camera.pitch == o.camera.pitch &&
           camera.fovY == o.camera.fovY && camera.lodScale == o.camera.lodScale;
  }
};

class WorldSource {
 public:
  virtual ~WorldSource() = default;
  // [views] The views of the next pictures, view 0 (the main one) first, in the window of `windowW` x `windowH`; empty:
  // one view, the picture as it always was. Given every time the main view's picture is made sure of; true when they
  // differ from the views last given (the picture then is not the same). With views the main view's Draw is given the
  // window's size (the views' pictures are laid into the window's) and the picture's answers for view 0 are in its own
  // pixels. A source without views (no renderer) ignores them.
  virtual bool SetViews(const std::vector<WorldViewSpec>& views, int windowW, int windowH) {
    (void)views, (void)windowW, (void)windowH;
    return false;
  }
  // The view at a point of the window (an inset before what it lies over); -1: none. Without views: 0.
  virtual int ViewAt(float x, float y) {
    (void)x, (void)y;
    return 0;
  }
  // The answers of Project, GroundAt, UnitAt, UnitsIn and LifeBarAnchors for a view, x and y in its own pixels (a
  // window point less its place), by its frame last drawn. View 0's are those functions themselves.
  virtual bool ViewProject(int view, const float world[3], float* x, float* y) { return view == 0 && Project(world, x, y); }
  virtual bool ViewGroundAt(int view, float x, float y, float world[3]) { return view == 0 && GroundAt(x, y, world); }
  virtual uint32_t ViewUnitAt(int view, float x, float y) { return view == 0 ? UnitAt(x, y) : 0xffffffffu; }
  virtual void ViewUnitsIn(int view, float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) {
    if (view == 0) UnitsIn(x0, y0, x1, y1, out);
    else out->clear();
  }
  // What the next pictures show beside the world itself; it stays until it is set again.
  virtual void SetScene(const WorldScene& scene) = 0;
  // True when the world is shown in the window by the renderer itself (the interface laid over
  // it): Draw then gives no pixels to copy (a token), and the world view leaves its part of the
  // interface's picture transparent.
  virtual bool DrawsToWindow() const { return false; }
  // The minimap: the whole map from straight above with every unit's icon, width * height pixels
  // as Draw gives them (the map's height fills the picture's height: give a width in the map's
  // proportion). MapSize: the map's size in world units. The other two go between the world and
  // the minimap picture last drawn. `resourceIcons`: the minimap view's EnableResourceRendering.
  virtual const uint32_t* DrawMinimap(int width, int height, bool resourceIcons) = 0;
  virtual bool MapSize(float* width, float* height) = 0;
  virtual bool MinimapProject(const float world[3], float* x, float* y) = 0;
  virtual bool MinimapGroundAt(float x, float y, float world[3]) = 0;
  // The world as the camera sees it: width * height pixels, 0xAARRGGBB, rows from the top; valid
  // until the next call. nullptr when there is no picture.
  virtual const uint32_t* Draw(const WorldCamera& camera, int width, int height) = 0;
  // For the picture last drawn, in its pixels: where a place of the world is (false behind the
  // camera), the ground under a point, the unit under a point (its entity id, 0xffffffff for
  // none), the units whose middle lies in a rectangle.
  virtual bool Project(const float world[3], float* x, float* y) = 0;
  virtual bool GroundAt(float x, float y, float world[3]) = 0;
  virtual uint32_t UnitAt(float x, float y) = 0;
  virtual void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) = 0;
  // The life bars of the frame about to be shown (after Draw): for each unit with bars, the middle
  // of its bars and their lower edge, in the picture's pixels (where the original puts a unit's
  // custom name). Empty when not known.
  struct BarAnchor {
    uint32_t unit;
    float x, y;
  };
  virtual void LifeBarAnchors(std::vector<BarAnchor>* out) { out->clear(); }
  // [views] A view's (its own pixels; another view's are a frame late: the renderer's ViewLifeBarAnchors).
  virtual void ViewLifeBarAnchors(int view, std::vector<BarAnchor>* out) {
    if (view == 0) LifeBarAnchors(out);
    else out->clear();
  }
  // A unit's place and turn as the coming frame shows it (orientation a quaternion x, y, z, w), for
  // the camera following units (renderer request 28); false: not known here (the camera then takes
  // the last beat's).
  virtual bool UnitTransform(uint32_t id, float position[3], float orientation[4]) {
    (void)id, (void)position, (void)orientation;
    return false;
  }

  // --- the cinematic camera for replays (ours, not the original's: the renderer's CineCamera, render/cinecam.h,
  // held here beside the picture it needs) ---
  // While it is on, Draw shows the world from its view in place of the camera given, which stays the game's (the
  // world view's, that the player's input moves); after CineStop the picture eases back to the game's camera over a
  // moment. CineStart: switched on from the game's camera as it is now (`game`, as Draw takes it), following the
  // unit `follow` (an entity id; 0xffffffff: none, it holds where it is); false where there is no picture.
  // CineStop: switched off, `game` the game's camera that goes on from where the cinematic one was (the same eye
  // and way of looking); false (and nothing set) when it was off. CineFrame: once a frame, with the frame's
  // seconds: what it follows, its easing, and what the picture leaves out meanwhile.
  struct CineState {
    bool on = false, easingBack = false, following = false;
    float followed[3] = {0, 0, 0};  // the place it circles while following
    // What the picture was last given: the life bars, the selection's marks, the work bars (all left out while
    // the cinematic camera shows), and whether the lens's blur is on.
    bool lifeBars = false, selectionMarks = true, workBars = true, lens = false;
  };
  virtual bool CineStart(const WorldCamera&, uint32_t) { return false; }
  virtual bool CineStop(WorldCamera*) { return false; }
  virtual void CineFrame(float) {}
  virtual CineState Cine() const { return CineState(); }
  // The camera the picture last drawn was seen from, when it is not the game's (the cinematic camera's, or one on
  // its way back to the game's); false otherwise.
  virtual bool ShownCamera(WorldCamera*) const { return false; }
  // A photo of the world (render/world_picture.h, TakePhoto): `width` x `height` pixels, which may be larger than
  // the window, from the cinematic camera while it is on, else from `game`; nothing of the interface. The pixels
  // as Draw gives them; *madeWidth x *madeHeight what came (smaller where the card's memory would not do). False
  // without a picture.
  virtual bool TakePhoto(const WorldCamera&, int, int, std::vector<uint32_t>*, int*, int*) { return false; }

  // --- the photo mode (ours, not the original's: Chris's approval of 2026-10-18, the visuals session's look 7; the
  // renderer's calls, docs/render-answers.md "To ui: the photo mode's calls are in") ---
  // The cinematic camera above, flown freely (CineCamera::Fly) while the world view's photo mode is open
  // (maui/worldview.cpp: PhotoModeCommand); the game's camera stays where it is meanwhile, and while it is open the
  // picture leaves out the bars, marks and shapes as for the cinematic camera. PhotoStart: from the game's camera as
  // it is now (from what shows while the picture eases back from the cinematic camera), with the map's size for its
  // reach; false without a picture. PhotoFly: once a frame before CineFrame, what the player holds: `move` {right, up,
  // forward} -1..1 (3 with Shift), `turn` {right, down} as a share of the window's height, `roll` -1..1. PhotoStop:
  // the free camera off; the picture eases back to the game's camera as it is (CineCamera::Stop and Shown: what Stop
  // hands back is not taken) and shows the game's own hour and weather again (ClearPhotoMoment).
  struct PhotoLens {  // (render::Lens's: on, how much of its blur, the aperture, the focus; 0 auto-focus)
    bool on = false;
    float strength = 1, fNumber = 2.8f, focus = 0;
  };
  struct PhotoMoment {  // (WorldPicture::GetPhotoMoment's; `set`: one of the photo mode's, else the game's own)
    float hour = -1;    // -1: the map's own fixed light
    int weather = 1;    // a number of WeatherKinds
    float strength = 1;
    bool set = false;
  };
  struct PhotoPose {  // (the free camera's, CinePose)
    float eye[3] = {0, 0, 0};
    float yaw = 0, pitch = 0, roll = 0, fovY = 0;
    bool free = false;  // flown (CineCamera::Mode::Free)
  };
  virtual bool PhotoStart(const WorldCamera&) { return false; }
  virtual void PhotoFly(float, const float[3], const float[2], float) {}
  virtual void PhotoStop() {}
  virtual PhotoPose PhotoCamera() const { return PhotoPose(); }
  // The picture's own moment (WorldPicture::SetPhotoMoment, ClearPhotoMoment, GetPhotoMoment, WeatherKinds).
  virtual void SetPhotoMoment(float, int, float) {}
  virtual void ClearPhotoMoment() {}
  virtual PhotoMoment GetPhotoMoment() const { return PhotoMoment(); }
  virtual std::vector<std::string> WeatherKinds() const { return {}; }
  // The photo mode's lens (World::lens while it is open and while the picture eases back from it).
  virtual void SetPhotoLens(const PhotoLens&) {}
  // What is under a pixel of the picture last drawn (WorldPicture::FocusAt): its distance along the line of sight; 0
  // for the sky, while a photo is being made, and without a picture.
  virtual float FocusAt(float, float) { return 0; }
  // A photo made over the next frames from the free camera (WorldPicture::StartPhoto: `width` x `height`, the
  // renderer's own supersampling), its progress 0..1 (PhotoProgress) and its pixels once it is done (TakeDonePhoto,
  // as Draw gives them). StartPhoto false while one is being made or the photo mode is not open.
  virtual bool StartPhoto(int, int) { return false; }
  virtual float PhotoProgress() const { return 0; }
  virtual bool TakeDonePhoto(std::vector<uint32_t>*, int*, int*) { return false; }
};

}  // namespace moho::ui
