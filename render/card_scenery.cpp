// [gpuscenery] The still scenery kept on the card and picked there (card_scenery.h).
#include "card_scenery.h"

#include "jobs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace render {
namespace {

// The card's buffer, in 32-bit words:
//   slots    32 words a prop: its instance (24: the rows, colour, bones, wear), its sphere's middle and radius (24-27), its
//            size radius (28), its group (29)
//   order    the props by model, each model's in the order the processor draws them (the slot of each)
//   models   12 words a model: its first entry in the order and how many (0, 1), its first level in the levels' table and
//            how many levels (2, 3), its first list (4), its first chunk and how many (5, 6), how many of its levels have a
//            size of the four-level standard (8: Model::levelPixels; 0: chosen by their triangles)
//   levels   4 words a level: its index count, what it is (1 hidden, 2 blended, 4 casts no shadow, 8 a stream's, 16 it
//            sways), where its model goes on to the next level (the standard's size, or its triangles by the next level's,
//            as Consider compares them), its height in its own units (ModelLod::swayHeight)
//   chunks   4 words a chunk of up to 256 of a model's props: the model, its first entry in the order, how many
//   passes   per pass: each prop's list in it (choice: one word an entry of the order), each chunk's count per list (16 a
//            chunk; after Bases where its props begin in each list), the lists (a slot each), and per list its record: an
//            indexed draw's five arguments, then where the list begins (8 words)
// A model's lists: two a level (0: near, 1: far, as Consider's `near`), the sway motion's in the near one.
constexpr uint32_t kSlotWords = 32;
constexpr uint32_t kChunk = 256;
constexpr uint32_t kNone = 0xffffffffu;
constexpr float kNearPixels = 32;  // (mesh_renderer.cpp, kPrepassPixels)
constexpr int kMostGroups = 32768;  // (the groups' listed bits are in the constants: 1024 rows of four words)

uint32_t Align4(uint32_t n) { return (n + 3u) & ~3u; }

// The three passes. They read and write the card's buffer alone (u0 of space1) and their constants (b3).
const char kPickShader[] = R"hlsl(
RWByteAddressBuffer card : register(u0, space1);
cbuffer CardPick : register(b3) {
  uint4 pickSizes;   // x passes, y chunks, z models, w entries of the order
  uint4 pickAt;      // x the order, y the models, z the levels, w the chunks (words)
  uint4 pickViews;   // x the first pass's words, y words a pass, z its lists' place in it, w its records' place
  float4 viewPlanes[40];  // five a pass: a point p is inside when dot(plane.xyz, p) + plane.w >= 0
  float4 viewEye[8];      // xyz where levels are measured from, w pixels a world unit at a distance of one
  float4 viewLevel[8];    // x the least pixels drawn, y the triangles wished for a pixel, z the LOD scale, w the standard's factor
  float4 viewMode[8];     // x 1: a shadow pass; y 1: streams drawn by their own pass; z 1: the sway motion's; w its least pixels
  float4 viewRight[8];    // xyz the sun's sideways axis, w the square of the cascade before (x)
  float4 viewUp[8];       // xyz the sun's other axis, w that square's y
  float4 viewHalf[8];     // x that square's half side (0: none)
  uint4 listedBits[1024]; // the groups the world lists this frame, a bit each
};
static const uint kNone = 0xffffffffu;
uint Word(uint w) { return card.Load(w * 4); }
uint4 Word4(uint w) { return card.Load4(w * 4); }
uint PassBase(uint v) { return pickViews.x + v * pickViews.y; }

// Which list of its model a prop goes in for pass v, or none: as MeshRenderer::Consider chooses (its tests in that order).
uint Choose(uint slot, uint model, uint v) {
  uint s = slot * 32;
  float4 r0 = asfloat(Word4(s)), r1 = asfloat(Word4(s + 4)), r2 = asfloat(Word4(s + 8));
  float4 sphere = asfloat(Word4(s + 24));
  uint2 more = card.Load2((s + 28) * 4);
  uint group = more.y;
  if (((listedBits[group >> 7][(group >> 5) & 3] >> (group & 31)) & 1) == 0) return kNone;
  float3 c = sphere.xyz;
  float r = sphere.w;
  float3 lo = c - float3(r, r, r), hi = c + float3(r, r, r);
  [unroll] for (int k = 0; k < 5; ++k) {
    float4 p = viewPlanes[v * 5 + k];
    float d = p.x * (p.x >= 0 ? hi.x : lo.x) + p.y * (p.y >= 0 ? hi.y : lo.y) + p.z * (p.z >= 0 ? hi.z : lo.z) + p.w;
    if (d < 0) return kNone;
  }
  bool shadow = viewMode[v].x > 0.5;
  float half = viewHalf[v].x;
  if (shadow && half > 0 && abs(dot(c, viewRight[v].xyz) - viewRight[v].w) + r <= half && abs(dot(c, viewUp[v].xyz) - viewUp[v].w) + r <= half)
    return kNone;  // (the nearer cascade has it)
  float focal = viewEye[v].w;
  float pixels = 2 * asfloat(more.x) * focal / max(length(c - viewEye[v].xyz), 0.01);
  if (pixels < viewLevel[v].x) return kNone;
  float sized = pixels / viewLevel[v].z;
  uint4 m0 = Word4(pickAt.y + model * 12);
  uint standard = Word(pickAt.y + model * 12 + 8);
  uint level = 0;
  if (standard > 0) {
    float size = sized * 0.75 * viewLevel[v].w;
    while (level < standard && level + 1 < m0.w && size < asfloat(Word(pickAt.z + (m0.z + level) * 4 + 2))) ++level;
  } else {
    float wanted = viewLevel[v].y * sized * sized;
    while (level + 1 < m0.w && wanted * wanted < asfloat(Word(pickAt.z + (m0.z + level) * 4 + 2))) ++level;
  }
  uint4 lod = Word4(pickAt.z + (m0.z + level) * 4);
  if (lod.y & 1) return kNone;                             // hidden
  if ((lod.y & 8) && viewMode[v].y > 0.5) return kNone;     // a stream's: the streams' own pass draws it
  if (shadow && (lod.y & 6)) return kNone;                 // a far card, or what casts none: no shadow
  if (viewMode[v].z > 0.5) {  // the camera's motion of what sways: what is tall enough on the screen
    if ((lod.y & 16) == 0) return kNone;
    float tall = asfloat(lod.w) * sqrt(r0.x * r0.x + r1.x * r1.x + r2.x * r2.x);
    if (!(tall * focal >= viewMode[v].w * max(length(float3(r0.w, r1.w, r2.w) - viewEye[v].xyz), 0.01))) return kNone;
    return level * 2;
  }
  return level * 2 + (pixels >= 32 ? 0 : 1);
}

groupshared uint chosen[256];
groupshared uint counted[16];

// 1. each prop's list in each pass, and each chunk's count per list
[numthreads(256, 1, 1)] void PickCS(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
  uint chunk = g.x, v = g.y;
  if (t < 16) counted[t] = 0;
  GroupMemoryBarrierWithGroupSync();
  uint4 ch = Word4(pickAt.w + chunk * 4);
  uint base = PassBase(v);
  if (t < ch.z) {
    uint e = ch.y + t;
    uint b = Choose(Word(pickAt.x + e), ch.x, v);
    if (b != kNone) InterlockedAdd(counted[b], 1u);
    card.Store((base + e) * 4, b);
  }
  GroupMemoryBarrierWithGroupSync();
  if (t < 16) card.Store((base + pickSizes.w + chunk * 16 + t) * 4, counted[t]);
}

// 2. per model and pass: its lists laid one after the other (each chunk's place in each), and each list's draw
[numthreads(64, 1, 1)] void BasesCS(uint3 id : SV_DispatchThreadID) {
  uint m = id.x, v = id.y;
  if (m >= pickSizes.z) return;
  uint4 m0 = Word4(pickAt.y + m * 12), m1 = Word4(pickAt.y + m * 12 + 4);
  uint lists = min(m0.w * 2, 16u);
  uint base = PassBase(v), counts = base + pickSizes.w;
  uint total[16], run[16];
  [unroll] for (uint b = 0; b < 16; ++b) total[b] = 0;
  for (uint c = m1.y; c < m1.y + m1.z; ++c)
    for (uint b = 0; b < lists; ++b) total[b] += Word(counts + c * 16 + b);
  uint at = base + pickViews.z + m0.x;
  for (uint b = 0; b < lists; ++b) {
    run[b] = at;
    at += total[b];
  }
  for (uint c = m1.y; c < m1.y + m1.z; ++c)
    for (uint b = 0; b < lists; ++b) {
      uint w = counts + c * 16 + b;
      uint n = Word(w);
      card.Store(w * 4, run[b]);
      run[b] += n;
    }
  for (uint b = 0; b < lists; ++b) {
    uint record = base + pickViews.w + (m1.x + b) * 8;
    uint indices = Word(pickAt.z + (m0.z + b / 2) * 4);
    card.Store4(record * 4, uint4(indices, total[b], 0, 0));
    card.Store4((record + 4) * 4, uint4(0, run[b] - total[b], 0, 0));
  }
}

// 3. each prop into its list, after those of its chunk before it in the order
[numthreads(256, 1, 1)] void LayCS(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
  uint chunk = g.x, v = g.y;
  uint4 ch = Word4(pickAt.w + chunk * 4);
  uint base = PassBase(v);
  uint b = t < ch.z ? Word(base + ch.y + t) : kNone;
  chosen[t] = b;
  GroupMemoryBarrierWithGroupSync();
  if (b == kNone) return;
  uint rank = 0;
  for (uint j = 0; j < t; ++j) rank += chosen[j] == b ? 1u : 0u;
  uint at = Word(base + pickSizes.w + chunk * 16 + b) + rank;
  card.Store(at * 4, Word(pickAt.x + ch.y + t));
}
)hlsl";

