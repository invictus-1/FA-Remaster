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
  uint32_t color;  // 0xAARRGGBB
};
struct WorldScene {
  std::vector<uint32_t> selection;  // entity ids
  std::vector<WorldGhost> ghosts;
  std::vector<WorldLine> lines;
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
};

// A game's pace, kept by the host (the original's client manager keeps it, FA exe 0x53e590):
// the speed in the steps the speed keys count (the game runs 10^(rate / 10) times as fast as
// normal: WLD_GetSimRate 0x88d0b0), and whether the players may change it (the game option
// GameSpeed = "adjustable"; "fast" starts at 4).
struct GamePace {
  int rate = 0;
  bool adjustable = false;
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
  // The minimap: the whole map from straight above with every unit's icon, width * height pixels
  // as Draw gives them (the map's height fills the picture's height: give a width in the map's
  // proportion). MapSize: the map's size in world units. The other two go between the world and
  // the minimap picture last drawn.
  virtual const uint32_t* DrawMinimap(int width, int height) = 0;
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
};

}  // namespace moho::ui
