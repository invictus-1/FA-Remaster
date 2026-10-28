// [gpuscenery] The still scenery kept on the card and picked there (optimisation step 2, "a steady 120": docs/render-
// optimisation.md section 10; MeshRenderer::cardScenery).
//
// Without it, every frame the processor tests each of the map's trees and rocks for each pass (the camera's, the sun's
// cascades), chooses its level, sorts what it found by mesh and copies every instance into the draws' constants: on the
// remastered Seton's some 55,000 props, the whole map in view late in a game. With it the model renderer's groups of still
// scenery (MeshRenderer::AddGroup: one square of the map's each) are kept in a buffer on the card, written again only
// where a group changes, and each frame three compute passes do that work there for all the passes at once:
//  1. Pick: a thread a prop and pass. It tests the prop's sphere against the pass's planes (and the cascade before it, the
//     sun's), measures its size on the screen and chooses its level exactly as MeshRenderer::Consider does, and notes which
//     of its model's lists it goes in: one list per level, near (large on screen) and far apart (the depth of what is near
//     is laid first, MeshRenderer::depthPrepass).
//  2. Bases: a thread a model and pass. It counts each list and lays the lists one after the other, and writes each list's
//     indirect draw (its level's indices, how many instances) and where it begins.
//  3. Lay: each prop written into its list, in the order the processor would have drawn them (the groups as the world
//     lists them, a group's props as it added them): what is blended (far trees' cards) comes out the same.
// The processor then records one indirect draw for each list that can have something in it: per group and model it knows
// the distances and sizes, and so which levels its props may be at in each pass (a few levels a model at most).
//
// Kept on the processor's path (as before): the lamps' shadow passes (each sees a few squares), what is see-through
// (glass), a model of a look of its own (a shield, a nuke, an effect: none of the map's), more than eight levels, and the
// scenery the frame adds itself (Add: the world beyond the map's edge, hot wrecks while they glow). Everything that changes
// per prop is either the world's (a felled, burnt, removed or made prop: its group filled anew, rewritten on the card) or
// the shaders' (the wind, the blasts' waves, plants pressed and burnt, the snow on them: the same shaders, reading the same
// instance from the card).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "view.h"

namespace render {

class CardScenery {
 public:
  explicit CardScenery(gfx::Device* device) : device_(device) {}
  ~CardScenery();
  CardScenery(const CardScenery&) = delete;
  CardScenery& operator=(const CardScenery&) = delete;

  // Whether the card can take a model's props (its levels drawn as the plain models are; at most kMostLevels levels).
  static constexpr int kMostLevels = 8;
  static bool Takes(const Model* model);
  static constexpr int kMostViews = 8;

  // A prop of a group, as the model renderer keeps it: its model, its instance as the shaders read it (24 floats: the
  // world matrix's top three rows, colour, bones, wear: MeshRenderer::Instance), its bounding sphere and the radius its
  // size on screen is measured by.
  struct Member {
    const Model* model;
    const float* instance;
    Vec3 centre;
    float radius, sizeRadius;
  };
  // A group's props anew (or a new group): `order` is where the world lists it among the groups (they are drawn in that
  // order); the indices of the members the card does not take are put in `left` (the processor draws those).
  void SetGroup(uint32_t group, uint32_t order, const Member* members, size_t count, std::vector<uint32_t>* left);
  // Whether a group has been given (SetGroup) at all.
  bool Has(uint32_t group) const;

  // One pass the card picks for (the camera's, a sun cascade's), or the camera's motion of what sways (sway).
  struct Pass {
    const View* view = nullptr;
    bool shadow = false;
    bool sway = false;
    float smallest = 1.5f;  // pixels: smaller is not drawn (MeshRenderer::minPixels, shadowMinPixels)
    float wish = 0.55f;     // the triangles a pixel of the circle (MeshRenderer::Draw's `wish`, with pi / 4)
    float standard = 1;     // the four-level standard's factor (sqrt(detail, by shadowDetail in a shadow pass))
    float swayPixels = 128; // (sway) the least height on screen (MeshRenderer::swayMotionPixels)
  };
  // The frame's picking: the groups listed (MeshRenderer::ListGroup), the passes; the changes since the last frame
  // written to the card and the three passes dispatched. Between passes (it dispatches). False: nothing picked (the card
  // cannot, or its buffer could not be made): the processor draws it all this frame.
  bool Pick(const std::vector<uint32_t>& listed, const Pass* passes, int count, bool divertStream, uint64_t frame);
  // [zoomhitch] Pick's three compute pipelines made now (MeshRenderer::CardKeep, on the device's own thread while the
  // processor picks), not in the first frame the card picks (a zoom out past the switch: 5 ms with the driver's copy
  // kept, more on a new build).
  void PrepareAhead() { MakePipelines(); }