// The constants of the three passes (kPickShader's CardPick).
struct PickConstants {
  uint32_t sizes[4], at[4], views[4];
  float planes[40][4];
  float eye[8][4], level[8][4], mode[8][4], right[8][4], up[8][4], half[8][4];
  uint32_t listed[1024][4];
};

// The level Consider chooses for a model at `pixels` across (before the view's LOD scale) in a pass.
size_t LevelAt(const Model* model, float pixels, const CardScenery::Pass& pass) {
  const float sized = pixels / pass.view->lodScale;
  const std::vector<ModelLod>& lods = model->lods;
  size_t level = 0;
  const std::vector<float>& sizes = model->levelPixels;
  if (!sizes.empty()) {
    const float size = sized * 0.75f * pass.standard;
    while (level < sizes.size() && level + 1 < lods.size() && size < sizes[level]) ++level;
  } else {
    const float wanted = pass.wish * sized * sized;
    while (level + 1 < lods.size() &&
           wanted * wanted < static_cast<float>(lods[level].indexCount / 3) * static_cast<float>(lods[level + 1].indexCount / 3))
      ++level;
  }
  return level;
}

}  // namespace

CardScenery::~CardScenery() {
  if (buffer_) device_->Destroy(buffer_);
  for (gfx::Pipeline* p : {&pickCs_, &basesCs_, &layCs_})
    if (*p) device_->Destroy(*p);
}

