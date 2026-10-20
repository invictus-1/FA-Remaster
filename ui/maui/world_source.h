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

// The camera as the interface keeps it: the place it looks at, how far away it is, heading and
// pitch in radians (pitch pi/2 looks straight down), the upright field of view in radians.
struct WorldCamera {
  float target[3] = {0, 0, 0};
  float distance = 100;
  float heading = 0;
  float pitch = 1.5707963f;
  float fovY = 0.9f;
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
};

class WorldSource {
 public:
  virtual ~WorldSource() = default;
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