  // After Pick: the indirect draws of a pass that may have instances, by model, level and near: each one level's list
  // (`record`: the byte offset of its arguments in Buffer(); the vertex shader reads its list's start four words after
  // them, card_scenery.cpp).
  struct Draw {
    const Model* model;
    const ModelLod* lod;
    uint32_t model_ = 0;  // the model's number here (MeshRenderer keeps an entry for each)
    bool near = false;
    uint32_t record = 0;
  };
  const std::vector<Draw>& Draws(int pass) const { return draws_[pass]; }
  gfx::Buffer Buffer() const { return buffer_; }
  size_t Models() const { return models_.size(); }
  const Model* ModelAt(size_t i) const { return models_[i].model; }
  // What the card holds: props, models, lists; and the processor's time for the frame's picking (ms)
  size_t Props() const { return orderCount_; }
  size_t Lists() const { return buckets_; }
  float pickMs = 0;

 private:
  gfx::Device* device_;
  gfx::Pipeline pickCs_, basesCs_, layCs_;
  int made_ = 0;  // (the compute pipelines: 0 not yet, 1 made, -1 they cannot be)
  gfx::Buffer buffer_, listed_;
  uint32_t bufferWords_ = 0;

  // the models it has seen, each with its levels (the lods' table) and its props this layout
  struct ModelRec {
    const Model* model;
    size_t levels = 0;
    uint32_t lodFirst = 0, bucketFirst = 0, orderFirst = 0, count = 0, chunkFirst = 0, chunks = 0;
  };
  std::vector<ModelRec> models_;
  std::unordered_map<const Model*, uint32_t> modelOf_;
  uint32_t ModelIndex(const Model* model);

  // per group: where its props lie in the slots, which models they are (for the lists' order and the processor's guess
  // per pass), its box, when it was last listed
  struct Pair {
    uint32_t model;
    float smallest, largest;  // the least and most size radius of its props of that model
  };
  struct GroupRec {
    uint32_t order = 0xffffffffu;
    uint32_t first = 0, room = 0, count = 0;  // slots
    std::vector<uint32_t> models;             // each prop's model (its number)
    std::vector<Pair> pairs;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    bool sent = false;      // given (SetGroup)
    bool held = false;      // its slots are in the layout on the card
    uint64_t listed = 0;    // the frame it was last listed
  };
  std::vector<GroupRec> groups_;
  std::vector<uint32_t> slots_;  // the slots on the processor (32 words a prop), as on the card
  uint32_t slotsUsed_ = 0, slotRoom_ = 0;  // slots handed out, and the room the card's buffer has for them
  std::vector<std::pair<uint32_t, uint32_t>> dirtySlots_;  // slot ranges written since the last upload (first, count)
  bool layoutDirty_ = true, rebuild_ = true;
  uint64_t lastSweep_ = 0;

  // the layout's tables (words), as on the card after the slots
  uint32_t orderAt_ = 0, modelsAt_ = 0, lodsAt_ = 0, chunksAt_ = 0, viewsAt_ = 0, viewWords_ = 0, listsAt_ = 0, recordsAt_ = 0;
  uint32_t orderCount_ = 0, chunks_ = 0, buckets_ = 0, lodCount_ = 0;
  std::vector<uint32_t> tables_;  // order list, models, lods, chunks
  bool Layout(uint64_t frame);
  bool MakeBuffer();
  void Upload();

  // the processor's guess per pass, and what it found
  std::vector<Draw> draws_[kMostViews];
  struct Seen {
    float hi = 0, lo = 1e30f;  // the most and least pixels its props may be in this pass
    bool any = false;
  };
  std::vector<Seen> seen_[kMostViews];        // (each pass its own: the passes are guessed on several threads at once)
  std::vector<uint32_t> touched_[kMostViews];
  std::vector<int32_t> pairAt_;  // (SetGroup's: per model its pair in the group being set, -1 none)
  void Guess(const std::vector<uint32_t>& listed, const Pass& pass, int index, bool divertStream);
  bool MakePipelines();
};

}  // namespace render