bool CardScenery::Takes(const Model* model) {
  if (!model || !model->ok || model->lods.empty() || model->lods.size() > static_cast<size_t>(kMostLevels)) return false;
  for (const ModelLod& l : model->lods)
    if (l.glass || l.impact || l.unlit || l.nuke || l.fades || l.shield || l.frame || l.shell || !l.indexCount) return false;
  return true;
}

uint32_t CardScenery::ModelIndex(const Model* model) {
  auto known = modelOf_.find(model);
  if (known != modelOf_.end()) return known->second;
  const uint32_t index = static_cast<uint32_t>(models_.size());
  models_.push_back({model});
  modelOf_.emplace(model, index);
  layoutDirty_ = true;
  return index;
}

bool CardScenery::Has(uint32_t group) const { return group < groups_.size() && groups_[group].sent; }

void CardScenery::SetGroup(uint32_t group, uint32_t order, const Member* members, size_t count, std::vector<uint32_t>* left) {
  if (group >= groups_.size()) groups_.resize(group + 1);
  GroupRec& g = groups_[group];
  if (order != g.order) layoutDirty_ = true;
  g.order = order;
  g.sent = true;
  left->clear();
  std::vector<uint32_t> taken;
  taken.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (Takes(members[i].model)) taken.push_back(static_cast<uint32_t>(i));
    else left->push_back(static_cast<uint32_t>(i));
  }
  const uint32_t n = static_cast<uint32_t>(taken.size());
  std::vector<uint32_t> models(n);
  for (uint32_t k = 0; k < n; ++k) models[k] = ModelIndex(members[taken[k]].model);
  if (models != g.models && g.held) layoutDirty_ = true;
  // its slots: where it was while it fits there, else room of its own at the end (the card's buffer made again, larger,
  // when that is past its room)
  if (n > g.room) {
    g.room = n + std::max<uint32_t>(8, n / 4);
    g.first = slotsUsed_;
    slotsUsed_ += g.room;
    if (slotsUsed_ > slotRoom_) rebuild_ = true;
    if (g.held) layoutDirty_ = true;
  }
  if (slots_.size() < static_cast<size_t>(g.first + g.room) * kSlotWords) slots_.resize(static_cast<size_t>(g.first + g.room) * kSlotWords);
  g.count = n;
  g.models.swap(models);
  g.lo = {1e30f, 1e30f, 1e30f};
  g.hi = {-1e30f, -1e30f, -1e30f};
  g.pairs.clear();
  for (uint32_t k = 0; k < n; ++k) {
    const Member& m = members[taken[k]];
    uint32_t* w = slots_.data() + static_cast<size_t>(g.first + k) * kSlotWords;
    std::memcpy(w, m.instance, 24 * sizeof(float));
    const float sphere[5] = {m.centre.x, m.centre.y, m.centre.z, m.radius, m.sizeRadius};
    std::memcpy(w + 24, sphere, sizeof sphere);
    w[29] = group;
    w[30] = w[31] = 0;
    g.lo = {std::min(g.lo.x, m.centre.x - m.radius), std::min(g.lo.y, m.centre.y - m.radius), std::min(g.lo.z, m.centre.z - m.radius)};
    g.hi = {std::max(g.hi.x, m.centre.x + m.radius), std::max(g.hi.y, m.centre.y + m.radius), std::max(g.hi.z, m.centre.z + m.radius)};
    if (pairAt_.size() < models_.size()) pairAt_.resize(models_.size(), -1);
    int32_t& at = pairAt_[g.models[k]];
    if (at < 0) {
      at = static_cast<int32_t>(g.pairs.size());
      g.pairs.push_back({g.models[k], m.sizeRadius, m.sizeRadius});
    } else {
      Pair& pair = g.pairs[static_cast<size_t>(at)];
      pair.smallest = std::min(pair.smallest, m.sizeRadius);
      pair.largest = std::max(pair.largest, m.sizeRadius);
    }
  }
  for (const Pair& p : g.pairs) pairAt_[p.model] = -1;
  if (n) dirtySlots_.push_back({g.first, n});
}

