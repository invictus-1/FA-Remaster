// Strategic icons: the small symbols that stand for units seen from far away. Each unit's blueprint
// names its symbol (StrategicIconName), says from which distance it shows (Display.Mesh.IconFadeInZoom)
// and which symbol lies on top where several overlap (StrategicIconSortPriority, 0 on top).
//
// The symbols are the game's own little pictures (/textures/ui/common/game/strategicicons/<name>_rest.dds,
// 12 to 32 pixels). They are drawn the way the original does it: on the finished picture, over
// everything, pixel for pixel, their grey parts in the army's colour. All symbols a game uses are
// kept in one texture, so a whole army's are one draw call.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

class StrategicIcons {
 public:
  StrategicIcons(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {}
  bool Init(std::string* error);

  // What a kind of unit shows.
  struct Kind {
    int icon = -1;       // which symbol (-1: none: the blueprint names none, or its file is missing)
    float fadeIn = 130;  // the distance from the camera at which the symbol is fully there
    int priority = 128;  // 0 lies on top, 255 at the bottom
    int iconSelected = -1;  // the symbol of a selected unit (<name>_selected.dds; -1: the plain one)
    // the ring under a selected unit: half its extent along the unit's own x and z (the blueprint's
    // SelectionSizeX, SelectionSizeZ)
    float selectionX = 0.5f, selectionZ = 0.5f;
  };
  // The kind a unit blueprint file describes ("/units/uel0201/uel0201_unit.bp"); read once.
  // Never null.
  const Kind* ForBlueprint(const std::string& blueprintPath);
  // Says what the kind of that blueprint file shows, in place of reading the file (the values the
  // simulation has). `icon` empty: none; `fadeIn`, `selectionX`, `selectionZ` 0: not said.
  void Define(const std::string& blueprintPath, const std::string& icon, int priority, float fadeIn, float selectionX, float selectionZ);

  // Empties the list; `view` is what the frame is seen with.
  void Begin(const View& view);
  // A unit of that kind at that place in the world, of an army with that colour (rgb in 0..1).
  // Nothing is listed while the unit is nearer than its kind's distance, or out of the picture.
  // `selected`: its selected form, where the game's files have one.
  void Add(const Kind* kind, Vec3 position, Vec3 color, bool selected = false);
  // The same symbol drawn into plain pixels (0xAARRGGBB rows from the top, `width` x `height`), its
  // middle at (x, y): for pictures made without the graphics card (a minimap).
  void Blit(const Kind* kind, Vec3 color, bool selected, int x, int y, uint32_t* pixels, int width, int height) const;
  // Draws the list into the current pass, which must be the window's.
  void Draw();

  // How many screen pixels one pixel of a symbol covers. 0: chosen from the picture's height
  // (1 up to 1440 lines, 2 for 2160), so symbols stay readable on a dense screen.
  float scale = 0;
  // A symbol comes in over this share of its distance, ending at the distance (0.1: from 90% of it).
  float fadeShare = 0.1f;
  int drawn = 0;  // symbols in the last Draw

 private:
  struct Rect {
    int x = 0, y = 0, width = 0, height = 0;  // in the shared texture
  };
  struct Listed {
    float x, y;  // the centre, in screen pixels
    int icon;
    uint32_t color;  // 0xAABBGGRR; alpha: how far it has come in
    int priority;
  };
  struct Vertex {
    float x, y, u, v;
    uint32_t color;
  };
  int Icon(const std::string& name, const char* suffix = "_rest");

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  gfx::Pipeline pipeline_;
  gfx::Buffer buffer_;
  // the shared texture: symbols are added in rows as they are first asked for
  static constexpr int kAtlasSize = 512;
  std::vector<uint8_t> atlasPixels_;
  gfx::Texture atlas_;
  bool atlasChanged_ = false;
  int rowX_ = 1, rowY_ = 1, rowHeight_ = 0;
  std::vector<Rect> rects_;
  std::unordered_map<std::string, int> iconByName_;
  std::unordered_map<std::string, std::unique_ptr<Kind>> kinds_;
  struct Said {  // what Define was told, until a unit of that kind is first shown
    std::string icon;
    int priority = 0;
    float fadeIn = 0, selectionX = 0, selectionZ = 0;
  };
  std::unordered_map<std::string, Said> said_;

  View view_;
  float pixel_ = 1;  // this frame's scale
  std::vector<Listed> listed_;
  std::vector<uint32_t> order_;
  std::vector<Vertex> vertices_;
};

}  // namespace render