// The order, the models', levels' and chunks' tables made anew from the groups held (and the slots packed anew first when
// they no longer fit the card's buffer); the buffer made again where it is too small.
bool CardScenery::Layout(uint64_t frame) {
  (void)frame;
  if (rebuild_) {  // (the groups' slots one after the other again, each with room to grow)
    uint32_t need = 0;
    for (const GroupRec& g : groups_) need += g.count ? g.count + std::max<uint32_t>(8, g.count / 4) : 0;
    std::vector<uint32_t> packed(static_cast<size_t>(need) * kSlotWords);
    uint32_t at = 0;
    for (GroupRec& g : groups_) {
      if (!g.count) {
        g.first = g.room = 0;
        continue;
      }
      const uint32_t room = g.count + std::max<uint32_t>(8, g.count / 4);
      std::memcpy(packed.data() + static_cast<size_t>(at) * kSlotWords, slots_.data() + static_cast<size_t>(g.first) * kSlotWords,
                  static_cast<size_t>(g.count) * kSlotWords * sizeof(uint32_t));
      g.first = at;
      g.room = room;
      at += room;
    }
    slots_.swap(packed);
    slotsUsed_ = at;
    slotRoom_ = std::max<uint32_t>(4096, at + at / 2);
    slots_.resize(static_cast<size_t>(slotRoom_) * kSlotWords);
    dirtySlots_.clear();
  }
  // the props of each model, in the groups' order (as the world lists them), each group's as it added them
  for (ModelRec& m : models_) {
    m.count = 0;
    m.levels = std::min(m.model->lods.size(), static_cast<size_t>(kMostLevels));
  }
  std::vector<uint32_t> held;
  for (uint32_t i = 0; i < groups_.size(); ++i)
    if (groups_[i].held && groups_[i].count) held.push_back(i);
  std::stable_sort(held.begin(), held.end(), [&](uint32_t a, uint32_t b) { return groups_[a].order < groups_[b].order; });
  for (uint32_t gi : held)
    for (uint32_t m : groups_[gi].models) ++models_[m].count;
  uint32_t entries = 0, lods = 0, chunks = 0, lists = 0;
  for (ModelRec& m : models_) {
    m.orderFirst = entries;
    entries += m.count;
    m.lodFirst = lods;
    lods += static_cast<uint32_t>(m.levels);
    m.chunkFirst = chunks;
    m.chunks = (m.count + kChunk - 1) / kChunk;
    chunks += m.chunks;
    m.bucketFirst = lists;
    lists += static_cast<uint32_t>(m.levels) * 2;
  }
  orderCount_ = entries;
  lodCount_ = lods;
  chunks_ = chunks;
  buckets_ = lists;
  orderAt_ = slotRoom_ * kSlotWords;
  modelsAt_ = orderAt_ + Align4(entries);
  lodsAt_ = modelsAt_ + static_cast<uint32_t>(models_.size()) * 12;
  chunksAt_ = lodsAt_ + lods * 4;
  viewsAt_ = Align4(chunksAt_ + chunks * 4);
  listsAt_ = Align4(entries) + chunks * 16;
  recordsAt_ = listsAt_ + Align4(entries);
  viewWords_ = recordsAt_ + lists * 8;
  tables_.assign(static_cast<size_t>(viewsAt_ - orderAt_), 0u);
  uint32_t* order = tables_.data();
  std::vector<uint32_t> next(models_.size());
  for (size_t m = 0; m < models_.size(); ++m) next[m] = models_[m].orderFirst;
  for (uint32_t gi : held) {
    const GroupRec& g = groups_[gi];
    for (uint32_t k = 0; k < g.count; ++k) order[next[g.models[k]]++] = g.first + k;
  }
  uint32_t* models = tables_.data() + (modelsAt_ - orderAt_);
  uint32_t* levels = tables_.data() + (lodsAt_ - orderAt_);
  uint32_t* chunkRows = tables_.data() + (chunksAt_ - orderAt_);
  for (size_t mi = 0; mi < models_.size(); ++mi) {
    const ModelRec& m = models_[mi];
    const Model* model = m.model;
    uint32_t* row = models + mi * 12;
    row[0] = m.orderFirst;
    row[1] = m.count;
    row[2] = m.lodFirst;
    row[3] = static_cast<uint32_t>(m.levels);
    row[4] = m.bucketFirst;
    row[5] = m.chunkFirst;
    row[6] = m.chunks;
    row[8] = static_cast<uint32_t>(model->levelPixels.size());
    for (size_t l = 0; l < m.levels; ++l) {
      const ModelLod& lod = model->lods[l];
      uint32_t* lv = levels + (m.lodFirst + l) * 4;
      lv[0] = lod.indexCount;
      lv[1] = (lod.hidden ? 1u : 0u) | (lod.blend ? 2u : 0u) | (lod.noShadow ? 4u : 0u) | (lod.stream ? 8u : 0u) | (lod.sway > 0 ? 16u : 0u);
      float next = 0;
      if (!model->levelPixels.empty()) next = l < model->levelPixels.size() ? model->levelPixels[l] : 0;
      else if (l + 1 < model->lods.size()) next = static_cast<float>(lod.indexCount / 3) * static_cast<float>(model->lods[l + 1].indexCount / 3);
      std::memcpy(&lv[2], &next, 4);
      std::memcpy(&lv[3], &lod.swayHeight, 4);
    }
    for (uint32_t c = 0; c < m.chunks; ++c) {
      uint32_t* ch = chunkRows + (m.chunkFirst + c) * 4;
      ch[0] = static_cast<uint32_t>(mi);
      ch[1] = m.orderFirst + c * kChunk;
      ch[2] = std::min(kChunk, m.count - c * kChunk);
    }
  }
  layoutDirty_ = false;
  const uint64_t words = static_cast<uint64_t>(viewsAt_) + static_cast<uint64_t>(kMostViews) * viewWords_;
  if (words > 0x3fffffffull) return false;  // (beyond a buffer's reach: four gigabytes)
  if (rebuild_ || !buffer_ || words > bufferWords_) {
    rebuild_ = false;
    if (!MakeBuffer()) return false;
  } else {
    device_->UpdateBufferRange(buffer_, orderAt_ * 4, tables_.data(), static_cast<uint32_t>(tables_.size() * 4));
  }
  return true;
}

// The card's buffer made again with room to grow, everything held in it (the passes' part zeros).
bool CardScenery::MakeBuffer() {
  if (buffer_) device_->Destroy(buffer_);
  buffer_ = {};
  const uint64_t words = static_cast<uint64_t>(viewsAt_) + static_cast<uint64_t>(kMostViews) * viewWords_;
  const uint32_t room = static_cast<uint32_t>(std::min<uint64_t>(0x3fffffffull, (words + words / 4 + 4095) & ~uint64_t{4095}));
  std::vector<uint32_t> data(room, 0u);
  std::memcpy(data.data(), slots_.data(), std::min<size_t>(slots_.size(), static_cast<size_t>(slotRoom_) * kSlotWords) * sizeof(uint32_t));
  std::memcpy(data.data() + orderAt_, tables_.data(), tables_.size() * sizeof(uint32_t));
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Card;
  bd.size = room * 4;
  bd.data = data.data();
  buffer_ = device_->CreateBuffer(bd);
  bufferWords_ = buffer_ ? room : 0;
  dirtySlots_.clear();
  if (!buffer_) fprintf(stderr, "render: the scenery's buffer on the card could not be made (%u MB)\n", room / (256u * 1024u));
  return static_cast<bool>(buffer_);
}

// The slots written since the last frame, onto the card (neighbouring ranges in one copy).
void CardScenery::Upload() {
  if (dirtySlots_.empty() || !buffer_) return;
  std::sort(dirtySlots_.begin(), dirtySlots_.end());
  for (size_t i = 0; i < dirtySlots_.size();) {
    uint32_t first = dirtySlots_[i].first, end = first + dirtySlots_[i].second;
    size_t k = i + 1;
    for (; k < dirtySlots_.size() && dirtySlots_[k].first <= end + 64; ++k) end = std::max(end, dirtySlots_[k].first + dirtySlots_[k].second);
    end = std::min(end, slotRoom_);
    if (end > first)
      device_->UpdateBufferRange(buffer_, first * kSlotWords * 4, slots_.data() + static_cast<size_t>(first) * kSlotWords, (end - first) * kSlotWords * 4);
    i = k;
  }
  dirtySlots_.clear();
}

bool CardScenery::MakePipelines() {
  if (made_) return made_ > 0;
  made_ = -1;
  std::string error;
  gfx::ComputeDesc cd;
  cd.source = kPickShader;
  cd.name = "scenery on the card: picked";
  cd.entry = "PickCS";
  pickCs_ = device_->CreateCompute(cd, &error);
  cd.name = "scenery on the card: lists laid out";
  cd.entry = "BasesCS";
  if (pickCs_) basesCs_ = device_->CreateCompute(cd, &error);
  cd.name = "scenery on the card: lists filled";
  cd.entry = "LayCS";
  if (basesCs_) layCs_ = device_->CreateCompute(cd, &error);
  if (!layCs_) {
    fprintf(stderr, "render: the scenery cannot be picked on the card (%s); the processor picks it\n", error.c_str());
    return false;
  }
  made_ = 1;
  return true;
}

bool CardScenery::Pick(const std::vector<uint32_t>& listed, const Pass* passes, int count, bool divertStream, uint64_t frame) {
  const auto t0 = std::chrono::steady_clock::now();
  for (std::vector<Draw>& d : draws_) d.clear();
  if (count <= 0 || count > kMostViews || !device_->IndirectDraws() || !MakePipelines()) return false;
  // the groups listed: held on the card (one the world lists again after a while), and those not listed for a while let go
  // of (a map opened after another leaves the old one's groups unlisted)
  for (uint32_t g : listed) {
    if (g >= groups_.size()) continue;
    if (g >= static_cast<uint32_t>(kMostGroups)) return false;
    GroupRec& r = groups_[g];
    r.listed = frame;
    if (!r.held && r.count) {
      r.held = true;
      layoutDirty_ = true;
    }
  }
  if (frame >= lastSweep_ + 600) {
    lastSweep_ = frame;
    for (GroupRec& r : groups_)
      if (r.held && r.listed + 600 < frame) {
        r.held = false;
        layoutDirty_ = true;
      }
  }
  if (layoutDirty_ || rebuild_ || !buffer_) {
    if (!Layout(frame)) return false;
  }
  Upload();
  if (!buffer_) return false;
  // the passes' constants, and the groups listed
  static PickConstants k;  // (16 KB: not on the stack; the frames' thread alone)
  std::memset(&k, 0, sizeof k);
  k.sizes[0] = static_cast<uint32_t>(count);
  k.sizes[1] = chunks_;
  k.sizes[2] = static_cast<uint32_t>(models_.size());
  k.sizes[3] = Align4(orderCount_);
  k.at[0] = orderAt_;
  k.at[1] = modelsAt_;
  k.at[2] = lodsAt_;
  k.at[3] = chunksAt_;
  k.views[0] = viewsAt_;
  k.views[1] = viewWords_;
  k.views[2] = listsAt_;
  k.views[3] = recordsAt_;
  for (int i = 0; i < count; ++i) {
    const Pass& p = passes[i];
    const View& v = *p.view;
    for (int j = 0; j < 5; ++j) {
      k.planes[i * 5 + j][0] = v.planes[j].x;
      k.planes[i * 5 + j][1] = v.planes[j].y;
      k.planes[i * 5 + j][2] = v.planes[j].z;
      k.planes[i * 5 + j][3] = v.planes[j].w;
    }
    const float focal = static_cast<float>(v.height) / (2 * std::tan(v.fovY * 0.5f));
    const float eye[4] = {v.eye.x, v.eye.y, v.eye.z, focal};
    const float level[4] = {p.smallest, p.wish, v.lodScale, p.standard};
    const float mode[4] = {p.shadow ? 1.0f : 0.0f, divertStream ? 1.0f : 0.0f, p.sway ? 1.0f : 0.0f, p.swayPixels};
    const float right[4] = {v.coveredRight.x, v.coveredRight.y, v.coveredRight.z, v.coveredX};
    const float up[4] = {v.coveredUp.x, v.coveredUp.y, v.coveredUp.z, v.coveredY};
    std::memcpy(k.eye[i], eye, sizeof eye);
    std::memcpy(k.level[i], level, sizeof level);
    std::memcpy(k.mode[i], mode, sizeof mode);
    std::memcpy(k.right[i], right, sizeof right);
    std::memcpy(k.up[i], up, sizeof up);
    k.half[i][0] = v.coveredHalf;
  }
  for (uint32_t g : listed)
    if (g < static_cast<uint32_t>(kMostGroups)) k.listed[g >> 7][(g >> 5) & 3] |= 1u << (g & 31);
  // what each pass may draw, by the groups' boxes (the processor's side)
  Jobs::Shared().Run(count, [&](int i) { Guess(listed, passes[i], i, divertStream); });
  if (chunks_) {
    device_->Mark("scenery picked on the card");
    device_->SetShaderBuffer(buffer_, 0);
    device_->SetConstants(3, &k, sizeof k);
    device_->SetPipeline(pickCs_);
    device_->Dispatch(chunks_, static_cast<uint32_t>(count));
    device_->SetPipeline(basesCs_);
    device_->Dispatch((static_cast<uint32_t>(models_.size()) + 63) / 64, static_cast<uint32_t>(count));
    device_->SetPipeline(layCs_);
    device_->Dispatch(chunks_, static_cast<uint32_t>(count));
  } else {
    for (std::vector<Draw>& d : draws_) d.clear();
  }
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  pickMs = pickMs == 0 ? ms : pickMs * 0.95f + ms * 0.05f;
  return true;
}

// The lists a pass may find props in: for each group it sees (its box, as MeshRenderer::Draw passes a group over whole),
// each of its models' sizes on the screen from the box's nearest and farthest point, so the levels its props may be at.
void CardScenery::Guess(const std::vector<uint32_t>& listed, const Pass& pass, int index, bool divertStream) {
  std::vector<Draw>& out = draws_[index];
  const View& v = *pass.view;
  const float focal = static_cast<float>(v.height) / (2 * std::tan(v.fovY * 0.5f));
  std::vector<Seen>& seen = seen_[index];
  std::vector<uint32_t>& touched = touched_[index];
  if (seen.size() < models_.size()) seen.resize(models_.size());
  touched.clear();
  for (uint32_t gi : listed) {
    if (gi >= groups_.size()) continue;
    const GroupRec& g = groups_[gi];
    if (!g.held || g.pairs.empty()) continue;
    if (!v.BoxVisible(g.lo, g.hi)) continue;
    if (pass.shadow && v.CoveredByNearer((g.lo + g.hi) * 0.5f, Length(g.hi - g.lo) * 0.5f)) continue;
    const Vec3 e = v.eye;
    const Vec3 nearest{std::clamp(e.x, g.lo.x, g.hi.x), std::clamp(e.y, g.lo.y, g.hi.y), std::clamp(e.z, g.lo.z, g.hi.z)};
    const Vec3 farthest{std::max(std::fabs(e.x - g.lo.x), std::fabs(e.x - g.hi.x)), std::max(std::fabs(e.y - g.lo.y), std::fabs(e.y - g.hi.y)),
                        std::max(std::fabs(e.z - g.lo.z), std::fabs(e.z - g.hi.z))};
    // (a hair wider than the sizes can be: the card works them out in its own rounding)
    const float toNear = 2 * focal / std::max(Length(nearest - e), 0.01f) * 1.001f, toFar = 2 * focal / std::max(Length(farthest), 0.01f) * 0.999f;
    for (const Pair& p : g.pairs) {
      const float most = p.largest * toNear;
      if (most < pass.smallest) continue;
      const float least = p.smallest * toFar;
      Seen& s = seen[p.model];
      if (!s.any) {
        s.any = true;
        s.hi = most;
        s.lo = least;
        touched.push_back(p.model);
      } else {
        s.hi = std::max(s.hi, most);
        s.lo = std::min(s.lo, least);
      }
    }
  }
  std::sort(touched.begin(), touched.end());
  for (uint32_t mi : touched) {
    Seen& s = seen[mi];
    s.any = false;
    const ModelRec& m = models_[mi];
    if (!m.count) continue;
    const Model* model = m.model;
    const size_t fine = LevelAt(model, s.hi, pass), coarse = std::min(LevelAt(model, std::max(s.lo, pass.smallest), pass), m.levels - 1);
    for (size_t l = fine; l <= coarse; ++l) {
      const ModelLod& lod = model->lods[l];
      if (lod.hidden || (lod.stream && divertStream) || (pass.shadow && (lod.blend || lod.noShadow))) continue;
      const uint32_t record = (viewsAt_ + static_cast<uint32_t>(index) * viewWords_ + recordsAt_ + (m.bucketFirst + static_cast<uint32_t>(l) * 2) * 8) * 4;
      if (pass.sway) {
        if (lod.sway > 0) out.push_back({model, &lod, mi, true, record});
        continue;
      }
      if (s.hi >= kNearPixels) out.push_back({model, &lod, mi, true, record});
      if (s.lo < kNearPixels) out.push_back({model, &lod, mi, false, record + 8 * 4});
    }
  }
}

}  // namespace render
