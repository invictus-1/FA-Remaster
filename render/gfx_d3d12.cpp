// Direct3D 12 backend of gfx.h.
//
// How it is put together:
//  - One root signature for every pipeline: four root constant-buffer views (b0..b3), one table of 44
//    textures (t0..t43) and the fixed samplers.
//  - Per back buffer ("frame slot"): a command allocator, a ring of upload pages for this frame's
//    constants and dynamic buffers, a share of the shader-visible descriptor heap, and the list of
//    resources destroyed while it was recorded. All of it is reused when the slot comes round again,
//    after the GPU has finished that frame.
//  - Every texture has one descriptor in a CPU-side heap; a draw's 32 textures are copied from there
//    into the frame's share of the shader-visible heap (once per distinct set per frame).
//  - Static buffers and textures are uploaded at creation through a separate command list, waiting for
//    the GPU each time (load time only).
//  - Compute shaders share the root signature; its last table holds the sixteen storage views (u0..u15),
//    copied from a CPU-side heap of their own at each dispatch. A storage target is in the
//    unordered-access state from a dispatch that writes it until a dispatch reads it or a pass begins.
#include "card_memory.h"  // (photofix2)
#include "gfx.h"
#include "gfx_vulkan_dxc.h"

#if defined(_WIN32)  // (the whole file: Direct3D 12 is Windows' alone)

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "gfx_d3d12_pipelines.h"  // [hitches]
#include "gfx_latency.h"
#include "gfx_pacing.h"
#include "hitch_log.h"  // [hitches]

namespace render::gfx {

// (IsBlockCompressed, BytesPerPixelOrBlock and Device::Create: gfx.cpp)

namespace {

template <class T>
class Com {
 public:
  Com() = default;
  Com(const Com& o) : p_(o.p_) {
    if (p_) p_->AddRef();
  }
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ~Com() { Reset(); }
  Com& operator=(Com o) noexcept {
    std::swap(p_, o.p_);
    return *this;
  }
  void Reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T* Get() const { return p_; }
  T* operator->() const { return p_; }
  T** Put() {
    Reset();
    return &p_;
  }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

constexpr int kFrames = 3;
constexpr int kTexSlots = 44;
constexpr int kCbSlots = 4;
constexpr uint32_t kMaxTextures = 16384;       // CPU-side descriptors, one per texture (slot 0: the null texture)
constexpr uint32_t kRingDescriptors = kFrames * kTexSlots * 4096;  // shader-visible, split between the frame slots
constexpr uint32_t kMaxColorTargets = 64;      // render-target views beyond the window's
constexpr uint32_t kMaxDepthViews = 128;       // depth views beyond the window's (one per layer of a depth target)
constexpr int kStorageSlots = 16;              // u0..u15
constexpr uint32_t kMaxStorageViews = 1024;    // storage views, one per level of a storage target (0: the null view)
constexpr size_t kPageSize = 8u << 20;
constexpr DXGI_FORMAT kBackFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

DXGI_FORMAT ToDxgi(Format f) {
  switch (f) {
    case Format::R8: return DXGI_FORMAT_R8_UNORM;
    case Format::A8: return DXGI_FORMAT_A8_UNORM;
    case Format::RG8: return DXGI_FORMAT_R8G8_UNORM;
    case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_sRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_sRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::R16: return DXGI_FORMAT_R16_UNORM;
    case Format::R16F: return DXGI_FORMAT_R16_FLOAT;
    case Format::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
    case Format::R32UI: return DXGI_FORMAT_R32_UINT;
    case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
    case Format::BC1_sRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case Format::BC2: return DXGI_FORMAT_BC2_UNORM;
    case Format::BC2_sRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
    case Format::BC3_sRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case Format::BC4: return DXGI_FORMAT_BC4_UNORM;
    case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
    case Format::BC7: return DXGI_FORMAT_BC7_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT ToDxgi(AttrType t) {
  switch (t) {
    case AttrType::Float1: return DXGI_FORMAT_R32_FLOAT;
    case AttrType::Float2: return DXGI_FORMAT_R32G32_FLOAT;
    case AttrType::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case AttrType::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case AttrType::UByte4: return DXGI_FORMAT_R8G8B8A8_UINT;
    case AttrType::UByte4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case AttrType::Short2N: return DXGI_FORMAT_R16G16_SNORM;
    case AttrType::Short4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
  }
  return DXGI_FORMAT_UNKNOWN;
}

void Complain(const char* what, HRESULT hr) {
  fprintf(stderr, "render: Direct3D 12: %s failed (0x%08lx)\n", what, static_cast<unsigned long>(hr));
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p = {};
  p.Type = type;
  return p;
}

D3D12_RESOURCE_DESC BufferResourceDesc(uint64_t size) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

struct Page {
  Com<ID3D12Resource> resource;
  uint8_t* cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  size_t size = 0;
};

struct Span {
  uint8_t* cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  ID3D12Resource* resource = nullptr;  // the page it lies in, and where in it (copies into textures)
  uint64_t offset = 0;
};

struct BufferRec {
  bool used = false;
  BufferKind kind = BufferKind::Vertex;
  bool dynamic = false;
  uint32_t stride = 0;
  uint32_t size = 0;
  int block = -1;                      // static: the block it is cut from (D3D12Device::blocks_)
  uint64_t offset = 0;                 // static: where in it
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;   // static: its place in the block; dynamic: this frame's span
  uint64_t frame = 0;                  // dynamic: the frame its contents belong to
  // [gpuscenery] a card buffer (BufferKind::Card): its own resource, which dispatches may write, and the state it is in now
  // (a buffer is back in the common state after each frame's lists are run)
  Com<ID3D12Resource> card;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
// [gpuscenery] what a card buffer is read in: by draws and dispatches through its t register, and as indirect draws' arguments
constexpr D3D12_RESOURCE_STATES kCardReadable =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

struct TextureRec {
  bool used = false;
  Com<ID3D12Resource> resource;
  // targets only
  bool target = false, depth = false;
  bool writable = false;  // currently in its render-target / depth-write state
  int width = 0, height = 0, layers = 1, samples = 1;
  uint32_t view = 0;      // its first render-target or depth view (index into that heap)
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  int pixelBytes = 4;     // textures: bytes a pixel (UpdateTexture; 0 for the block-compressed)
  // storage targets only
  bool storage = false;
  bool unordered = false;  // currently in the unordered-access state (written by a dispatch)
  int slices = 1, mips = 1;  // (slices: a volume's)
  uint32_t uav = 0;        // its first storage view, one per level (index into that heap)
};

struct BackgroundPipeline;
struct PipelineRec {
  bool used = false;
  bool compute = false;
  Com<ID3D12PipelineState> state;
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  std::shared_ptr<PendingPipeline> pending;  // (asked for in a batch, not made yet)
  std::shared_ptr<BackgroundPipeline> background;  // [snowhitch] (being made on the device's own thread, not taken yet)
};

// [snowhitch] A pipeline asked for between BeginBackgroundPipelines and EndBackgroundPipelines: what it is made from, how far
// its making is (under the device's backgroundMutex_), and what was made.
struct BackgroundPipeline {
  enum Stage { Queued, Making, Made, NotMade };
  std::shared_ptr<PendingPipeline> what;
  Stage stage = Queued;
  PipelineRec made;
  std::string why;  // (NotMade: why; it is made again on the frames' thread, which says so)
};

// A ray tracing structure (gfx.h): its buffer, and for a geometry not built yet what it is built from; for a
// scene, the texture slot its view takes (as a texture's, so SetScene binds it as SetTexture does).
struct StructureRec {
  bool used = false;
  bool scene = false;
  Com<ID3D12Resource> buffer;
  uint64_t size = 0;
  uint32_t texture = 0;
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> parts;
  uint64_t scratch = 0;
  // [cardcost] a scene built to be refitted (RefitScene): over how many instances (refitted only over as many); -1 not
  uint64_t refitCount = ~uint64_t{0};
};

struct TableKey {
  std::array<uint32_t, kTexSlots> t{};
  bool operator==(const TableKey& o) const { return t == o.t; }
};
struct TableHash {
  size_t operator()(const TableKey& k) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : k.t) h = (h ^ v) * 1099511628211ull;
    return static_cast<size_t>(h);
  }
};

struct FrameSlot {
  Com<ID3D12CommandAllocator> allocator;
  Com<ID3D12Resource> backBuffer;
  uint64_t fence = 0;
  std::vector<Page> pages;
  size_t page = 0, offset = 0;
  uint32_t descriptorsUsed = 0;
  std::unordered_map<TableKey, uint32_t, TableHash> tables;
  std::vector<Com<IUnknown>> garbage;
  std::vector<std::array<uint64_t, 3>> freedRanges;  // static buffers let go of: block, offset, size
  std::vector<const char*> marks;  // the stages whose times this frame's queries take
  // [mcrec] recording on several threads (Device::BeginParallel): the allocators of the frame's further lists (its own
  // list again after each parallel section, and the recorders'), taken in turn each frame; each recorder's pages of
  // upload memory and its texture sets (by the recorder's number)
  std::vector<Com<ID3D12CommandAllocator>> moreAllocators;
  size_t moreUsed = 0;
  struct RecorderRoom {
    std::vector<Page> pages;
    size_t page = 0, offset = 0;
    std::unordered_map<TableKey, uint32_t, TableHash> tables;
  };
  std::vector<RecorderRoom> recorders;
};

// Static buffers are cut from large blocks (making a resource of their own for each was most of a
// map's loading time: some hundred buffers of its props' meshes took 0.4 s), first fit.
struct BufferBlock {
  Com<ID3D12Resource> resource;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  uint64_t size = 0;
  std::vector<std::pair<uint64_t, uint64_t>> free;  // offset, size
};
// What uploads are copied from: pages of upload memory kept by each batch and used again once the card
// is done with it (instead of a resource made for each upload).
struct StagePage {
  Com<ID3D12Resource> resource;
  uint8_t* cpu = nullptr;
  uint64_t size = 0;
};

class D3D12Device final : public Device {
 public:
  ~D3D12Device() override;
  bool Init(const DeviceDesc& desc, std::string* error);

  const char* BackendName() const override { return "Direct3D 12"; }
  std::string AdapterName() const override { return adapterName_; }
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  void Resize(int width, int height) override;
  void SetVsync(bool on) override { vsync_ = on; }

  Buffer CreateBuffer(const BufferDesc& desc) override;
  void UpdateBuffer(Buffer buffer, const void* data, uint32_t size) override;
  void* WriteBuffer(Buffer buffer, uint32_t size) override;
  Texture CreateTexture(const TextureDesc& desc) override;
  void UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) override;
  Texture CreateTarget(const TargetDesc& desc) override;
  Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) override;
  Pipeline CreateCompute(const ComputeDesc& desc, std::string* error) override;
  void BeginPipelineBatch() override;
  bool EndPipelineBatch(std::string* error) override;
  void BeginBackgroundPipelines() override { backgroundAsked_ = dxil_ && !shaderCache_.empty(); }  // [snowhitch]
  void EndBackgroundPipelines() override { backgroundAsked_ = false; }
  int BackgroundPipelinesWaiting() override {  // [bgload]
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    return static_cast<int>(backgroundQueue_.size()) + backgroundMaking_;
  }
  bool PipelineReady(Pipeline pipeline) override {  // [bgload]
    const PipelineRec* rec = Find(pipeline);
    if (!rec || !rec->background) return true;
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    return rec->background->stage == BackgroundPipeline::Made || rec->background->stage == BackgroundPipeline::NotMade;
  }
  std::unique_ptr<Prepared> PrepareTexture(const TextureDesc& desc) override;  // [spikes]
  Texture FinishTexture(std::unique_ptr<Prepared> prepared) override;
  std::unique_ptr<Prepared> PrepareBuffer(const BufferDesc& desc) override;
  Buffer FinishBuffer(std::unique_ptr<Prepared> prepared) override;
  std::unique_ptr<Prepared> PrepareTarget(const TargetDesc& desc) override;  // [warmtarget]
  Texture FinishTarget(std::unique_ptr<Prepared> prepared) override;
  HRESULT TargetResource(const TargetDesc& desc, Com<ID3D12Resource>* out);  // (any thread)
  Texture MakeTarget(const TargetDesc& desc, Com<ID3D12Resource> made);  // (CreateTarget; `made`: its resource, PrepareTarget's)
  void Destroy(Buffer buffer) override;
  void Destroy(Texture texture) override;
  void Destroy(Pipeline pipeline) override;
  void Mark(const char* name) override;
  uint64_t MemoryInUse() override;
  bool MemoryBudget(uint64_t* budget, uint64_t* used) override;  // (photofix2)
  void TakeTimes(std::vector<std::pair<const char*, double>>* out) override;

  void BeginFrame(const float clearColor[4]) override;
  void BeginPass(const PassDesc& pass) override;
  void Resolve(Texture from, Texture to) override;
  void SetPipeline(Pipeline pipeline) override;
  void SetConstants(int slot, const void* data, uint32_t size) override;
  void SetTexture(int slot, Texture texture) override;
  void SetVertexBuffer(Buffer buffer) override;
  void SetIndexBuffer(Buffer buffer) override;
  void SetShaderBuffer(Buffer buffer, int slot) override;
  void SetStorage(int slot, Texture texture, int mip) override;
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  void Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) override;
  void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override;
  // [gpuscenery] card buffers and indirect draws (gfx.h)
  bool IndirectDraws() const override { return static_cast<bool>(drawIndexedSignature_); }
  void UpdateBufferRange(Buffer buffer, uint32_t offset, const void* data, uint32_t size) override;
  void DrawIndexedIndirect(Buffer arguments, uint32_t offset) override;
  void EndFrame(std::vector<uint8_t>* capture, bool present) override;
  bool RayTracing() const override { return rayTracing_; }
  Structure CreateGeometry(const GeometryPart* parts, int count) override;
  std::unique_ptr<Prepared> DescribeGeometry(const GeometryPart* parts, int count) override;  // [warmtarget]
  void SizeGeometry(Prepared* prepared) override;
  Structure FinishGeometry(std::unique_ptr<Prepared> prepared) override;
  void BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) override;
  bool RefitScene(Structure* scene, const SceneInstance* instances, uint32_t count, bool rebuild) override;  // [cardcost]
  bool MakeScene(Structure* scene, const SceneInstance* instances, uint32_t count, int how);                 // [cardcost]
  void SetScene(int slot, Structure scene) override;
  void Destroy(Structure structure) override;
  bool SetFrameGeneration(std::function<bool()> make) override;
  bool FrameGenerating() const override { return generating_; }
  Texture WindowPicture() override { return generating_ ? windowPictures_[cur_] : Texture{}; }
  Texture BetweenPicture() override { return generating_ ? betweenPictures_[cur_] : Texture{}; }
  bool SetPhotoWindow(int width, int height) override;
  bool SetLatencyLimiter(bool on) override;
  bool LatencyLimiter() const override { return latency_; }
  void WaitForNextFrame() override;
  // [mcrec] recording on several threads (gfx.h)
  int ParallelRecorders() const override { return kMostRecorders; }
  bool BeginParallel(int count) override;
  Recorder* Parallel(int index) override;
  void EndParallel() override;

 private:
  // [mcrec] Recording on several threads. The frame's commands are then several lists, handed to the card together in
  // their order at EndFrame (order_): the frame's own list up to a parallel section, each recorder's list, the frame's
  // own list again (one of moreLists_, set up as the last recorder left things), and so on. A recorder records into a list
  // of its own, with its own pages of upload memory for its constants and its own texture sets in the frame's share of
  // the shader-visible heap (taken from the share by `parallelDescriptors_`, as many recorders may take at once).
  static constexpr int kMostRecorders = 16;
  static constexpr size_t kRecorderPageSize = 2u << 20;
  // What the draws are given, as a list has it set now: the frame's own (the device's) or a recorder's.
  struct DrawSet {
    ID3D12PipelineState* pipeline = nullptr;  // (kept alive by the record or by Retire until the frame is done)
    bool compute = false;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D12_GPU_VIRTUAL_ADDRESS constants[kCbSlots] = {}, buffers[2] = {};
    TableKey textures;
    bool vertices = false, indices = false;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
  };
  class ParallelRecorder;
  std::vector<std::unique_ptr<ParallelRecorder>> recorders_;
  std::vector<Com<ID3D12GraphicsCommandList>> moreLists_;   // (lists may be reset once handed over: shared by the slots)
  std::vector<Com<ID3D12GraphicsCommandList4>> moreLists4_;  // (the same, for ray tracing's builds)
  size_t moreListsUsed_ = 0;
  Com<ID3D12GraphicsCommandList> firstList_;
  Com<ID3D12GraphicsCommandList4> firstList4_;
  std::vector<ID3D12CommandList*> order_;
  int parallel_ = 0;  // recorders handed out by BeginParallel (0: none)
  std::atomic<uint32_t> parallelDescriptors_{0};
  std::mutex parallelMutex_;  // (a pipeline not made yet that a recorder sets: made under it)
  std::atomic<bool> warnedParallel_{false};
  ID3D12GraphicsCommandList* afterList_ = nullptr;  // (the frame's own list after the section under way, made ready)
  ID3D12GraphicsCommandList4* afterList4_ = nullptr;
  D3D12_GPU_DESCRIPTOR_HANDLE RecorderTable(int index, const TableKey& key);
  // what the frame's own list has set besides the draws' state (the pass's targets and viewport), and what it has set of
  // the draws' (the pipeline, the vertex and index buffers), to set again on a list after a section
  UINT targetCount_ = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE targetRtv_{}, targetDsv_{};
  bool targetDepth_ = false;
  int viewportWidth_ = 0, viewportHeight_ = 0;
  DrawSet set_;  // (its pipeline, topology, vertex and index views; the rest is in constantsAt_, buffersAt_, boundTextures_)
  void SetTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
  bool NextList(ID3D12GraphicsCommandList** list, ID3D12GraphicsCommandList4** list4);
  void SetUp(ID3D12GraphicsCommandList* list, const DrawSet& s);
  Span AllocateIn(std::vector<Page>& pages, size_t& page, size_t& offset, size_t pageSize, size_t size, size_t align);

  // (photomode) SetPhotoWindow: the picture and the depth a frame draws its window into while it is on, their size;
  // binding them as the window's pass (BeginFrame, BeginPass)
  Texture photoColor_, photoDepth_;
  int photoWidth_ = 0, photoHeight_ = 0;
  bool photoOn_ = false;
  bool PhotoNow() { return photoOn_ && Find(photoColor_) && Find(photoDepth_); }
  void BindPhotoWindow(bool clearColor, const float clearValue[4], bool clearDepth);
  // the latency limiter (gfx.h, SetLatencyLimiter; gfx_latency.h): the switch (to wantLatency_), making the swap
  // chain anew; the swap chain's flags (the waitable object's only while it is on); the frames' work taken from the
  // slots the card has finished; the wait for a frame to begin (with the just-in-time wait or without), and with
  // frame generation, the wait for the presenter; the swap chain's frame statistics taken (the refreshes, what was
  // shown when, refreshes missed)
  void SwitchLatency();
  UINT ChainFlags(bool presenter) const;
  bool ChainWrongForVsync() const {  // (the limiter on, without frame generation: the chain made for the other vsync)
    return latency_ && !generating_ && ((chainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) != vsync_;
  }
  void TakeWork();
  double CardFree() const;
  void LimitFrames(bool justInTime);
  void WaitForPresenter();
  void TakeStatistics();
  double Deadline() const;
  // (refreshes a picture is shown for, with vsync)
  UINT Interval() const { return static_cast<UINT>(std::clamp(syncInterval, 1, 4)); }
  UINT LastPresent() const {  // (the number of the swap chain's last Present: the frame statistics' numbering)
    UINT n = 0;
    swapChain_->GetLastPresentCount(&n);
    return n;
  }
  // frame generation (gfx.h): the swap chain made on `queue`; each frame slot's window picture and the one made
  // between; the thread that shows them (Presenter), its start, its end (after what was handed to it), waiting
  // until it has shown all it was handed and the card has copied it; waiting until a slot's pictures are copied
  // (before the slot draws into them again); one picture copied into the swap chain and shown (the time just
  // before it is shown in `at`), the present fence's value once the card has copied it
  bool SwitchFrameGeneration();  // (to wantGenerating_)
  bool MakeSwapChain(ID3D12CommandQueue* queue, std::string* error);
  bool RemakeSwapChain(ID3D12CommandQueue* queue);
  bool MakeFramePictures();
  void DropFramePictures();
  void StartPresenter();
  void StopPresenter();
  void DrainPresenter();
  void Presenter();
  void WaitPresented(int slot);
  uint64_t Show(ID3D12Resource* picture, bool vsync, double* at);
  static void WaitOn(ID3D12Fence* fence, uint64_t value, HANDLE event);
  // ray tracing: a buffer on the card for a structure or a build's room; the scratch room grown to `size`; the
  // geometry made since the last build, built
  Com<ID3D12Resource> RayBuffer(uint64_t size, D3D12_RESOURCE_STATES state);
  bool GrowScratch(uint64_t size);
  void BuildGeometry();
  StructureRec* Find(Structure s) { return s.id && s.id <= structures_.size() && structures_[s.id - 1].used ? &structures_[s.id - 1] : nullptr; }
  // Direct3D's own compiler (FXC), the shader kept from an earlier run when its source has not changed
  bool CompileFxc(std::string_view source, const char* entry, const char* target, const char* name, Com<ID3DBlob>& out, std::string* error);
  bool CreateTargets();
  void WaitFence(uint64_t value);
  void WaitIdle();
  // Uploads (a new texture's or buffer's contents) go into one open batch, sent to the card ahead of
  // the next frame's own work (FlushUploads, from EndFrame), without the processor waiting for them:
  // a texture made in the middle of a game no longer stops everything until the card is idle. A
  // batch's staging copies are let go once the card is done with it. A batch that grows large
  // (a map loading) is sent at once, so they do not pile up.
  bool BeginUpload();
  void KeepStaging(Com<ID3D12Resource> staging, uint64_t bytes);
  // Room in the open upload batch's pages for `size` bytes at `align`: where to write them, and the page
  // and offset to copy from. Call BeginUpload first. Counts toward the batch's size (FlushUploads).
  bool Stage(uint64_t size, uint64_t align, uint8_t** cpu, ID3D12Resource** page, uint64_t* offset);
  // A static buffer's room in a block; and letting it go once the frames that use it are done.
  bool CutBlock(uint64_t size, int* block, uint64_t* offset);
  void FreeRange(int block, uint64_t offset, uint64_t size);
  void FlushUploads();
  Span Allocate(size_t size, size_t align);
  // The bound textures' table in this frame's share of the shader-visible heap (made once a frame for
  // each set); BindTextures gives it to the draws when the set has changed.
  D3D12_GPU_DESCRIPTOR_HANDLE TextureTable();
  void BindTextures();
  // The storage targets a dispatch wrote, readable again (before a pass, and at the frame's end).
  void ReleaseUnordered();
  D3D12_CPU_DESCRIPTOR_HANDLE UavHandle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = uavStart_;
    h.ptr += static_cast<SIZE_T>(index) * srvStep_;
    return h;
  }
  void Retire(IUnknown* object);
  uint32_t NewTextureIndex();
  void ReleasePassTargets();
  void SetViewport(int width, int height);
  D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvStart_;
    h.ptr += static_cast<SIZE_T>(index) * rtvStep_;
    return h;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = dsvStart_;
    h.ptr += static_cast<SIZE_T>(index) * dsvStep_;
    return h;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE StagingHandle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = stagingStart_;
    h.ptr += static_cast<SIZE_T>(index) * srvStep_;
    return h;
  }
  BufferRec* Find(Buffer b) { return b.id && b.id <= buffers_.size() && buffers_[b.id - 1].used ? &buffers_[b.id - 1] : nullptr; }
  TextureRec* Find(Texture t) { return t.id && t.id < textures_.size() && textures_[t.id].used ? &textures_[t.id] : nullptr; }
  PipelineRec* Find(Pipeline p) { return p.id && p.id <= pipelines_.size() && pipelines_[p.id - 1].used ? &pipelines_[p.id - 1] : nullptr; }

  HWND hwnd_ = nullptr;
  int width_ = 0, height_ = 0;
  bool vsync_ = true, tearing_ = false, inFrame_ = false;
  bool dxil_ = false;  // shaders compiled by DXC (CreatePipeline)
  double driverMs_ = 0;  // [hitches] the last MakePipeline's (MakeCompute's) time in the driver, and whether it was kept
  bool driverKept_ = false;
  std::string adapterName_;
  std::string shaderCache_;  // the folder compiled shaders are kept in (empty: none)
  bool batching_ = false;    // pipelines asked for are made at EndPipelineBatch
  std::string batchError_;   // (the first that could not be made, in a batch)
  bool MakePipeline(const PipelineDesc& desc, PipelineRec* out, std::string* error);
  bool MakeCompute(const ComputeDesc& desc, PipelineRec* out, std::string* error);
  // ([snowhitch] the two above, also on the device's own thread: `background` takes the shaders from the cache alone,
  // compiling there what it lacks with a compiler of its own; the driver's time and whether the pipeline was kept go out)
  bool MakePipelineWith(const PipelineDesc& desc, PipelineRec* out, std::string* error, bool background, double* driverMs, bool* kept);
  bool MakeComputeWith(const ComputeDesc& desc, PipelineRec* out, std::string* error, bool background, double* driverMs, bool* kept);
  Pipeline Keep(PipelineRec rec);
  bool Realize(uint32_t index, std::string* error);
  // [snowhitch] pipelines made on a thread of the device's own (BeginBackgroundPipelines): asked for, made there in turn,
  // taken into their slot the first time one is set (TakeBackground: waited for, or made then if its turn has not come)
  bool backgroundAsked_ = false;
  std::thread backgroundThread_;
  std::mutex backgroundMutex_;
  std::condition_variable backgroundWake_, backgroundDone_;
  std::deque<std::shared_ptr<BackgroundPipeline>> backgroundQueue_;
  int backgroundMaking_ = 0;  // [bgload] (the one being made now: 1)
  bool backgroundStop_ = false;
  std::shared_ptr<BackgroundPipeline> AskBackground(std::shared_ptr<PendingPipeline> what);
  void BackgroundWork();
  void TakeBackground(uint32_t index);

  Com<IDXGIFactory4> factory_;
  Com<IDXGIAdapter3> adapter3_;  // (what the system counts of the card's memory this program uses)
  Com<ID3D12Device> device_;
  D3D12PipelineStore pipelineStore_;  // [hitches] the driver's pipelines kept from run to run (gfx_d3d12_pipelines.h)
  Com<ID3D12InfoQueue> infoQueue_;
  Com<ID3D12CommandQueue> queue_;
  Com<IDXGISwapChain3> swapChain_;
  Com<ID3D12GraphicsCommandList> list_, uploadList_;
  static constexpr int kUploadBatches = 4;
  Com<ID3D12CommandAllocator> uploadAllocators_[kUploadBatches];
  uint64_t uploadFences_[kUploadBatches] = {};
  std::vector<Com<ID3D12Resource>> uploadStaging_[kUploadBatches];
  // [spikes] (BeginUpload) a batch's upload buffers of their own (KeepStaging: what was made ahead brings one each) let go of
  // on a thread of their own: letting go of a few dozen took the frame 5-17 ms
  std::mutex releaseMutex_;
  std::condition_variable releaseWake_;
  std::vector<Com<ID3D12Resource>> releasing_;
  std::vector<Com<IUnknown>> releasingAny_;  // [bgload] (a frame slot's garbage: LetGo below)
  bool releaseStop_ = false;
  std::thread releaser_;
  void LetGo(std::vector<Com<ID3D12Resource>>* staging);
  // [bgload] what a frame slot let go of (Retire), done with now, handed to the same thread: a world let go of leaves
  // a gigabyte and more there, which took the frame that found it 100-200 ms to let go of
  void LetGo(std::vector<Com<IUnknown>>* garbage);
  void StartReleaser();
  std::vector<StagePage> stagePages_[kUploadBatches];
  size_t stagePage_ = 0;
  uint64_t stageOffset_ = 0;
  std::vector<BufferBlock> blocks_;
  int uploadBatch_ = 0;
  bool uploadOpen_ = false;
  uint64_t uploadBytes_ = 0;
  Com<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  uint64_t fenceNext_ = 1;
  Com<ID3D12RootSignature> rootSignature_;
  Com<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_, stagingHeap_, ringHeap_;
  Com<ID3D12Resource> depth_, readback_;
  uint64_t readbackSize_ = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvStart_{}, dsvStart_{}, stagingStart_{}, ringCpuStart_{};
  D3D12_GPU_DESCRIPTOR_HANDLE ringGpuStart_{};
  UINT rtvStep_ = 0, dsvStep_ = 0, srvStep_ = 0;
  uint32_t nextRtv_ = kFrames, nextDsv_ = 1;  // views handed out so far (the window's come first)
  std::vector<uint32_t> freeRtv_;
  std::vector<std::pair<uint32_t, uint32_t>> freeDsv_;  // first view, count
  uint32_t passColor_ = 0, passDepth_ = 0;    // the current pass's targets (0: none, or the window's)

  // the stages' times: kMarks timestamp queries for each frame slot, read back once the slot's
  // frame is done
  static constexpr uint32_t kMarks = 64;
  Com<ID3D12QueryHeap> timeQueries_;
  Com<ID3D12Resource> timeReadback_;
  double ticksPerMs_ = 0;
  std::vector<std::pair<const char*, double>> lastTimes_;
  FrameSlot frames_[kFrames];
  int cur_ = 0;
  uint64_t frameNumber_ = 0;

  std::vector<BufferRec> buffers_;      // id = index + 1
  std::vector<TextureRec> textures_;    // id = index (= its descriptor in the staging heap); 0 is the null texture
  std::vector<PipelineRec> pipelines_;  // id = index + 1
  std::vector<uint32_t> freeBuffers_, freeTextures_, freePipelines_;

  TableKey boundTextures_;
  bool texturesDirty_ = true;
  bool warnedDescriptors_ = false;

  // compute: the storage views' heap (CPU-side), what the next dispatch binds, and the constants and
  // shader buffers set (kept, as compute has its own root arguments, given at each dispatch)
  Com<ID3D12DescriptorHeap> uavHeap_;
  D3D12_CPU_DESCRIPTOR_HANDLE uavStart_{};
  uint32_t nextUav_ = 1;
  std::vector<std::pair<uint32_t, uint32_t>> freeUav_;  // first view, count
  std::array<std::pair<uint32_t, int>, kStorageSlots> boundStorage_{};  // texture, level
  std::vector<uint32_t> unordered_;  // the textures in the unordered-access state
  D3D12_GPU_VIRTUAL_ADDRESS constantsAt_[kCbSlots] = {}, buffersAt_[2] = {};
  bool computePipeline_ = false;  // the pipeline set last is a compute one
  bool computeRoot_ = false;      // the compute root signature set this frame
  bool dispatched_ = false;       // the last thing recorded was a dispatch (the next waits for its writes)

  // [gpuscenery] card buffers (gfx.h, BufferKind::Card): those made (ids), indirect draws' signature (none: the card cannot);
  // a card buffer moved into `to` (barriers gathered into `out`, or recorded at once when null); and all of them made
  // readable again (before a pass, at the frame's end)
  std::vector<uint32_t> cardBuffers_;
  Com<ID3D12CommandSignature> drawIndexedSignature_;
  void CardState(BufferRec& rec, D3D12_RESOURCE_STATES to, std::vector<D3D12_RESOURCE_BARRIER>* out = nullptr);
  void CardsReadable();

  // ray tracing (DXR 1.1: inline ray queries with shader model 6.5)
  bool rayTracing_ = false;
  Com<ID3D12Device5> device5_;
  Com<ID3D12GraphicsCommandList4> list4_;  // (list_, as the interface that builds the structures)
  std::vector<StructureRec> structures_;   // id = index + 1
  std::vector<uint32_t> freeStructures_, pendingGeometry_;
  Com<ID3D12Resource> scratch_;
  uint64_t scratchSize_ = 0;

  // frame generation (gfx.h): while it is on, a frame slot is the frame's number (no longer the swap chain's
  // buffer, which the presenter alone draws into), and the swap chain is made on a queue of the presenter's
  // own, so that what it copies and shows is not held up behind the next frame's work on the frame's queue
  struct PresentJob {
    int slot = 0;
    uint64_t fence = 0;                 // the frame's work done on the card
    ID3D12Resource* between = nullptr;  // the picture made between (none: the frame alone)
    ID3D12Resource* frame = nullptr;
    bool vsync = true;
    double readAt = 0;                  // (measureLatency) when the frame's input was read
  };
  bool generating_ = false, wantGenerating_ = false;
  std::function<bool()> makeBetween_;
  Texture windowPictures_[kFrames], betweenPictures_[kFrames];
  bool handedOver_[kFrames] = {};  // the slot's two pictures left in the copy-source state at its frame's end
  Com<ID3D12CommandQueue> presentQueue_;
  Com<ID3D12Fence> presentFence_;  // signalled on the present queue after each picture it copies
  HANDLE presentEvent_ = nullptr;  // (the presenter thread's, for its own waits)
  static constexpr int kShowLists = 2 * kFrames;
  Com<ID3D12CommandAllocator> showAllocators_[kShowLists];
  uint64_t showFences_[kShowLists] = {};
  int showNext_ = 0;
  uint64_t presentNext_ = 1;
  Com<ID3D12GraphicsCommandList> showList_;
  std::thread presenter_;
  std::mutex presentMutex_;  // (over the jobs, presentStop_, slotShowing_ and slotCopied_)
  std::condition_variable presentWake_, presentDone_;
  std::deque<PresentJob> presentJobs_;
  bool presentStop_ = false;
  bool slotShowing_[kFrames] = {};     // handed to the presenter and not shown yet
  uint64_t slotCopied_[kFrames] = {};  // the present fence's value once the card has copied the slot's pictures
  Pacing pacing_;                      // (the presenter's)
  double lastShown_ = 0;               // (the presenter's: when it last showed a picture)

  // the latency limiter (SetLatencyLimiter): asked for, and on (the swap chain then made with its frame latency
  // waitable object, latencyWait_, at a frame latency of latencyAhead_); whether the frame to come has had its
  // waits already, on that object among them (until its Present uses it); when the last input read was
  // (WaitForNextFrame; 0: none since the last frame began), and the time the frame to come is to be done by and
  // whether its start was put off for it; each slot's frame: its input read, its hand-over to the card, that time
  // and whether, and whether the card's time for it was taken (two timestamps a slot, written only while the limiter
  // is on)
  bool wantLatency_ = false, latency_ = false;
  HANDLE latencyWait_ = nullptr;
  UINT chainFlags_ = 0;  // (the swap chain's, as it was made: ResizeBuffers keeps them)
  // (latencyDebt_: frames let ahead that the object is to take back, LimitFrames)
  int latencyAhead_ = 1, latencyDebt_ = 0;
  bool latencyTicket_ = false, latencyStalled_ = false;
  double readAt_ = 0, nextDeadline_ = 0;
  bool nextPaced_ = false;
  struct LatencySlot {
    double read = 0, handed = 0, deadline = 0;
    bool paced = false, timed = false;
  };
  LatencySlot latencySlots_[kFrames];
  Com<ID3D12QueryHeap> latencyQueries_;
  Com<ID3D12Resource> latencyTimes_;
  gfx::LatencyLimiter limiter_;
  Waiter latencyWaiter_;
  // the swap chain's frame statistics (the main thread's, or the presenter's while frame generation is on): the
  // refreshes, the newest picture shown and its refresh, the refresh's time for the main thread while the presenter
  // takes them; and the measure (measureLatency)
  RefreshGrid grid_;
  UINT statsPresent_ = 0, statsRefresh_ = 0;
  double statsShownAt_ = 0;  // (the time of the refresh the newest picture shown was first shown at)
  long statsAsked_ = 0, statsMissing_ = 0;  // (the measure's: the statistics asked for, and not given)
  uint64_t chainBase_ = 0;  // (added to a swap chain's picture numbers for the measure's: each new chain starts again)
  std::atomic<double> presenterPeriod_{0};
  LatencyRecord latencyRecord_;
  LatencyCost cost_;  // (the measure's: what the limiter's own work and waits took)
};

// [mcrec] Recording on several threads (gfx.h, BeginParallel). A recorder: the device's draw calls into a list of its own,
// with what they set kept in a DrawSet of its own (as the device keeps its own in constantsAt_, buffersAt_, boundTextures_
// and set_), its constants in pages of its own and its texture sets in the frame's share of the heap.
class D3D12Device::ParallelRecorder final : public Recorder {
 public:
  ParallelRecorder(D3D12Device* device, int index) : d_(device), index_(index) {}
  void Start(ID3D12GraphicsCommandList* list, const DrawSet& set) {
    list_ = list;
    set_ = set;
    dirty_ = true;
    draws_ = 0;
  }
  void SetPipeline(Pipeline pipeline) override {
    ID3D12PipelineState* state = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    {  // (one not made yet is made here, as the device's own SetPipeline makes it: the pipelines' records under the lock)
      std::lock_guard<std::mutex> lock(d_->parallelMutex_);
      PipelineRec* rec = d_->Find(pipeline);
      if (rec && rec->pending) {
        std::string why;
        if (!d_->Realize(pipeline.id - 1, &why) && d_->batchError_.empty()) d_->batchError_ = why;
        rec = d_->Find(pipeline);
      }
      if (rec && rec->background) {
        d_->TakeBackground(pipeline.id - 1);
        rec = d_->Find(pipeline);
      }
      if (!rec || !rec->state || rec->compute) return;
      state = rec->state.Get();
      topology = rec->topology;
    }
    list_->SetPipelineState(state);
    list_->IASetPrimitiveTopology(topology);
    set_.pipeline = state;
    set_.compute = false;
    set_.topology = topology;
  }
  void SetConstants(int slot, const void* data, uint32_t size) override {
    if (slot < 0 || slot >= kCbSlots || !size) return;
    const uint32_t padded = (size + 255u) & ~255u;
    FrameSlot::RecorderRoom& room = d_->frames_[d_->cur_].recorders[static_cast<size_t>(index_)];
    const Span s = d_->AllocateIn(room.pages, room.page, room.offset, kRecorderPageSize, padded, 256);
    if (!s.cpu) return;
    std::memcpy(s.cpu, data, size);
    list_->SetGraphicsRootConstantBufferView(static_cast<UINT>(slot), s.gpu);
    set_.constants[slot] = s.gpu;
  }
  void SetTexture(int slot, Texture texture) override {
    if (slot < 0 || slot >= kTexSlots) return;
    const uint32_t id = d_->Find(texture) ? texture.id : 0;
    if (set_.textures.t[static_cast<size_t>(slot)] == id) return;
    set_.textures.t[static_cast<size_t>(slot)] = id;
    dirty_ = true;
  }
  void SetVertexBuffer(Buffer buffer) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || rec->kind != BufferKind::Vertex || (rec->dynamic && rec->frame != d_->frameNumber_)) return;
    const D3D12_VERTEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride};
    list_->IASetVertexBuffers(0, 1, &view);
    set_.vertices = true;
    set_.vertexView = view;
  }
  void SetIndexBuffer(Buffer buffer) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != d_->frameNumber_)) return;
    const D3D12_INDEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    list_->IASetIndexBuffer(&view);
    set_.indices = true;
    set_.indexView = view;
  }
  void SetShaderBuffer(Buffer buffer, int slot) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || slot < 0 || slot > 1) return;
    if (rec->dynamic ? rec->kind != BufferKind::Shader || rec->frame != d_->frameNumber_ : !rec->gpu) return;
    list_->SetGraphicsRootShaderResourceView(static_cast<UINT>(kCbSlots + 1 + slot), rec->gpu);
    set_.buffers[slot] = rec->gpu;
  }
  void Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) override {
    ++draws_;
    Bind();
    list_->DrawInstanced(vertexCount, instances, firstVertex, 0);
  }
  void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override {
    ++draws_;
    Bind();
    list_->DrawIndexedInstanced(indexCount, instances, firstIndex, baseVertex, 0);
  }
  void DrawIndexedIndirect(Buffer arguments, uint32_t offset) override {  // [gpuscenery]
    const BufferRec* rec = d_->Find(arguments);
    if (!rec || !rec->card || !d_->drawIndexedSignature_ || (offset & 3u) || offset + 20 > rec->size) return;
    ++draws_;
    Bind();
    list_->ExecuteIndirect(d_->drawIndexedSignature_.Get(), 1, rec->card.Get(), offset, nullptr, 0);
  }

  ID3D12GraphicsCommandList* list_ = nullptr;
  DrawSet set_;
  int draws_ = 0;

 private:
  void Bind() {
    if (!dirty_) return;
    dirty_ = false;
    list_->SetGraphicsRootDescriptorTable(kCbSlots, d_->RecorderTable(index_, set_.textures));
  }
  D3D12Device* d_;
  int index_;
  bool dirty_ = true;
};

D3D12Device::~D3D12Device() {
  {  // [snowhitch] the thread making pipelines ahead: what it is making finished, the rest left
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    backgroundStop_ = true;
  }
  backgroundWake_.notify_all();
  if (backgroundThread_.joinable()) backgroundThread_.join();
  {  // [spikes] (the upload buffers' releaser: what it holds let go of first)
    std::lock_guard<std::mutex> lock(releaseMutex_);
    releaseStop_ = true;
  }
  releaseWake_.notify_all();
  if (releaser_.joinable()) releaser_.join();
  if (generating_) StopPresenter();
  if (queue_ && fence_) WaitIdle();
  pipelineStore_.Save();  // [hitches] (the pipelines made since the map opened, for the next run)
  if (measureLatency) {
    latencyRecord_.Print(latency_ ? "Direct3D 12 (low latency)" : "Direct3D 12", vsync_ ? grid_.Period() * Interval() : 0, grid_.Period());
    fprintf(stderr, "render: Direct3D 12: the frame statistics asked for %ld times, not given %ld\n", statsAsked_, statsMissing_);
    if (latency_) limiter_.Print("Direct3D 12"), cost_.Print("Direct3D 12");
  }
  if (latencyWait_) CloseHandle(latencyWait_);
  if (fenceEvent_) CloseHandle(fenceEvent_);
  if (presentEvent_) CloseHandle(presentEvent_);
}

bool D3D12Device::Init(const DeviceDesc& desc, std::string* error) {
  auto fail = [&](const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "Direct3D 12: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return false;
  };
  hwnd_ = static_cast<HWND>(desc.window);
  // where compiled shaders are kept (the user's local application data unless the host says)
  if (desc.shaderCache != "-") {
    std::string dir = desc.shaderCache;
    if (dir.empty()) {
      char local[MAX_PATH] = {};
      const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local);
      if (n > 0 && n < sizeof local) {
        dir = std::string(local) + "\\FA Remastered";
        CreateDirectoryA(dir.c_str(), nullptr);
        dir += "\\shaders";
      }
    }
    if (!dir.empty() && (CreateDirectoryA(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) shaderCache_ = dir;
  }
  width_ = std::max(1, desc.width);
  height_ = std::max(1, desc.height);
  vsync_ = desc.vsync;

  UINT factoryFlags = 0;
  if (desc.debug) {
    Com<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.Put())))) {
      debug->EnableDebugLayer();
      factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    } else {
      fprintf(stderr, "render: the Direct3D 12 debug layer is not installed; running without it\n");
    }
  }
  HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory_.Put()));
  if (FAILED(hr)) return fail("CreateDXGIFactory2", hr);

  // the fastest adapter that can do Direct3D 12 (a laptop or a CPU with graphics lists the slow one first)
  Com<IDXGIAdapter1> adapter;
  Com<IDXGIFactory6> factory6;
  if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(factory6.Put())))) {
    for (UINT i = 0;; ++i) {
      Com<IDXGIAdapter1> a;
      if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(a.Put())) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 d;
      a->GetDesc1(&d);
      if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
        adapter = a;
        break;
      }
    }
    BOOL allow = FALSE;
    if (SUCCEEDED(factory6->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearing_ = allow != FALSE;
  }
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.Put()));
  if (FAILED(hr)) return fail("D3D12CreateDevice", hr);
  if (adapter) adapter->QueryInterface(IID_PPV_ARGS(adapter3_.Put()));
  if (factoryFlags & DXGI_CREATE_FACTORY_DEBUG) device_->QueryInterface(IID_PPV_ARGS(infoQueue_.Put()));
  if (adapter) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    char name[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    adapterName_ = n > 0 ? name : "";
  }
  // (DXC's shaders are shader model 6.0: every card's Direct3D 12 driver of the last years takes it)
  D3D12_FEATURE_DATA_SHADER_MODEL model = {D3D_SHADER_MODEL_6_0};
  dxil_ = !desc.oldShaderCompiler && SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model)) &&
          model.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
  // (decided here for every shader: see DxcPresent)
  if (dxil_ && !DxcPresent()) {
    fprintf(stderr, "render: Direct3D 12: no dxcompiler.dll beside the program; the shaders compiled by Direct3D's own compiler\n");
    dxil_ = false;
  }

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.Put()));
  if (FAILED(hr)) return fail("CreateCommandQueue", hr);

  if (!MakeSwapChain(queue_.Get(), error)) return false;

  auto makeHeap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible, Com<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    return device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(out.Put()));
  };
  if (FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrames + kMaxColorTargets, false, rtvHeap_)) ||
      FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 + kMaxDepthViews, false, dsvHeap_)) ||
      FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxTextures, false, stagingHeap_)) ||
      FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRingDescriptors, true, ringHeap_)) ||
      FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxStorageViews, false, uavHeap_)))
    return fail("CreateDescriptorHeap", hr);
  uavStart_ = uavHeap_->GetCPUDescriptorHandleForHeapStart();
  rtvStart_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
  dsvStart_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
  stagingStart_ = stagingHeap_->GetCPUDescriptorHandleForHeapStart();
  ringCpuStart_ = ringHeap_->GetCPUDescriptorHandleForHeapStart();
  ringGpuStart_ = ringHeap_->GetGPUDescriptorHandleForHeapStart();
  rtvStep_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsvStep_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  srvStep_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // texture 0: the null texture every unused slot points at (reads as zero)
  textures_.resize(1);
  D3D12_SHADER_RESOURCE_VIEW_DESC nullView = {};
  nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nullView.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(nullptr, &nullView, StagingHandle(0));
  {  // and storage view 0 for every empty storage slot
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullStorage = {};
    nullStorage.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullStorage.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(nullptr, nullptr, &nullStorage, UavHandle(0));
  }

  for (FrameSlot& f : frames_) {
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(f.allocator.Put()));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
  }
  for (Com<ID3D12CommandAllocator>& a : uploadAllocators_) {
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.Put()));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
  }
  hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(list_.Put()));
  if (FAILED(hr)) return fail("CreateCommandList", hr);
  {  // ray tracing: DXR 1.1 (inline ray queries), with DXC's shader model 6.5
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    D3D12_FEATURE_DATA_SHADER_MODEL m65 = {D3D_SHADER_MODEL_6_5};
    rayTracing_ = dxil_ && SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)) &&
                  o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
                  SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &m65, sizeof m65)) &&
                  m65.HighestShaderModel >= D3D_SHADER_MODEL_6_5 && SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(device5_.Put()))) &&
                  SUCCEEDED(list_->QueryInterface(IID_PPV_ARGS(list4_.Put())));
  }
  list_->Close();
  firstList_ = list_;  // [mcrec]
  firstList4_ = list4_;
  hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocators_[0].Get(), nullptr, IID_PPV_ARGS(uploadList_.Put()));
  if (FAILED(hr)) return fail("CreateCommandList", hr);
  uploadList_->Close();
  hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Put()));
  if (FAILED(hr)) return fail("CreateFence", hr);
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  {  // the stages' times (none when the card cannot take them: the frames are drawn all the same)
    UINT64 frequency = 0;
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kFrames * kMarks;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(static_cast<UINT64>(kFrames) * kMarks * 8);
    if (SUCCEEDED(queue_->GetTimestampFrequency(&frequency)) && frequency &&
        SUCCEEDED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(timeQueries_.Put()))) &&
        SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(timeReadback_.Put()))))
      ticksPerMs_ = static_cast<double>(frequency) / 1000.0;
    else
      timeQueries_ = {};
  }

  // the one root signature
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = kTexSlots;
  D3D12_DESCRIPTOR_RANGE storageRange = {};  // (compute shaders' u0..u15)
  storageRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  storageRange.NumDescriptors = kStorageSlots;
  D3D12_ROOT_PARAMETER params[kCbSlots + 6] = {};
  for (int i = 0; i < kCbSlots; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[kCbSlots].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[kCbSlots].DescriptorTable.NumDescriptorRanges = 1;
  params[kCbSlots].DescriptorTable.pDescriptorRanges = &range;
  params[kCbSlots].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // the shader buffer: a root descriptor, so it needs no entry in the descriptor tables (t0, space1)
  params[kCbSlots + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[kCbSlots + 1].Descriptor.ShaderRegister = 0;
  params[kCbSlots + 1].Descriptor.RegisterSpace = 1;
  params[kCbSlots + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[kCbSlots + 2] = params[kCbSlots + 1];  // the second shader buffer (t1, space1)
  params[kCbSlots + 2].Descriptor.ShaderRegister = 1;
  params[kCbSlots + 3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[kCbSlots + 3].DescriptorTable.NumDescriptorRanges = 1;
  params[kCbSlots + 3].DescriptorTable.pDescriptorRanges = &storageRange;
  params[kCbSlots + 3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // [gpuscenery] the two shader buffers' slots written by compute shaders when they hold card buffers (u0, u1 of space1):
  // root descriptors too
  for (int i = 0; i < 2; ++i) {
    params[kCbSlots + 4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[kCbSlots + 4 + i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    params[kCbSlots + 4 + i].Descriptor.RegisterSpace = 1;
    params[kCbSlots + 4 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC samplers[6] = {};
  // s5: for shadow maps. Compares against the stored (reversed) depth: 1 where the given depth is
  // nearer to the light or equal; blends the four nearest results. Outside the map: lit.
  samplers[5].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[5].AddressU = samplers[5].AddressV = samplers[5].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  samplers[5].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  samplers[5].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
  samplers[5].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[5].ShaderRegister = 5;
  samplers[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  const struct {
    D3D12_FILTER filter;
    D3D12_TEXTURE_ADDRESS_MODE address;
  } kinds[5] = {{D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP},
                {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
                {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
                {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP},
                {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP}};
  for (int i = 0; i < 5; ++i) {
    samplers[i].Filter = kinds[i].filter;
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = kinds[i].address;
    samplers[i].MaxAnisotropy = 16;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = static_cast<UINT>(i);
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = kCbSlots + 6;
  rd.pParameters = params;
  rd.NumStaticSamplers = 6;
  rd.pStaticSamplers = samplers;
  rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Com<ID3DBlob> blob, errors;
  hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.Put(), errors.Put());
  if (FAILED(hr)) return fail("D3D12SerializeRootSignature", hr);
  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature_.Put()));
  if (FAILED(hr)) return fail("CreateRootSignature", hr);
  // [hitches] (beside the shaders; not under the validation layer, --debug, where a pipeline not kept yet would be
  // reported as an error by the library's "not found" answer)
  pipelineStore_.Open(device_.Get(), desc.debug ? std::string() : shaderCache_, blob->GetBufferPointer(), blob->GetBufferSize());
  {  // [gpuscenery] indirect draws: one indexed draw's five arguments (none where the driver will not: IndirectDraws false)
    D3D12_INDIRECT_ARGUMENT_DESC arg = {};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sd = {};
    sd.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    if (FAILED(device_->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(drawIndexedSignature_.Put())))) drawIndexedSignature_ = {};
  }

  if (!CreateTargets()) return fail("creating the back and depth buffers", E_FAIL);
  return true;
}

bool D3D12Device::CreateTargets() {
  for (int i = 0; i < kFrames; ++i) {
    HRESULT hr = swapChain_->GetBuffer(static_cast<UINT>(i), IID_PPV_ARGS(frames_[i].backBuffer.Put()));
    if (FAILED(hr)) return Complain("GetBuffer", hr), false;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvStart_;
    h.ptr += static_cast<SIZE_T>(i) * rtvStep_;
    device_->CreateRenderTargetView(frames_[i].backBuffer.Get(), nullptr, h);
  }
  D3D12_RESOURCE_DESC dd = {};
  dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  dd.Width = static_cast<UINT64>(width_);
  dd.Height = static_cast<UINT>(height_);
  dd.DepthOrArraySize = 1;
  dd.MipLevels = 1;
  dd.Format = kDepthFormat;
  dd.SampleDesc.Count = 1;
  dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = kDepthFormat;
  clear.DepthStencil.Depth = 0.0f;
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                IID_PPV_ARGS(depth_.Put()));
  if (FAILED(hr)) return Complain("creating the depth buffer", hr), false;
  device_->CreateDepthStencilView(depth_.Get(), nullptr, dsvStart_);
  return true;
}

bool D3D12Device::MakeSwapChain(ID3D12CommandQueue* queue, std::string* error) {
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = static_cast<UINT>(width_);
  sd.Height = static_cast<UINT>(height_);
  sd.Format = kBackFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kFrames;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  // (frame generation's: the presenter shows from it)
  const bool presenter = presentQueue_ && queue == presentQueue_.Get();
  sd.Flags = chainFlags_ = ChainFlags(presenter);
  Com<IDXGISwapChain1> sc1;
  HRESULT hr = factory_->CreateSwapChainForHwnd(queue, hwnd_, &sd, nullptr, nullptr, sc1.Put());
  if (SUCCEEDED(hr)) hr = sc1->QueryInterface(IID_PPV_ARGS(swapChain_.Put()));
  if (FAILED(hr)) {
    char buf[160];
    snprintf(buf, sizeof buf, "Direct3D 12: making the swap chain failed (0x%08lx)", static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return false;
  }
  factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  latencyAhead_ = 1;
  latencyDebt_ = 0;
  latencyTicket_ = latencyStalled_ = false;
  // the latency limiter: a frame latency of one, and the object that says when a frame may begin
  if (chainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
    swapChain_->SetMaximumFrameLatency(1);
    latencyWait_ = swapChain_->GetFrameLatencyWaitableObject();
  }
  // (a new swap chain counts its pictures from the start: the measure's numbers go on)
  statsPresent_ = statsRefresh_ = 0;
  chainBase_ += uint64_t{1} << 32;
  return true;
}

// The swap chain's flags: as they always were (tearing where the system allows it), and with the latency limiter on
// and vsync the frame latency waitable object's. Not without vsync (frames are then not held back for the screen, and
// a chain with the object ran slower there in most runs: 1.4 -> 1.8-3.0 ms a frame, light scene, busy machine; the
// limiter keeps only its wait for the card then), and not for frame generation's presenter: its chain stays as it
// always was (a frame latency of one there, waited for before each picture, cost the pictures' rate at 240 Hz and
// without vsync, as much as half of it on a busy machine; WaitForPresenter keeps the real frames waiting for the
// presenter few instead). The chain is made anew when vsync is switched (WaitForNextFrame, BeginFrame).
UINT D3D12Device::ChainFlags(bool presenter) const {
  const bool waitable = latency_ && vsync_ && !presenter;
  return (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u) | (waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u);
}

// (between frames, the card idle)
bool D3D12Device::RemakeSwapChain(ID3D12CommandQueue* queue) {
  for (FrameSlot& f : frames_) {
    f.backBuffer.Reset();
    f.garbage.clear();
  }
  depth_.Reset();
  if (latencyWait_) CloseHandle(latencyWait_);
  latencyWait_ = nullptr;
  swapChain_.Reset();  // (the window's old swap chain gone before another is made for it)
  std::string why;
  if (!MakeSwapChain(queue, &why)) {
    fprintf(stderr, "render: %s\n", why.c_str());
    return false;
  }
  return CreateTargets();
}

// The window's picture and the one made between, for each frame slot (the window's size; RGBA8, the swap chain's
// format, so that either is copied into it as it is).
bool D3D12Device::MakeFramePictures() {
  for (int i = 0; i < kFrames; ++i) {
    TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = Format::RGBA8;
    const float worldClear[4] = {0.05f, 0.06f, 0.08f, 1};  // (what a world's frame begins with: World's hosts')
    std::copy(worldClear, worldClear + 4, td.clearValue);
    windowPictures_[i] = CreateTarget(td);
    td.storage = true;
    betweenPictures_[i] = CreateTarget(td);
    handedOver_[i] = false;
    if (!windowPictures_[i] || !betweenPictures_[i]) return false;
  }
  return true;
}

void D3D12Device::DropFramePictures() {
  for (int i = 0; i < kFrames; ++i) {
    Destroy(windowPictures_[i]);
    Destroy(betweenPictures_[i]);
    windowPictures_[i] = betweenPictures_[i] = {};
    handedOver_[i] = false;
  }
}

// The function at a frame's end replaced at once (a world that goes away is never called after); the window's own
// pictures and the swap chain switched now when between frames, else at the next BeginFrame.
bool D3D12Device::SetFrameGeneration(std::function<bool()> make) {
  wantGenerating_ = static_cast<bool>(make);
  makeBetween_ = std::move(make);
  if (inFrame_ || wantGenerating_ == generating_) return true;
  return SwitchFrameGeneration();
}

bool D3D12Device::SwitchFrameGeneration() {
  const bool on = wantGenerating_;
  if (on && !presentQueue_) {  // the presenter's queue, fence, event and command lists, the first time
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool made = SUCCEEDED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(presentQueue_.Put()))) &&
                SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(presentFence_.Put())));
    for (auto& a : showAllocators_)
      made = made && SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.Put())));
    made = made && SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, showAllocators_[0].Get(), nullptr, IID_PPV_ARGS(showList_.Put())));
    if (made) showList_->Close();
    if (made) presentEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!made || !presentEvent_) {
      fprintf(stderr, "render: Direct3D 12: frame generation's queue could not be made\n");
      presentQueue_ = {};
      wantGenerating_ = false;
      return false;
    }
  }
  if (generating_) StopPresenter();
  WaitIdle();
  if (on) {
    if (!MakeFramePictures() || !RemakeSwapChain(presentQueue_.Get())) {
      fprintf(stderr, "render: Direct3D 12: frame generation could not be switched on\n");
      DropFramePictures();
      RemakeSwapChain(queue_.Get());
      wantGenerating_ = false;
      return false;
    }
    generating_ = true;
    pacing_.Reset();
    lastShown_ = 0;
    StartPresenter();
  } else {
    generating_ = false;
    DropFramePictures();
    RemakeSwapChain(queue_.Get());
  }
  return true;
}

// The latency limiter switched now when between frames, else at the next WaitForNextFrame or BeginFrame.
bool D3D12Device::SetLatencyLimiter(bool on) {
  wantLatency_ = on;
  if (!inFrame_ && wantLatency_ != latency_) SwitchLatency();
  return inFrame_ || latency_ == on;  // (false: the swap chain could not be made with the waitable object)
}

// The swap chain made anew, with the frame latency waitable object or without it (as it always was: ChainFlags), the
// card idle; while frame generation is on its presenter's chain is the same either way and stays (the presenter
// stopped and started again around the switch).
void D3D12Device::SwitchLatency() {
  if (wantLatency_ && !latencyQueries_ && ticksPerMs_ > 0) {  // the slots' timestamps, the first time
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kFrames;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(16 * kFrames);
    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(latencyQueries_.Put()))) ||
        FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(latencyTimes_.Put()))))
      latencyQueries_ = {}, latencyTimes_ = {};
  }
  if (generating_) StopPresenter();
  WaitIdle();
  latency_ = wantLatency_;
  // (only where its flags change: without vsync the chain is the same either way, and a chain made anew without vsync
  // ran at times far slower than the first one made for the window: 1.4 -> 4.6 ms a frame, the light scene)
  if (!generating_ && ChainFlags(false) != chainFlags_ && !RemakeSwapChain(queue_.Get()) && latency_) {
    fprintf(stderr, "render: Direct3D 12: the latency limiter could not be switched on\n");
    latency_ = wantLatency_ = false;
    RemakeSwapChain(queue_.Get());
  }
  limiter_ = gfx::LatencyLimiter{};
  for (LatencySlot& s : latencySlots_) s = LatencySlot{};
  readAt_ = nextDeadline_ = 0;
  nextPaced_ = false;
  if (generating_) {
    pacing_.Reset();
    lastShown_ = 0;
    StartPresenter();
  }
}

// Called by the host just before it reads its input: with the limiter on, the waits that keep the frames waiting to
// be shown to one (or two, gfx_latency.h), then the wait that begins the frame just in time; with it off, nothing
// (the moment is only noted for the measure).
void D3D12Device::WaitForNextFrame() {
  if (inFrame_) return;
  if (wantLatency_ != latency_ || ChainWrongForVsync()) SwitchLatency();
  if (latency_ && !PhotoNow()) {  // (a photo's frames are never shown: nothing to wait for, photo.h)
    if (generating_) WaitForPresenter();
    else LimitFrames(true);
  }
  if (latency_ || measureLatency) readAt_ = Now();
}

// The frames the card has finished, for the limiter, in the order they were handed over: their input read, their
// hand-over, the card's first and last timestamps in the processor's time (by the queue's clock calibration) and its
// time for them, and the time each was to be done by.
void D3D12Device::TakeWork() {
  if (!latencyTimes_) return;
  const uint64_t done = fence_->GetCompletedValue();
  int order[kFrames], n = 0;
  for (int i = 0; i < kFrames; ++i)
    if (latencySlots_[i].timed && done >= frames_[i].fence) order[n++] = i;
  if (n == 0) return;
  std::sort(order, order + n, [&](int a, int b) { return frames_[a].fence < frames_[b].fence; });
  UINT64 calibratedCard = 0, calibratedQpc = 0;
  static const double qpcPerSecond = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  const bool calibrated = SUCCEEDED(queue_->GetClockCalibration(&calibratedCard, &calibratedQpc));
  const double ticksPerSecond = ticksPerMs_ * 1000;
  // (a timestamp in the processor's time: the calibration's, and the card's ticks from the calibration to it)
  auto cpuTime = [&](uint64_t t) {
    return calibrated ? static_cast<double>(calibratedQpc) / qpcPerSecond + (static_cast<double>(t) - static_cast<double>(calibratedCard)) / ticksPerSecond : 0.0;
  };
  for (int k = 0; k < n; ++k) {
    LatencySlot& s = latencySlots_[order[k]];
    s.timed = false;
    void* mapped = nullptr;
    const SIZE_T at = static_cast<SIZE_T>(order[k]) * 16;
    D3D12_RANGE range = {at, at + 16};
    if (FAILED(latencyTimes_->Map(0, &range, &mapped))) continue;
    uint64_t t[2];
    std::memcpy(t, static_cast<const uint8_t*>(mapped) + at, sizeof t);
    D3D12_RANGE none = {0, 0};
    latencyTimes_->Unmap(0, &none);
    LatencyFrame f;
    f.read = s.read, f.handed = s.handed, f.deadline = s.deadline, f.paced = s.paced;
    if (t[1] > t[0]) {
      f.card = static_cast<double>(t[1] - t[0]) / ticksPerSecond;
      f.cardBegin = cpuTime(t[0]), f.cardEnd = cpuTime(t[1]);
    }
    limiter_.Work(f);
  }
}

// When the card will be done with the frames it was given (0: not known): the newest finished frame's end, then for
// each frame not finished yet, in the order handed over, the card's least time for a frame from its hand-over or the
// end of the one before, whichever is later.
double D3D12Device::CardFree() const {
  double free = limiter_.LastCardEnd();
  if (free <= 0) return 0;
  int order[kFrames], n = 0;
  for (int i = 0; i < kFrames; ++i)
    if (latencySlots_[i].timed) order[n++] = i;
  std::sort(order, order + n, [&](int a, int b) { return frames_[a].fence < frames_[b].fence; });
  for (int k = 0; k < n; ++k) free = std::max(free, latencySlots_[order[k]].handed) + limiter_.Card();
  return free;
}

// Without frame generation: the frame latency (one, or two where frames do not fit into a refresh one after the
// other), the wait on the swap chain's object until a frame may begin (once a frame: the host may call this more than
// once, and BeginFrame waits itself when it did not), and then the wait that begins it as late as the frames before
// allow (gfx_latency.h): for the refresh it will be shown at, and until the card is about to be free of the frames
// before. Without vsync the chain has no such object (ChainFlags): only the wait for the card is left.
void D3D12Device::LimitFrames(bool justInTime) {
  const double t0 = Now();
  TakeWork();
  const double t1 = Now();
  limiter_.Pace(vsync_, grid_.Period() * Interval());
  // (the object counts the frames that may still begin, as a semaphore: a higher frame latency adds to its count at
  // once, a lower one takes nothing away (as Wine's notes on it say too), so what was let ahead before is taken back
  // here (latencyDebt_), as it comes free, by waits that do not wait)
  if (latencyWait_ && limiter_.Ahead() != latencyAhead_ && SUCCEEDED(swapChain_->SetMaximumFrameLatency(static_cast<UINT>(limiter_.Ahead())))) {
    latencyDebt_ = limiter_.Ahead() < latencyAhead_ ? latencyDebt_ + latencyAhead_ - limiter_.Ahead() : 0;
    latencyAhead_ = limiter_.Ahead();
  }
  if (!latencyTicket_ && latencyWait_) {
    // (never for good: a window hidden or minimised shows nothing, and its object may not come free; after a wait that
    // ran out, the object is only looked at until it comes free again, so such a window's frames are not held up; the
    // frame that did not wait is taken back as above)
    latencyStalled_ = WaitForSingleObjectEx(latencyWait_, latencyStalled_ ? 0 : 100, FALSE) == WAIT_TIMEOUT;
    if (latencyStalled_) latencyDebt_ = std::min(latencyDebt_ + 1, kFrames);  // (never more than the swap chain holds)
    else
      while (latencyDebt_ > 0 && WaitForSingleObjectEx(latencyWait_, 0, FALSE) == WAIT_OBJECT_0) --latencyDebt_;
  }
  latencyTicket_ = true;  // (this frame's waits done: BeginFrame does not wait again)
  const double t2 = Now();
  TakeStatistics();
  limiter_.Pace(vsync_, grid_.Period() * Interval());
  nextDeadline_ = vsync_ ? Deadline() : 0;
  nextPaced_ = false;
  const double now = Now();
  const double at = justInTime ? limiter_.StartAt(now, nextDeadline_, CardFree()) : now;
  nextPaced_ = justInTime && limiter_.Paced();
  cost_.Add(t1 - t0, now - t2, t2 - t1, at - now);
  if (!justInTime) return;
  if (at > now) latencyWaiter_.Until(at);
}

// When the frame to come must be done (0: not known): the refresh it will be first shown at, as the swap chain's
// statistics tell it (the newest picture shown and the refresh it was first shown at, then an interval of refreshes
// for each picture handed over since and one for this frame), less one refresh: the system's compositor takes a
// window's picture at the refresh before the one it shows it at (measured: the frame latency object comes free just
// after a refresh, with the picture handed over last still a whole interval from being shown). A window the
// compositor leaves out (one that fills the screen) is shown a refresh sooner; the deadline is then a refresh early,
// which costs that refresh but misses none.
double D3D12Device::Deadline() const {
  const double period = grid_.Period();
  if (!statsPresent_ || period <= 0) return 0;
  const UINT queued = LastPresent() - statsPresent_;  // (handed over and not yet shown)
  if (queued > 4) return 0;  // (the statistics behind: a window not on the screen)
  return statsShownAt_ + (static_cast<double>((queued + 1) * Interval()) - 1) * period;
}

// With frame generation (the presenter shows the frames): the real frames handed to the presenter and not yet shown
// kept to one, or to none where the screen sets the pace (vsync) and a frame's work fits well within a refresh:
// the frame then begins as the frame before is shown, and is ready for its own first showing a refresh later. The
// presenter and its swap chain are as without the limiter (ChainFlags), and there is no just-in-time wait beyond
// this: the pictures' pacing is the presenter's (docs/render-optimisation.md, section 9).
void D3D12Device::WaitForPresenter() {
  const double period = presenterPeriod_.load();
  limiter_.Pace(vsync_, period);
  TakeWork();
  if (limiter_.SteppedBack()) return;  // (the way without the limiter, where frames came faster so: gfx_latency.h, Keep)
  const bool none = vsync_ && period > 0 && limiter_.Predicted() + limiter_.Margin() < period;
  std::unique_lock<std::mutex> lock(presentMutex_);
  presentDone_.wait_for(lock, std::chrono::milliseconds(250), [&] {
    int waiting = 0;
    for (bool b : slotShowing_) waiting += b ? 1 : 0;
    return waiting <= (none ? 0 : 1);
  });
}

// The swap chain's frame statistics, when it says something new: the refreshes (their period and the last one's
// time), the newest picture shown and the refresh it was first shown at (for the measure), and whether a refresh was
// missed since the picture seen before (more refreshes than pictures, with vsync: for the limiter's margin).
void D3D12Device::TakeStatistics() {
  DXGI_FRAME_STATISTICS st = {};
  ++statsAsked_;
  if (!swapChain_ || FAILED(swapChain_->GetFrameStatistics(&st)) || !st.SyncRefreshCount) {
    ++statsMissing_;  // (none while the window is not shown by the compositor: hidden, covered, minimised)
    return;
  }
  static const double qpcPerSecond = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  // (Now()'s clock: steady_clock is QPC's)
  const double sync = static_cast<double>(st.SyncQPCTime.QuadPart) / qpcPerSecond;
  grid_.Refresh(st.SyncRefreshCount, sync);
  // (the time between pictures with vsync: the refresh's, or its multiple with an interval; the presenter's thread,
  // while frame generation is on: the main thread's limiter takes it from there)
  if (generating_) presenterPeriod_.store(grid_.Period() * Interval());
  if (st.PresentCount <= statsPresent_ || grid_.Period() <= 0) return;
  const double shown = sync - static_cast<double>(st.SyncRefreshCount - st.PresentRefreshCount) * grid_.Period();
  if (!generating_ && latency_ && vsync_ && statsPresent_ && st.PresentRefreshCount >= statsRefresh_) {
    const bool late = st.PresentRefreshCount - statsRefresh_ > (st.PresentCount - statsPresent_) * Interval();
    limiter_.Shown(late, true);
  }
  statsPresent_ = st.PresentCount;
  statsRefresh_ = st.PresentRefreshCount;
  statsShownAt_ = shown;
  if (measureLatency) latencyRecord_.Shown(chainBase_ + st.PresentCount, shown);
}

void D3D12Device::StartPresenter() {
  presentStop_ = false;
  for (int i = 0; i < kFrames; ++i) slotShowing_[i] = false;
  presenter_ = std::thread([this] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // (AMD's presenter's priority)
    Presenter();
  });
}

void D3D12Device::StopPresenter() {
  {
    std::lock_guard<std::mutex> lock(presentMutex_);
    presentStop_ = true;
  }
  presentWake_.notify_all();
  if (presenter_.joinable()) presenter_.join();
  presentStop_ = false;
  for (uint64_t v : slotCopied_) WaitOn(presentFence_.Get(), v, fenceEvent_);
}

void D3D12Device::DrainPresenter() {
  {
    std::unique_lock<std::mutex> lock(presentMutex_);
    presentDone_.wait(lock, [&] { return presentJobs_.empty() && std::none_of(std::begin(slotShowing_), std::end(slotShowing_), [](bool b) { return b; }); });
  }
  for (uint64_t v : slotCopied_) WaitOn(presentFence_.Get(), v, fenceEvent_);
}

void D3D12Device::WaitPresented(int slot) {
  uint64_t copied = 0;
  {
    std::unique_lock<std::mutex> lock(presentMutex_);
    presentDone_.wait(lock, [&] { return !slotShowing_[slot]; });
    copied = slotCopied_[slot];
  }
  WaitOn(presentFence_.Get(), copied, fenceEvent_);
}

void D3D12Device::WaitOn(ID3D12Fence* fence, uint64_t value, HANDLE event) {
  if (!fence || fence->GetCompletedValue() >= value) return;
  fence->SetEventOnCompletion(value, event);
  WaitForSingleObject(event, INFINITE);
}

// The presenter's thread: for each frame handed to it, once the card has finished it, the picture made between
// and then the frame, each a pacing's spacing after the picture shown before it.
void D3D12Device::Presenter() {
  Waiter waiter;
  ShownRecord record;
  for (;;) {
    PresentJob job;
    {
      std::unique_lock<std::mutex> lock(presentMutex_);
      presentWake_.wait(lock, [&] { return presentStop_ || !presentJobs_.empty(); });
      if (presentJobs_.empty()) {  // (stopped, and all handed to it shown)
        record.Print("Direct3D 12");
        return;
      }
      job = presentJobs_.front();
      presentJobs_.pop_front();
    }
    WaitOn(fence_.Get(), job.fence, presentEvent_);
    const double spacing = pacing_.Ready(Now());
    uint64_t copied = 0;
    if (job.between) {
      waiter.Until(lastShown_ + spacing);
      copied = Show(job.between, job.vsync, &lastShown_);
      record.Shown(lastShown_, true);
      if (measureLatency || latency_) TakeStatistics();
    }
    waiter.Until(lastShown_ + spacing);
    copied = Show(job.frame, job.vsync, &lastShown_);
    record.Shown(lastShown_, false);
    if (measureLatency) latencyRecord_.Presented(chainBase_ + LastPresent(), job.readAt);
    if (measureLatency || latency_) TakeStatistics();
    {
      std::lock_guard<std::mutex> lock(presentMutex_);
      slotShowing_[job.slot] = false;
      slotCopied_[job.slot] = copied;
    }
    presentDone_.notify_all();
  }
}

uint64_t D3D12Device::Show(ID3D12Resource* picture, bool vsync, double* at) {
  const int a = showNext_;
  showNext_ = (showNext_ + 1) % kShowLists;
  WaitOn(presentFence_.Get(), showFences_[a], presentEvent_);  // (the card done with that list's last copy)
  showAllocators_[a]->Reset();
  showList_->Reset(showAllocators_[a].Get(), nullptr);
  ID3D12Resource* back = frames_[swapChain_->GetCurrentBackBufferIndex()].backBuffer.Get();
  D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
  showList_->ResourceBarrier(1, &b);
  showList_->CopyResource(back, picture);
  b = Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  showList_->ResourceBarrier(1, &b);
  showList_->Close();
  ID3D12CommandList* lists[] = {showList_.Get()};
  presentQueue_->ExecuteCommandLists(1, lists);
  *at = Now();  // (as AMD's: the time just before it is shown, not after Present returns, which may wait)
  HRESULT hr = swapChain_->Present(vsync ? Interval() : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (FAILED(hr)) Complain("Present", hr);
  showFences_[a] = presentNext_++;
  presentQueue_->Signal(presentFence_.Get(), showFences_[a]);
  return showFences_[a];
}

void D3D12Device::WaitFence(uint64_t value) {
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fenceEvent_);
  WaitForSingleObject(fenceEvent_, INFINITE);
}

void D3D12Device::WaitIdle() {
  FlushUploads();
  uint64_t v = fenceNext_++;
  queue_->Signal(fence_.Get(), v);
  WaitFence(v);
}

void D3D12Device::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  if (generating_) DrainPresenter();
  WaitIdle();
  if (generating_) DropFramePictures();  // (let go of at once below: the card is idle)
  for (FrameSlot& f : frames_) {
    f.backBuffer.Reset();
    f.garbage.clear();
    for (const auto& r : f.freedRanges) FreeRange(static_cast<int>(r[0]), r[1], r[2]);
    f.freedRanges.clear();
  }
  depth_.Reset();
  width_ = width;
  height_ = height;
  HRESULT hr = swapChain_->ResizeBuffers(kFrames, static_cast<UINT>(width), static_cast<UINT>(height), kBackFormat, chainFlags_);
  if (FAILED(hr)) Complain("ResizeBuffers", hr);
  if (chainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) swapChain_->SetMaximumFrameLatency(static_cast<UINT>(latencyAhead_));
  CreateTargets();
  if (generating_ && !MakeFramePictures()) {  // (frame generation off: the swap chain back on the frame's queue)
    fprintf(stderr, "render: Direct3D 12: frame generation's pictures could not be made; it is off\n");
    StopPresenter();
    DropFramePictures();
    generating_ = wantGenerating_ = false;
    RemakeSwapChain(queue_.Get());
  }
}

bool D3D12Device::BeginUpload() {
  if (uploadOpen_) return true;
  // (the batch's allocator and staging copies are free again once the card is done with its last use)
  const int k = uploadBatch_;
  const auto t0 = std::chrono::steady_clock::now();  // [spikes] (a batch reopened that took long: its parts, in the hitch log)
  WaitFence(uploadFences_[k]);
  const auto t1 = std::chrono::steady_clock::now();
  // (its pages free again: two kept for the next uploads, the rest let go of; a map's loading may take many. [spikes] Those
  // let go of, with the batch's upload buffers of their own, on the releaser's thread: was uploadStaging_[k].clear() and
  // the pages' resize here, 5-90 ms in a frame)
  for (size_t i = 2; i < stagePages_[k].size(); ++i) uploadStaging_[k].push_back(std::move(stagePages_[k][i].resource));
  if (stagePages_[k].size() > 2) stagePages_[k].resize(2);
  const size_t letGo = uploadStaging_[k].size();
  LetGo(&uploadStaging_[k]);
  stagePage_ = 0;
  stageOffset_ = 0;
  const auto t2 = std::chrono::steady_clock::now();
  if (FAILED(uploadAllocators_[k]->Reset()) || FAILED(uploadList_->Reset(uploadAllocators_[k].Get(), nullptr))) return false;
  if (HitchLog::on) {
    const auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    if (ms(t0, t3) > 1) {
      char text[200];
      snprintf(text, sizeof text, "(the upload batch reopened: the card waited for %.1f ms, %zu upload buffers handed on in %.1f ms, its list reset in %.1f ms)",
               ms(t0, t1), letGo, ms(t1, t2), ms(t2, t3));
      HitchLog::Add(text, ms(t0, t3), 9);
    }
  }
  uploadOpen_ = true;
  uploadBytes_ = 0;
  return true;
}

// [spikes] (BeginUpload: the card is done with them) handed to the releaser's thread, started the first time
void D3D12Device::LetGo(std::vector<Com<ID3D12Resource>>* staging) {
  if (staging->empty()) return;
  {
    std::lock_guard<std::mutex> lock(releaseMutex_);
    for (Com<ID3D12Resource>& r : *staging) releasing_.push_back(std::move(r));
    StartReleaser();
  }
  staging->clear();
  releaseWake_.notify_all();
}

// [bgload] (BeginFrame: the slot's frame is done on the card)
void D3D12Device::LetGo(std::vector<Com<IUnknown>>* garbage) {
  if (garbage->empty()) return;
  {
    std::lock_guard<std::mutex> lock(releaseMutex_);
    for (Com<IUnknown>& r : *garbage) releasingAny_.push_back(std::move(r));
    StartReleaser();
  }
  garbage->clear();
  releaseWake_.notify_all();
}

// (under releaseMutex_) the releaser's thread, started the first time
void D3D12Device::StartReleaser() {
  if (releaser_.joinable()) return;
  releaser_ = std::thread([this] {
    std::unique_lock<std::mutex> lock(releaseMutex_);
    for (;;) {
      releaseWake_.wait(lock, [&] { return releaseStop_ || !releasing_.empty() || !releasingAny_.empty(); });
      std::vector<Com<ID3D12Resource>> now;
      std::vector<Com<IUnknown>> nowAny;  // [bgload]
      now.swap(releasing_);
      nowAny.swap(releasingAny_);
      lock.unlock();
      now.clear();
      nowAny.clear();
      lock.lock();
      if (releaseStop_ && releasing_.empty() && releasingAny_.empty()) return;
    }
  });
}

void D3D12Device::KeepStaging(Com<ID3D12Resource> staging, uint64_t bytes) {
  uploadStaging_[uploadBatch_].push_back(std::move(staging));
  uploadBytes_ += bytes;
  if (uploadBytes_ > (256ull << 20)) FlushUploads();  // (a map loading: sent on, so copies do not pile up)
}

bool D3D12Device::Stage(uint64_t size, uint64_t align, uint8_t** cpu, ID3D12Resource** page, uint64_t* offset) {
  std::vector<StagePage>& pages = stagePages_[uploadBatch_];
  for (;;) {
    if (stagePage_ < pages.size()) {
      const uint64_t at = (stageOffset_ + align - 1) / align * align;
      if (at + size <= pages[stagePage_].size) {
        stageOffset_ = at + size;
        *cpu = pages[stagePage_].cpu + at;
        *page = pages[stagePage_].resource.Get();
        *offset = at;
        uploadBytes_ += size;
        return true;
      }
      ++stagePage_;
      stageOffset_ = 0;
      continue;
    }
    StagePage p;
    p.size = std::max<uint64_t>(32ull << 20, (size + 65535) / 65536 * 65536);
    D3D12_HEAP_PROPERTIES up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(p.size);
    HRESULT hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(p.resource.Put()));
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) hr = p.resource->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
      Complain("allocating an upload page", hr);
      return false;
    }
    p.cpu = static_cast<uint8_t*>(mapped);
    pages.push_back(std::move(p));
    stagePage_ = pages.size() - 1;
    stageOffset_ = 0;
  }
}

bool D3D12Device::CutBlock(uint64_t size, int* block, uint64_t* offset) {
  const uint64_t align = 256;
  for (size_t i = 0; i < blocks_.size(); ++i) {
    std::vector<std::pair<uint64_t, uint64_t>>& free = blocks_[i].free;
    for (size_t k = 0; k < free.size(); ++k) {
      const uint64_t at = (free[k].first + align - 1) / align * align;
      const uint64_t end = free[k].first + free[k].second;
      if (at + size > end) continue;
      const uint64_t start = free[k].first, before = at - start, after = end - (at + size);
      free.erase(free.begin() + static_cast<std::ptrdiff_t>(k));
      if (after) free.push_back({at + size, after});
      if (before) free.push_back({start, before});
      *block = static_cast<int>(i);
      *offset = at;
      return true;
    }
  }
  BufferBlock b;
  b.size = std::max<uint64_t>(64ull << 20, (size + 65535) / 65536 * 65536);
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC rd = BufferResourceDesc(b.size);
  // (buffers start in the common state and are promoted by their first use: the copy, later the draw)
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(b.resource.Put()));
  if (FAILED(hr)) {
    Complain("creating a block of buffers", hr);
    return false;
  }
  b.gpu = b.resource->GetGPUVirtualAddress();
  b.free.push_back({0, b.size});
  blocks_.push_back(std::move(b));
  return CutBlock(size, block, offset);
}

void D3D12Device::FreeRange(int block, uint64_t offset, uint64_t size) {
  if (block < 0 || static_cast<size_t>(block) >= blocks_.size()) return;
  std::vector<std::pair<uint64_t, uint64_t>>& free = blocks_[static_cast<size_t>(block)].free;
  free.push_back({offset, size});
  // (neighbours joined again, so that the block does not crumble)
  std::sort(free.begin(), free.end());
  std::vector<std::pair<uint64_t, uint64_t>> joined;
  for (const auto& r : free) {
    if (!joined.empty() && joined.back().first + joined.back().second == r.first) joined.back().second += r.second;
    else joined.push_back(r);
  }
  free.swap(joined);
}

void D3D12Device::FlushUploads() {
  if (!uploadOpen_) return;
  const auto t0 = std::chrono::steady_clock::now();  // [spikes] (one that took long, in the hitch log)
  uploadList_->Close();
  ID3D12CommandList* lists[] = {uploadList_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  if (HitchLog::on) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 1) {
      char text[120];
      snprintf(text, sizeof text, "(the uploads sent: %.0f MB)", static_cast<double>(uploadBytes_) / (1 << 20));
      HitchLog::Add(text, ms, 9);
    }
  }
  const uint64_t v = fenceNext_++;
  queue_->Signal(fence_.Get(), v);
  uploadFences_[uploadBatch_] = v;
  uploadBatch_ = (uploadBatch_ + 1) % kUploadBatches;
  uploadOpen_ = false;
  uploadBytes_ = 0;
}

void D3D12Device::Retire(IUnknown* object) {
  if (!object) return;
  Com<IUnknown> keep;
  object->AddRef();
  *keep.Put() = object;
  frames_[cur_].garbage.push_back(std::move(keep));
}

Span D3D12Device::Allocate(size_t size, size_t align) {
  FrameSlot& f = frames_[cur_];
  return AllocateIn(f.pages, f.page, f.offset, kPageSize, size, align);
}

// [mcrec] (the frame's own pages, or a recorder's: Allocate's work, on any thread for a recorder's)
Span D3D12Device::AllocateIn(std::vector<Page>& pages, size_t& page, size_t& offset, size_t pageSize, size_t size, size_t align) {
  for (;;) {
    if (page < pages.size()) {
      Page& p = pages[page];
      size_t at = (offset + align - 1) / align * align;
      if (at + size <= p.size) {
        offset = at + size;
        return {p.cpu + at, p.gpu + at, p.resource.Get(), at};
      }
      ++page;
      offset = 0;
      continue;
    }
    Page p;
    p.size = std::max(pageSize, (size + 65535) / 65536 * 65536);
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(p.size);
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(p.resource.Put()));
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) hr = p.resource->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
      Complain("allocating a frame upload page", hr);
      return {};
    }
    p.cpu = static_cast<uint8_t*>(mapped);
    p.gpu = p.resource->GetGPUVirtualAddress();
    pages.push_back(std::move(p));
  }
}

Buffer D3D12Device::CreateBuffer(const BufferDesc& desc) {
  BufferRec rec;
  rec.used = true;
  rec.kind = desc.kind;
  rec.dynamic = desc.dynamic;
  rec.stride = desc.stride;
  if (desc.kind == BufferKind::Card) {  // [gpuscenery] a resource of its own, which dispatches may write
    if (desc.dynamic || !desc.size || (desc.size & 15u) || !drawIndexedSignature_) return {};
    rec.size = desc.size;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(desc.size);
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(rec.card.Put()));
    if (FAILED(hr)) return Complain("making a card buffer", hr), Buffer{};
    uint8_t* cpu = nullptr;
    ID3D12Resource* page = nullptr;
    uint64_t from = 0;
    if (!BeginUpload() || !Stage(desc.size, 16, &cpu, &page, &from)) return Complain("uploading a card buffer", E_FAIL), Buffer{};
    if (desc.data) std::memcpy(cpu, desc.data, desc.size);
    else std::memset(cpu, 0, desc.size);
    uploadList_->CopyBufferRegion(rec.card.Get(), 0, page, from, desc.size);  // (from the common state: promoted, and back after)
    rec.gpu = rec.card->GetGPUVirtualAddress();
  } else if (!desc.dynamic) {
    if (!desc.data || !desc.size) return {};
    rec.size = desc.size;
    uint8_t* cpu = nullptr;
    ID3D12Resource* page = nullptr;
    uint64_t from = 0;
    if (!CutBlock(desc.size, &rec.block, &rec.offset)) return {};
    if (!BeginUpload() || !Stage(desc.size, 16, &cpu, &page, &from)) {
      FreeRange(rec.block, rec.offset, desc.size);
      return Complain("uploading a buffer", E_FAIL), Buffer{};
    }
    std::memcpy(cpu, desc.data, desc.size);
    BufferBlock& b = blocks_[static_cast<size_t>(rec.block)];
    uploadList_->CopyBufferRegion(b.resource.Get(), rec.offset, page, from, desc.size);
    rec.gpu = b.gpu + rec.offset;
    if (uploadBytes_ > (256ull << 20)) FlushUploads();  // (a map loading: sent on, so copies do not pile up)
  }
  uint32_t index;
  if (!freeBuffers_.empty()) {
    index = freeBuffers_.back();
    freeBuffers_.pop_back();
    buffers_[index] = std::move(rec);
  } else {
    index = static_cast<uint32_t>(buffers_.size());
    buffers_.push_back(std::move(rec));
  }
  if (desc.kind == BufferKind::Card) cardBuffers_.push_back(index + 1);  // [gpuscenery]
  return {index + 1};
}

// [gpuscenery] A card buffer from the state it is in to `to` (nothing when it is there already).
void D3D12Device::CardState(BufferRec& rec, D3D12_RESOURCE_STATES to, std::vector<D3D12_RESOURCE_BARRIER>* out) {
  if (!rec.card || rec.state == to) return;
  const D3D12_RESOURCE_BARRIER b = Transition(rec.card.Get(), rec.state, to);
  if (out) out->push_back(b);
  else list_->ResourceBarrier(1, &b);
  rec.state = to;
}

void D3D12Device::CardsReadable() {
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  for (uint32_t id : cardBuffers_)
    if (BufferRec* rec = Find(Buffer{id})) CardState(*rec, kCardReadable, &barriers);
  if (!barriers.empty()) list_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

void D3D12Device::UpdateBufferRange(Buffer buffer, uint32_t offset, const void* data, uint32_t size) {
  BufferRec* rec = Find(buffer);
  if (!rec || !rec->card || !inFrame_ || parallel_ || !data || !size || ((offset | size) & 3u) || static_cast<uint64_t>(offset) + size > rec->size) return;
  const Span s = Allocate(size, 16);
  if (!s.cpu) return;
  std::memcpy(s.cpu, data, size);
  CardState(*rec, D3D12_RESOURCE_STATE_COPY_DEST);
  list_->CopyBufferRegion(rec->card.Get(), offset, s.resource, s.offset, size);
  CardState(*rec, kCardReadable);  // (read by what comes next: a dispatch moves it on to be written)
}

void D3D12Device::DrawIndexedIndirect(Buffer arguments, uint32_t offset) {
  const BufferRec* rec = Find(arguments);
  if (!rec || !rec->card || !drawIndexedSignature_ || (offset & 3u) || static_cast<uint64_t>(offset) + 20 > rec->size) return;
  ++draws_;
  BindTextures();
  list_->ExecuteIndirect(drawIndexedSignature_.Get(), 1, rec->card.Get(), offset, nullptr, 0);
}

void D3D12Device::UpdateBuffer(Buffer buffer, const void* data, uint32_t size) {
  if (void* to = WriteBuffer(buffer, size)) std::memcpy(to, data, size);
}

void* D3D12Device::WriteBuffer(Buffer buffer, uint32_t size) {
  BufferRec* rec = Find(buffer);
  if (!rec || !rec->dynamic || !inFrame_ || !size) return nullptr;
  Span s = Allocate(size, 16);
  if (!s.cpu) return nullptr;
  rec->gpu = s.gpu;
  rec->size = size;
  rec->frame = frameNumber_;
  return s.cpu;
}

Texture D3D12Device::CreateTexture(const TextureDesc& desc) {
  DXGI_FORMAT format = ToDxgi(desc.format);
  if (format == DXGI_FORMAT_UNKNOWN || desc.width <= 0 || desc.height <= 0 || desc.mips <= 0 || desc.layers <= 0 || !desc.data)
    return {};
  uint32_t index;
  if (!freeTextures_.empty()) {
    index = freeTextures_.back();
  } else if (textures_.size() < kMaxTextures) {
    index = static_cast<uint32_t>(textures_.size());
  } else {
    fprintf(stderr, "render: Direct3D 12: out of texture slots (%u)\n", kMaxTextures);
    return {};
  }

  TextureRec rec;
  rec.used = true;
  rec.pixelBytes = IsBlockCompressed(desc.format) ? 0 : BytesPerPixelOrBlock(desc.format);
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = static_cast<UINT16>(desc.layers);
  rd.MipLevels = static_cast<UINT16>(desc.mips);
  rd.Format = format;
  rd.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  const auto t0 = std::chrono::steady_clock::now();  // [hitches] (its parts' times, for the hitch log)
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(rec.resource.Put()));
  if (FAILED(hr)) return Complain("creating a texture", hr), Texture{};
  const auto t1 = std::chrono::steady_clock::now();

  const UINT subresources = static_cast<UINT>(desc.mips * desc.layers);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> rowBytes(subresources);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&rd, 0, subresources, 0, layouts.data(), rows.data(), rowBytes.data(), &total);
  uint8_t* mapped = nullptr;
  ID3D12Resource* staging = nullptr;
  uint64_t stagedAt = 0;
  if (!BeginUpload() || !Stage(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &mapped, &staging, &stagedAt)) return Complain("uploading a texture", E_FAIL), Texture{};
  const auto t2 = std::chrono::steady_clock::now();
  for (D3D12_PLACED_SUBRESOURCE_FOOTPRINT& l : layouts) l.Offset += stagedAt;  // (where the page has room)
  mapped -= stagedAt;

  // the caller's images are tightly packed; the upload buffer wants each row at the footprint's pitch
  const bool blocks = IsBlockCompressed(desc.format);
  const size_t unit = static_cast<size_t>(BytesPerPixelOrBlock(desc.format));
  const uint8_t* src = static_cast<const uint8_t*>(desc.data);
  const uint8_t* srcEnd = src + desc.dataSize;
  bool truncated = false;
  for (int layer = 0; layer < desc.layers && !truncated; ++layer) {
    for (int mip = 0; mip < desc.mips; ++mip) {
      const UINT sub = static_cast<UINT>(layer * desc.mips + mip);
      size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
      size_t srcRow = (blocks ? (w + 3) / 4 : w) * unit;
      size_t srcRows = blocks ? (h + 3) / 4 : h;
      if (src + srcRow * srcRows > srcEnd) {
        truncated = true;
        break;
      }
      uint8_t* dst = static_cast<uint8_t*>(mapped) + layouts[sub].Offset;
      size_t copy = std::min<size_t>(srcRow, static_cast<size_t>(rowBytes[sub]));
      for (size_t y = 0; y < std::min<size_t>(srcRows, rows[sub]); ++y)
        std::memcpy(dst + y * layouts[sub].Footprint.RowPitch, src + y * srcRow, copy);
      src += srcRow * srcRows;
    }
  }
  if (truncated) {
    fprintf(stderr, "render: a %dx%d texture with %d mips has too little data (%zu bytes)\n", desc.width, desc.height, desc.mips,
            desc.dataSize);
    return {};
  }
  if (HitchLog::on) {  // [hitches] (a texture that took long: its parts)
    const auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    if (ms(t0, t3) > 2) {
      char text[160];
      snprintf(text, sizeof text, "(the texture's resource %.1f ms, its upload room %.1f ms, its copy %.1f ms)", ms(t0, t1), ms(t1, t2), ms(t2, t3));
      HitchLog::Add(text, ms(t0, t3), 9);
    }
  }
  for (UINT sub = 0; sub < subresources; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = rec.resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = sub;
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = layouts[sub];
    uploadList_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  D3D12_RESOURCE_BARRIER b = Transition(rec.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  uploadList_->ResourceBarrier(1, &b);
  if (uploadBytes_ > (256ull << 20)) FlushUploads();  // (a map loading: sent on, so copies do not pile up)

  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
  view.Format = format;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (desc.format == Format::BC5)  // (red also in alpha: a normal map's x where the game's DXT5 ones keep it)
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
  if (desc.cube && desc.layers == 6) {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    view.TextureCube.MipLevels = static_cast<UINT>(desc.mips);
  } else if (desc.layers > 1) {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = static_cast<UINT>(desc.mips);
    view.Texture2DArray.ArraySize = static_cast<UINT>(desc.layers);
  } else {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = static_cast<UINT>(desc.mips);
  }
  device_->CreateShaderResourceView(rec.resource.Get(), &view, StagingHandle(index));

  if (!freeTextures_.empty()) {
    freeTextures_.pop_back();
    textures_[index] = std::move(rec);
  } else {
    textures_.push_back(std::move(rec));
  }
  return {index};
}

// [spikes] The halves of CreateTexture and of a static CreateBuffer (gfx.h, PrepareTexture). Prepare, on any thread: what
// they make before their copies are recorded - a texture's resource, and its images laid out at the footprints' pitches in
// an upload buffer of its own; a buffer's contents in an upload buffer of its own (the device is free-threaded for these;
// nothing of the device's own lists, pages or tables is touched). Finish, on the frames' thread: the handle, the copies
// recorded into the open upload batch (sent ahead of the next frame's work, as CreateTexture's are), the texture's view;
// the upload buffer kept with the batch until the card is done with it (KeepStaging).
namespace {
struct PreparedTexture12 final : Device::Prepared {
  Com<ID3D12Resource> resource, staging;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
  uint64_t total = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Format kind = Format::Unknown;
  int mips = 1, layers = 1;
  bool cube = false;
};
struct PreparedBuffer12 final : Device::Prepared {
  Com<ID3D12Resource> staging;
  BufferDesc desc;  // (its data not kept: it is in the upload buffer)
};
}  // namespace

std::unique_ptr<Device::Prepared> D3D12Device::PrepareTexture(const TextureDesc& desc) {
  const DXGI_FORMAT format = ToDxgi(desc.format);
  if (format == DXGI_FORMAT_UNKNOWN || desc.width <= 0 || desc.height <= 0 || desc.mips <= 0 || desc.layers <= 0 || !desc.data) return nullptr;
  auto p = std::make_unique<PreparedTexture12>();
  p->format = format;
  p->kind = desc.format;
  p->mips = desc.mips;
  p->layers = desc.layers;
  p->cube = desc.cube;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = static_cast<UINT16>(desc.layers);
  rd.MipLevels = static_cast<UINT16>(desc.mips);
  rd.Format = format;
  rd.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(p->resource.Put()));
  if (FAILED(hr)) return Complain("creating a texture", hr), nullptr;
  const UINT subresources = static_cast<UINT>(desc.mips * desc.layers);
  p->layouts.resize(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> rowBytes(subresources);
  device_->GetCopyableFootprints(&rd, 0, subresources, 0, p->layouts.data(), rows.data(), rowBytes.data(), &p->total);
  D3D12_HEAP_PROPERTIES up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC bd = BufferResourceDesc(std::max<uint64_t>(p->total, 256));
  hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(p->staging.Put()));
  if (FAILED(hr)) return Complain("allocating a texture's upload buffer", hr), nullptr;
  uint8_t* mapped = nullptr;
  const D3D12_RANGE none = {0, 0};
  if (FAILED(p->staging->Map(0, &none, reinterpret_cast<void**>(&mapped)))) return Complain("mapping a texture's upload buffer", E_FAIL), nullptr;
  // (as CreateTexture: the caller's images are tightly packed; the upload buffer wants each row at the footprint's pitch)
  const bool blocks = IsBlockCompressed(desc.format);
  const size_t unit = static_cast<size_t>(BytesPerPixelOrBlock(desc.format));
  const uint8_t* src = static_cast<const uint8_t*>(desc.data);
  const uint8_t* srcEnd = src + desc.dataSize;
  for (int layer = 0; layer < desc.layers; ++layer)
    for (int mip = 0; mip < desc.mips; ++mip) {
      const UINT sub = static_cast<UINT>(layer * desc.mips + mip);
      const size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
      const size_t srcRow = (blocks ? (w + 3) / 4 : w) * unit;
      const size_t srcRows = blocks ? (h + 3) / 4 : h;
      if (src + srcRow * srcRows > srcEnd) {
        p->staging->Unmap(0, nullptr);
        fprintf(stderr, "render: a %dx%d texture with %d mips has too little data (%zu bytes)\n", desc.width, desc.height, desc.mips, desc.dataSize);
        return nullptr;
      }
      uint8_t* dst = mapped + p->layouts[sub].Offset;
      const size_t copy = std::min<size_t>(srcRow, static_cast<size_t>(rowBytes[sub]));
      for (size_t y = 0; y < std::min<size_t>(srcRows, rows[sub]); ++y)
        std::memcpy(dst + y * p->layouts[sub].Footprint.RowPitch, src + y * srcRow, copy);
      src += srcRow * srcRows;
    }
  p->staging->Unmap(0, nullptr);
  return p;
}

Texture D3D12Device::FinishTexture(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedTexture12*>(prepared.get());
  if (!p) return Device::FinishTexture(std::move(prepared));  // (one kept whole: CreateTexture)
  if (!p->resource || !p->staging) return {};
  const auto t0 = std::chrono::steady_clock::now();  // [spikes] (one that took long: its parts, in the hitch log)
  if (!BeginUpload()) return Complain("uploading a texture", E_FAIL), Texture{};
  const uint32_t index = NewTextureIndex();
  if (!index) return {};
  const auto t1 = std::chrono::steady_clock::now();
  for (UINT sub = 0; sub < static_cast<UINT>(p->layouts.size()); ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = p->resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = sub;
    from.pResource = p->staging.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = p->layouts[sub];
    uploadList_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  D3D12_RESOURCE_BARRIER b = Transition(p->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  uploadList_->ResourceBarrier(1, &b);
  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};  // (as CreateTexture's)
  view.Format = p->format;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (p->kind == Format::BC5)
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
  if (p->cube && p->layers == 6) {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    view.TextureCube.MipLevels = static_cast<UINT>(p->mips);
  } else if (p->layers > 1) {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = static_cast<UINT>(p->mips);
    view.Texture2DArray.ArraySize = static_cast<UINT>(p->layers);
  } else {
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = static_cast<UINT>(p->mips);
  }
  const auto t2 = std::chrono::steady_clock::now();
  device_->CreateShaderResourceView(p->resource.Get(), &view, StagingHandle(index));
  TextureRec rec;
  rec.used = true;
  rec.pixelBytes = IsBlockCompressed(p->kind) ? 0 : BytesPerPixelOrBlock(p->kind);
  rec.resource = std::move(p->resource);
  textures_[index] = std::move(rec);
  const auto t3 = std::chrono::steady_clock::now();
  KeepStaging(std::move(p->staging), p->total);  // (let go of once the card is done with this batch; a large one sent at once)
  if (HitchLog::on) {
    const auto t4 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    if (ms(t0, t4) > 2) {
      char text[200];
      snprintf(text, sizeof text, "(finished: its batch and slot %.1f ms, its copies %.1f ms, its view %.1f ms, its upload buffer kept %.1f ms)", ms(t0, t1),
               ms(t1, t2), ms(t2, t3), ms(t3, t4));
      HitchLog::Add(text, ms(t0, t4), 9);
    }
  }
  return {index};
}

std::unique_ptr<Device::Prepared> D3D12Device::PrepareBuffer(const BufferDesc& desc) {
  if (desc.dynamic || !desc.data || !desc.size) return nullptr;
  auto p = std::make_unique<PreparedBuffer12>();
  p->desc = desc;
  p->desc.data = nullptr;
  D3D12_HEAP_PROPERTIES up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC bd = BufferResourceDesc(desc.size);
  HRESULT hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(p->staging.Put()));
  if (FAILED(hr)) return Complain("allocating a buffer's upload buffer", hr), nullptr;
  void* mapped = nullptr;
  const D3D12_RANGE none = {0, 0};
  if (FAILED(p->staging->Map(0, &none, &mapped))) return Complain("mapping a buffer's upload buffer", E_FAIL), nullptr;
  std::memcpy(mapped, desc.data, desc.size);
  p->staging->Unmap(0, nullptr);
  return p;
}

Buffer D3D12Device::FinishBuffer(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedBuffer12*>(prepared.get());
  if (!p) return Device::FinishBuffer(std::move(prepared));  // (one kept whole: CreateBuffer)
  if (!p->staging) return {};
  // (as CreateBuffer's static path, copied from the buffer's own upload buffer in place of the batch's pages)
  BufferRec rec;
  rec.used = true;
  rec.kind = p->desc.kind;
  rec.stride = p->desc.stride;
  rec.size = p->desc.size;
  if (!CutBlock(p->desc.size, &rec.block, &rec.offset)) return {};
  if (!BeginUpload()) {
    FreeRange(rec.block, rec.offset, p->desc.size);
    return Complain("uploading a buffer", E_FAIL), Buffer{};
  }
  BufferBlock& b = blocks_[static_cast<size_t>(rec.block)];
  uploadList_->CopyBufferRegion(b.resource.Get(), rec.offset, p->staging.Get(), 0, p->desc.size);
  rec.gpu = b.gpu + rec.offset;
  uint32_t index;
  if (!freeBuffers_.empty()) {
    index = freeBuffers_.back();
    freeBuffers_.pop_back();
    buffers_[index] = std::move(rec);
  } else {
    index = static_cast<uint32_t>(buffers_.size());
    buffers_.push_back(std::move(rec));
  }
  KeepStaging(std::move(p->staging), p->desc.size);
  return {index + 1};
}

uint32_t D3D12Device::NewTextureIndex() {  // 0: no room
  if (!freeTextures_.empty()) {
    uint32_t index = freeTextures_.back();
    freeTextures_.pop_back();
    return index;
  }
  if (textures_.size() >= kMaxTextures) {
    fprintf(stderr, "render: Direct3D 12: out of texture slots (%u)\n", kMaxTextures);
    return 0;
  }
  textures_.emplace_back();
  return static_cast<uint32_t>(textures_.size() - 1);
}

Texture D3D12Device::CreateTarget(const TargetDesc& desc) { return MakeTarget(desc, {}); }

// [warmtarget] A target's resource, as CreateTarget makes it: on any thread (the device is free-threaded for this; nothing of
// its own lists, slots or tables is touched). The slow part of a target at the window's size.
HRESULT D3D12Device::TargetResource(const TargetDesc& desc, Com<ID3D12Resource>* out) {
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC rd = {};
  if (desc.storage) {
    const bool volume = desc.depth > 1;
    rd.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = static_cast<UINT64>(desc.width);
    rd.Height = static_cast<UINT>(desc.height);
    rd.DepthOrArraySize = static_cast<UINT16>(volume ? desc.depth : 1);
    rd.MipLevels = static_cast<UINT16>(desc.mips);
    rd.Format = ToDxgi(desc.format);
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                            IID_PPV_ARGS(out->Put()));
  }
  const bool depth = desc.format == Format::D32;
  const DXGI_FORMAT format = depth ? DXGI_FORMAT_D32_FLOAT : ToDxgi(desc.format);
  const int samples = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? desc.samples : 1;
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = static_cast<UINT16>(desc.layers);
  rd.MipLevels = 1;
  rd.Format = depth ? DXGI_FORMAT_R32_TYPELESS : format;
  rd.SampleDesc.Count = static_cast<UINT>(samples);
  rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = format;
  if (!depth) std::memcpy(clear.Color, desc.clearValue, sizeof clear.Color);
  return device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clear,
                                          IID_PPV_ARGS(out->Put()));
}

// [warmtarget] The halves of CreateTarget (gfx.h, PrepareTarget): its resource made on the calling thread; its slots, views
// and handle on the frames' thread. A description CreateTarget would refuse is refused by Prepare too (null: Create then).
namespace {
struct PreparedTarget12 final : Device::Prepared {
  TargetDesc desc;
  Com<ID3D12Resource> resource;
};
}  // namespace

std::unique_ptr<Device::Prepared> D3D12Device::PrepareTarget(const TargetDesc& desc) {
  if (desc.width <= 0 || desc.height <= 0) return nullptr;
  if (desc.storage) {
    if (ToDxgi(desc.format) == DXGI_FORMAT_UNKNOWN || desc.depth <= 0 || desc.layers != 1 || desc.samples > 1 || desc.mips <= 0) return nullptr;
  } else if ((desc.format != Format::D32 && ToDxgi(desc.format) == DXGI_FORMAT_UNKNOWN) || IsBlockCompressed(desc.format) || desc.layers <= 0) {
    return nullptr;
  }
  auto p = std::make_unique<PreparedTarget12>();
  p->desc = desc;
  if (FAILED(TargetResource(desc, &p->resource))) return nullptr;  // (CreateTarget then: it says why)
  return p;
}

Texture D3D12Device::FinishTarget(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedTarget12*>(prepared.get());
  if (!p) return Device::FinishTarget(std::move(prepared));  // (one kept whole: CreateTarget)
  return MakeTarget(p->desc, std::move(p->resource));
}

Texture D3D12Device::MakeTarget(const TargetDesc& desc, Com<ID3D12Resource> made) {
  if (desc.storage) {  // written by compute shaders: no render-target view, a storage view for each level
    const DXGI_FORMAT format = ToDxgi(desc.format);
    const bool volume = desc.depth > 1;
    int most = 1;  // (the levels it can have)
    for (int s = std::max({desc.width, desc.height, volume ? desc.depth : 1}); s > 1; s >>= 1) ++most;
    const bool writable = desc.format == Format::RGBA8 || desc.format == Format::R8 || desc.format == Format::R16F || desc.format == Format::RG16F ||
                          desc.format == Format::RGBA16F || desc.format == Format::R32F || desc.format == Format::R32UI || desc.format == Format::RGBA32F;
    if (!writable || desc.width <= 0 || desc.height <= 0 || desc.depth <= 0 || desc.layers != 1 || desc.samples > 1 || desc.mips <= 0 ||
        desc.mips > most) {
      fprintf(stderr, "render: Direct3D 12: a storage target needs RGBA8, R8, R16F, RG16F, RGBA16F, R32F, R32UI or RGBA32F, one layer, one sample, and levels it can have\n");
      return {};
    }
    uint32_t first = 0;
    const uint32_t count = static_cast<uint32_t>(desc.mips);
    for (size_t i = 0; i < freeUav_.size() && !first; ++i)
      if (freeUav_[i].second == count) {
        first = freeUav_[i].first;
        freeUav_.erase(freeUav_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    if (!first && nextUav_ + count <= kMaxStorageViews) {
      first = nextUav_;
      nextUav_ += count;
    }
    if (!first) {
      fprintf(stderr, "render: Direct3D 12: too many storage targets\n");
      return {};
    }
    TextureRec rec;
    rec.used = rec.storage = true;
    rec.width = desc.width;
    rec.height = desc.height;
    rec.slices = desc.depth;
    rec.mips = desc.mips;
    rec.format = format;
    rec.uav = first;
    HRESULT hr = S_OK;  // [warmtarget] (its resource: made here, or by PrepareTarget on another thread)
    if (made) rec.resource = std::move(made);
    else hr = TargetResource(desc, &rec.resource);
    if (FAILED(hr)) {
      freeUav_.emplace_back(first, count);
      return Complain("creating a storage target", hr), Texture{};
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (volume) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      srv.Texture3D.MipLevels = count;
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = count;
    }
    for (uint32_t mip = 0; mip < count; ++mip) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
      uv.Format = format;
      if (volume) {
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.MipSlice = mip;
        uv.Texture3D.WSize = UINT(-1);  // (all its slices at that level)
      } else {
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Texture2D.MipSlice = mip;
      }
      device_->CreateUnorderedAccessView(rec.resource.Get(), nullptr, &uv, UavHandle(first + mip));
    }
    const uint32_t index = NewTextureIndex();
    if (!index) {
      freeUav_.emplace_back(first, count);
      return {};
    }
    device_->CreateShaderResourceView(rec.resource.Get(), &srv, StagingHandle(index));
    textures_[index] = std::move(rec);
    return {index};
  }
  const bool depth = desc.format == Format::D32;
  const DXGI_FORMAT format = depth ? DXGI_FORMAT_D32_FLOAT : ToDxgi(desc.format);
  const int samples = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? desc.samples : 1;
  if (format == DXGI_FORMAT_UNKNOWN || IsBlockCompressed(desc.format) || desc.width <= 0 || desc.height <= 0 || desc.layers <= 0 ||
      (!depth && desc.layers != 1) || (samples > 1 && desc.layers != 1))
    return {};
  TextureRec rec;
  rec.used = rec.target = true;
  rec.depth = depth;
  rec.width = desc.width;
  rec.height = desc.height;
  rec.layers = desc.layers;
  rec.format = format;

  // its views: one render-target view, or one depth view per layer
  bool haveView = false;
  if (depth) {
    for (size_t i = 0; i < freeDsv_.size() && !haveView; ++i)
      if (freeDsv_[i].second == static_cast<uint32_t>(desc.layers)) {
        rec.view = freeDsv_[i].first;
        freeDsv_.erase(freeDsv_.begin() + static_cast<std::ptrdiff_t>(i));
        haveView = true;
      }
    if (!haveView && nextDsv_ + static_cast<uint32_t>(desc.layers) <= 1 + kMaxDepthViews) {
      rec.view = nextDsv_;
      nextDsv_ += static_cast<uint32_t>(desc.layers);
      haveView = true;
    }
  } else if (!freeRtv_.empty()) {
    rec.view = freeRtv_.back();
    freeRtv_.pop_back();
    haveView = true;
  } else if (nextRtv_ < kFrames + kMaxColorTargets) {
    rec.view = nextRtv_++;
    haveView = true;
  }
  if (!haveView) {
    fprintf(stderr, "render: Direct3D 12: too many render targets\n");
    return {};
  }

  rec.samples = samples;
  HRESULT hr = S_OK;  // [warmtarget] (its resource: made here, or by PrepareTarget on another thread)
  if (made) rec.resource = std::move(made);
  else hr = TargetResource(desc, &rec.resource);
  if (FAILED(hr)) return Complain("creating a render target", hr), Texture{};

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = depth ? DXGI_FORMAT_R32_FLOAT : format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (samples > 1) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;  // (a view has to exist; shaders do not read it)
  } else if (desc.layers > 1) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = static_cast<UINT>(desc.layers);
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
  }
  if (depth) {
    for (int layer = 0; layer < desc.layers; ++layer) {
      D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
      dv.Format = DXGI_FORMAT_D32_FLOAT;
      if (samples > 1) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      } else if (desc.layers > 1) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = static_cast<UINT>(layer);
        dv.Texture2DArray.ArraySize = 1;
      } else {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      }
      device_->CreateDepthStencilView(rec.resource.Get(), &dv, DsvHandle(rec.view + static_cast<uint32_t>(layer)));
    }
  } else {
    device_->CreateRenderTargetView(rec.resource.Get(), nullptr, RtvHandle(rec.view));
  }
  const uint32_t index = NewTextureIndex();
  if (!index) return {};
  device_->CreateShaderResourceView(rec.resource.Get(), &srv, StagingHandle(index));
  textures_[index] = std::move(rec);
  return {index};
}

bool D3D12Device::CompileFxc(std::string_view source, const char* entry, const char* target, const char* name, Com<ID3DBlob>& out,
                             std::string* error) {
  constexpr UINT kFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
  // (kept from an earlier run when the same source was compiled the same way: by a hash of all of it)
  std::string cached;
  if (!shaderCache_.empty()) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t size) {
      for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    const char kVersion[] = "d3dcompile/1";
    mix(kVersion, sizeof kVersion);
    mix(source.data(), source.size());
    mix(entry, std::strlen(entry) + 1);
    mix(target, std::strlen(target) + 1);
    mix(&kFlags, sizeof kFlags);
    char file[32];
    snprintf(file, sizeof file, "%016llx.cso", static_cast<unsigned long long>(h));
    cached = shaderCache_ + "\\" + file;
    if (FILE* f = fopen(cached.c_str(), "rb")) {
      fseek(f, 0, SEEK_END);
      const long size = ftell(f);
      fseek(f, 0, SEEK_SET);
      bool ok = size > 0 && SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(size), out.Put())) &&
                fread(out->GetBufferPointer(), 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
      fclose(f);
      if (ok) return true;
      out.Reset();
    }
  }
  Com<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry, target, kFlags, 0, out.Put(), errors.Put());
  if (FAILED(hr)) {
    std::string text = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    if (error) *error = std::string(name) + " (" + entry + "): " + text;
    return false;
  }
  if (!cached.empty()) {  // (written under another name first, so a half-written file is never read)
    const std::string part = cached + ".part";
    if (FILE* f = fopen(part.c_str(), "wb")) {
      const bool ok = fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), f) == out->GetBufferSize();
      fclose(f);
      if (!ok || !MoveFileExA(part.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(part.c_str());
    }
  }
  return true;
}

bool D3D12Device::MakePipeline(const PipelineDesc& desc, PipelineRec* out, std::string* error) {
  return MakePipelineWith(desc, out, error, false, &driverMs_, &driverKept_);
}

bool D3D12Device::MakePipelineWith(const PipelineDesc& desc, PipelineRec* out, std::string* error, bool background, double* driverMs, bool* kept) {
  Com<ID3DBlob> vs, ps;
  const bool hasPixelShader = !desc.pixelSource.empty();
  auto blob = [](const std::vector<uint8_t>& code, Com<ID3DBlob>& out) {
    if (FAILED(D3DCreateBlob(code.size(), out.Put()))) return false;
    std::memcpy(out->GetBufferPointer(), code.data(), code.size());
    return true;
  };
  // DXC's DXIL (shader model 6.0) where DXC is beside the program: it compiles the renderer's shaders in
  // well under half the time Direct3D's own compiler (FXC) takes (a first start 20-26 s against 9-11),
  // and the card runs them at least as fast. FXC for both of the pipeline's shaders where DXC cannot
  // make one of them.
  bool compiled = false;
  if (background) {  // [snowhitch] (on the device's own thread: from the cache, what it lacks compiled into it first)
    std::vector<uint8_t> v, p;
    const std::string vSource(desc.vertexSource), pSource(desc.pixelSource);
    auto cached = [&] {
      return CachedDxil(vSource, desc.vertexEntry, "vs", shaderCache_, &v) && (!hasPixelShader || CachedDxil(pSource, desc.pixelEntry, "ps", shaderCache_, &p));
    };
    if (!cached()) {
      std::vector<ShaderJob> jobs = {{vSource, desc.vertexEntry, "vs"}};
      if (hasPixelShader) jobs.push_back({pSource, desc.pixelEntry, "ps"});
      PrewarmDxil(jobs, shaderCache_);
    }
    if (!cached() || !blob(v, vs) || (hasPixelShader && !blob(p, ps))) {
      if (error) *error = std::string(desc.name) + ": its shaders could not be compiled on the device's own thread";
      return false;
    }
    compiled = true;
  } else if (dxil_) {
    std::vector<uint8_t> v, p;
    std::string why;
    bool missing = false;
    compiled = CompileDxil(std::string(desc.vertexSource), desc.vertexEntry, "vs", shaderCache_, &v, &why, &missing) &&
               (!hasPixelShader || CompileDxil(std::string(desc.pixelSource), desc.pixelEntry, "ps", shaderCache_, &p, &why, &missing)) &&
               blob(v, vs) && (!hasPixelShader || blob(p, ps));
    if (!compiled) {
      if (missing) {
        fprintf(stderr, "render: Direct3D 12: %s; the shaders compiled by Direct3D's own compiler\n", why.c_str());
        dxil_ = false;
      } else {
        fprintf(stderr, "render: Direct3D 12: %s: DXC: %s; Direct3D's own compiler instead\n", desc.name, why.c_str());
      }
      vs.Reset();
      ps.Reset();
    }
  }
  if (!compiled) {
    if (!CompileFxc(desc.vertexSource, desc.vertexEntry, "vs_5_1", desc.name, vs, error)) return false;
    if (hasPixelShader && !CompileFxc(desc.pixelSource, desc.pixelEntry, "ps_5_1", desc.name, ps, error)) return false;
  }

  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  for (const VertexAttr& a : desc.attrs) {
    D3D12_INPUT_ELEMENT_DESC e = {};
    e.SemanticName = a.semantic;
    e.SemanticIndex = static_cast<UINT>(a.index);
    e.Format = ToDxgi(a.type);
    e.AlignedByteOffset = a.offset;
    e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    elements.push_back(e);
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = rootSignature_.Get();
  pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  if (hasPixelShader) pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pd.SampleMask = 0xffffffffu;
  pd.BlendState.AlphaToCoverageEnable = desc.alphaToCoverage && desc.samples > 1;
  D3D12_RENDER_TARGET_BLEND_DESC& bt = pd.BlendState.RenderTarget[0];
  bt.RenderTargetWriteMask = !desc.colorWrite ? 0 : desc.alphaWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
  bt.BlendOp = bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  bt.SrcBlend = bt.SrcBlendAlpha = D3D12_BLEND_ONE;
  bt.DestBlend = bt.DestBlendAlpha = D3D12_BLEND_ZERO;
  switch (desc.blend) {
    case Blend::Opaque: break;
    case Blend::Alpha:
      bt.BlendEnable = TRUE;
      bt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      bt.DestBlend = bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      break;
    case Blend::Premultiplied:
      bt.BlendEnable = TRUE;
      bt.DestBlend = bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      break;
    case Blend::Additive:
      bt.BlendEnable = TRUE;
      bt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      bt.DestBlend = bt.DestBlendAlpha = D3D12_BLEND_ONE;
      break;
    case Blend::Multiply:
      bt.BlendEnable = TRUE;
      bt.SrcBlend = D3D12_BLEND_ZERO;
      bt.DestBlend = D3D12_BLEND_SRC_COLOR;
      bt.SrcBlendAlpha = D3D12_BLEND_ZERO;
      bt.DestBlendAlpha = D3D12_BLEND_ONE;
      break;
    case Blend::InverseModulate:
      bt.BlendEnable = TRUE;
      bt.SrcBlend = D3D12_BLEND_ZERO;
      bt.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
      bt.SrcBlendAlpha = D3D12_BLEND_ZERO;
      bt.DestBlendAlpha = D3D12_BLEND_ONE;
      break;
  }
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = desc.cull == Cull::None ? D3D12_CULL_MODE_NONE
                                : desc.cull == Cull::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
  pd.RasterizerState.FrontCounterClockwise = desc.frontCounterClockwise;
  pd.RasterizerState.DepthClipEnable = TRUE;
  // reversed depth: "toward the camera" is a larger value; a 32-bit float buffer scales the bias by the
  // exponent of the primitive's largest depth, so the unit is 2^-23 of that
  pd.RasterizerState.DepthBias = static_cast<INT>(desc.depthBias);
  pd.DepthStencilState.DepthEnable = desc.depthTest;
  pd.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
  pd.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
  pd.PrimitiveTopologyType = desc.topology == Topology::Lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                              : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (!desc.depthOnly) {
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = ToDxgi(desc.colorFormat);
  }
  pd.DSVFormat = desc.depthTarget ? kDepthFormat : DXGI_FORMAT_UNKNOWN;
  if (!desc.depthTarget) pd.DepthStencilState.DepthEnable = FALSE;
  pd.SampleDesc.Count = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? static_cast<UINT>(desc.samples) : 1;
  pd.RasterizerState.MultisampleEnable = pd.SampleDesc.Count > 1;

  PipelineRec rec;
  rec.used = true;
  rec.topology = desc.topology == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  const auto driverStart = std::chrono::steady_clock::now();  // [hitches]
  bool keptNow = false;  // [hitches] (made by the driver in an earlier run: gfx_d3d12_pipelines.h)
  HRESULT hr = pipelineStore_.Graphics(device_.Get(), pd, rec.state.Put(), &keptNow);
  *driverMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - driverStart).count();
  *kept = keptNow;
  if (FAILED(hr)) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s: CreateGraphicsPipelineState failed (0x%08lx)", desc.name, static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return false;
  }
  *out = std::move(rec);
  return true;
}

bool D3D12Device::MakeCompute(const ComputeDesc& desc, PipelineRec* out, std::string* error) {
  return MakeComputeWith(desc, out, error, false, &driverMs_, &driverKept_);
}

bool D3D12Device::MakeComputeWith(const ComputeDesc& desc, PipelineRec* out, std::string* error, bool background, double* driverMs, bool* kept) {
  Com<ID3DBlob> cs;
  bool compiled = false;
  if (background) {  // [snowhitch] (as MakePipelineWith's)
    std::vector<uint8_t> code;
    const std::string source(desc.source);
    if (!CachedDxil(source, desc.entry, "cs", shaderCache_, &code)) PrewarmDxil({{source, desc.entry, "cs"}}, shaderCache_);
    if (!CachedDxil(source, desc.entry, "cs", shaderCache_, &code) || FAILED(D3DCreateBlob(code.size(), cs.Put()))) {
      if (error) *error = std::string(desc.name) + ": its shader could not be compiled on the device's own thread";
      return false;
    }
    std::memcpy(cs->GetBufferPointer(), code.data(), code.size());
    compiled = true;
  } else if (dxil_) {  // (DXC where it is beside the program, as for the drawing pipelines)
    std::vector<uint8_t> code;
    std::string why;
    bool missing = false;
    compiled = CompileDxil(std::string(desc.source), desc.entry, "cs", shaderCache_, &code, &why, &missing) &&
               SUCCEEDED(D3DCreateBlob(code.size(), cs.Put()));
    if (compiled) {
      std::memcpy(cs->GetBufferPointer(), code.data(), code.size());
    } else {
      if (missing) dxil_ = false;
      fprintf(stderr, "render: Direct3D 12: %s: DXC: %s; Direct3D's own compiler instead\n", desc.name, why.c_str());
      cs.Reset();
    }
  }
  if (!compiled && !CompileFxc(desc.source, desc.entry, "cs_5_1", desc.name, cs, error)) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
  cd.pRootSignature = rootSignature_.Get();
  cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  PipelineRec rec;
  rec.used = rec.compute = true;
  bool keptNow = false;  // [hitches] (as MakePipeline's)
  const auto driverStart = std::chrono::steady_clock::now();
  HRESULT hr = pipelineStore_.Compute(device_.Get(), cd, rec.state.Put(), &keptNow);
  *driverMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - driverStart).count();
  *kept = keptNow;
  if (FAILED(hr)) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s: CreateComputePipelineState failed (0x%08lx)", desc.name, static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return false;
  }
  *out = std::move(rec);
  return true;
}

Pipeline D3D12Device::Keep(PipelineRec rec) {
  uint32_t index;
  if (!freePipelines_.empty()) {
    index = freePipelines_.back();
    freePipelines_.pop_back();
    pipelines_[index] = std::move(rec);
  } else {
    index = static_cast<uint32_t>(pipelines_.size());
    pipelines_.push_back(std::move(rec));
  }
  return {index + 1};
}

Pipeline D3D12Device::CreatePipeline(const PipelineDesc& desc, std::string* error) {
  PipelineRec rec;
  if (batching_) {  // (made at the batch's end, its shaders compiled with the others')
    rec.used = true;
    rec.topology = desc.topology == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rec.pending = PendingPipeline::Of(desc);
    return Keep(std::move(rec));
  }
  if (backgroundAsked_) {  // [snowhitch] (made on the device's own thread; taken the first time it is set)
    rec.used = true;
    rec.topology = desc.topology == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rec.background = AskBackground(PendingPipeline::Of(desc));
    return Keep(std::move(rec));
  }
  HitchLog::Scope made("pipeline", desc.name);  // [hitches] (one made outside a batch: while the game runs)
  driverMs_ = 0;
  if (!MakePipeline(desc, &rec, error)) return {};
  if (HitchLog::on) made.Append(" (the driver " + std::to_string(static_cast<int>(driverMs_ + 0.5)) + " ms" + (driverKept_ ? ", kept" : "") + ")");
  return Keep(std::move(rec));
}

Pipeline D3D12Device::CreateCompute(const ComputeDesc& desc, std::string* error) {
  PipelineRec rec;
  if (batching_) {
    rec.used = rec.compute = true;
    rec.pending = PendingPipeline::Of(desc);
    return Keep(std::move(rec));
  }
  if (backgroundAsked_) {  // [snowhitch]
    rec.used = rec.compute = true;
    rec.background = AskBackground(PendingPipeline::Of(desc));
    return Keep(std::move(rec));
  }
  HitchLog::Scope made("compute pipeline", desc.name);  // [hitches]
  driverMs_ = 0;
  if (!MakeCompute(desc, &rec, error)) return {};
  if (HitchLog::on) made.Append(" (the driver " + std::to_string(static_cast<int>(driverMs_ + 0.5)) + " ms" + (driverKept_ ? ", kept" : "") + ")");
  return Keep(std::move(rec));
}

// A pipeline asked for in a batch, made now in its slot (what could not be made leaves the slot empty: its
// handle then draws nothing).
bool D3D12Device::Realize(uint32_t index, std::string* error) {
  std::shared_ptr<PendingPipeline> p = std::move(pipelines_[index].pending);
  if (!p) return true;
  PipelineRec rec;
  const bool made = p->compute ? MakeCompute(p->computeDesc, &rec, error) : MakePipeline(p->desc, &rec, error);
  pipelines_[index] = made ? std::move(rec) : PipelineRec{};
  return made;
}

// [snowhitch] A pipeline to be made on the device's own thread (begun the first time one is asked for).
std::shared_ptr<BackgroundPipeline> D3D12Device::AskBackground(std::shared_ptr<PendingPipeline> what) {
  auto job = std::make_shared<BackgroundPipeline>();
  job->what = std::move(what);
  {
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    backgroundQueue_.push_back(job);
    if (!backgroundThread_.joinable()) backgroundThread_ = std::thread([this] { BackgroundWork(); });
  }
  backgroundWake_.notify_all();
  return job;
}

// [snowhitch] The device's own thread: the pipelines asked for, made one after another in the order asked (the shader cache
// and the kept pipelines are shared with the frames' thread under their own locks; Direct3D 12's device may be called from
// any thread).
void D3D12Device::BackgroundWork() {
  for (;;) {
    std::shared_ptr<BackgroundPipeline> job;
    {
      std::unique_lock<std::mutex> lock(backgroundMutex_);
      backgroundWake_.wait(lock, [this] { return backgroundStop_ || !backgroundQueue_.empty(); });
      if (backgroundStop_) return;
      job = backgroundQueue_.front();
      backgroundQueue_.pop_front();
      job->stage = BackgroundPipeline::Making;
      backgroundMaking_ = 1;  // [bgload]
    }
    const PendingPipeline& what = *job->what;
    HitchLog::Scope made(what.compute ? "compute pipeline made ahead" : "pipeline made ahead", what.name);  // [hitches] (elsewhere)
    PipelineRec rec;
    std::string why;
    double ms = 0;
    bool kept = false;
    const bool ok = what.compute ? MakeComputeWith(what.computeDesc, &rec, &why, true, &ms, &kept) : MakePipelineWith(what.desc, &rec, &why, true, &ms, &kept);
    if (HitchLog::on && ok) made.Append(" (the driver " + std::to_string(static_cast<int>(ms + 0.5)) + " ms" + (kept ? ", kept" : "") + ")");
    made.End();
    {
      std::lock_guard<std::mutex> lock(backgroundMutex_);
      backgroundMaking_ = 0;  // [bgload]
      job->made = std::move(rec);
      job->why = why;
      job->stage = ok ? BackgroundPipeline::Made : BackgroundPipeline::NotMade;
    }
    backgroundDone_.notify_all();
  }
}

// [snowhitch] Pipeline `index`, asked for in the background, into its slot: as made there; waited for while it is being made;
// made here, as it would have been when it was asked for, when its turn has not come yet or it could not be made there.
void D3D12Device::TakeBackground(uint32_t index) {
  std::shared_ptr<BackgroundPipeline> job = pipelines_[index].background;
  bool here = false;
  {
    std::unique_lock<std::mutex> lock(backgroundMutex_);
    if (job->stage == BackgroundPipeline::Queued) {
      backgroundQueue_.erase(std::find(backgroundQueue_.begin(), backgroundQueue_.end(), job));
      here = true;
    } else {
      const bool making = job->stage == BackgroundPipeline::Making;
      const auto start = std::chrono::steady_clock::now();
      backgroundDone_.wait(lock, [&] { return job->stage == BackgroundPipeline::Made || job->stage == BackgroundPipeline::NotMade; });
      here = job->stage == BackgroundPipeline::NotMade;
      if (HitchLog::on && making)  // [hitches] (wanted while it was being made: the frame waited)
        HitchLog::Add("waited for a pipeline made ahead: " + job->what->name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
  }
  PipelineRec& rec = pipelines_[index];
  if (!here) {
    rec = std::move(job->made);
    return;
  }
  const PendingPipeline& what = *job->what;
  if (!job->why.empty()) fprintf(stderr, "render: Direct3D 12: %s; made on the frames' thread\n", job->why.c_str());
  PipelineRec made;
  std::string why;
  {
    HitchLog::Scope note(what.compute ? "compute pipeline" : "pipeline", what.name);  // [hitches] (as CreatePipeline's)
    driverMs_ = 0;
    const bool ok = what.compute ? MakeCompute(what.computeDesc, &made, &why) : MakePipeline(what.desc, &made, &why);
    if (HitchLog::on && ok) note.Append(" (the driver " + std::to_string(static_cast<int>(driverMs_ + 0.5)) + " ms" + (driverKept_ ? ", kept" : "") + ")");
    if (!ok) fprintf(stderr, "render: Direct3D 12: %s\n", why.c_str());
  }
  if (made.state) {
    rec = std::move(made);
  } else {  // (cannot be made: its handle draws nothing, as a batch's)
    rec = PipelineRec{};
    rec.used = true;
  }
}

void D3D12Device::BeginPipelineBatch() {
  batching_ = dxil_ && !shaderCache_.empty();
  batchError_.clear();
}

bool D3D12Device::EndPipelineBatch(std::string* error) {
  if (!batching_) return true;
  batching_ = false;
  std::vector<ShaderJob> jobs;
  for (const PipelineRec& r : pipelines_)
    if (r.pending) r.pending->Jobs(&jobs);
  PrewarmDxil(jobs, shaderCache_);  // (the cache's missing shaders, on several cores at once)
  for (uint32_t i = 0; i < pipelines_.size(); ++i) {
    std::string why;
    if (pipelines_[i].pending && !Realize(i, &why) && batchError_.empty()) batchError_ = why;
  }
  pipelineStore_.Save();  // [hitches] (what the driver made of them kept for the next run, now: a game may end any way)
  if (batchError_.empty()) return true;
  if (error) *error = batchError_;
  return false;
}

void D3D12Device::Destroy(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec) return;
  if (rec->block >= 0) frames_[cur_].freedRanges.push_back({static_cast<uint64_t>(rec->block), rec->offset, rec->size});
  if (rec->card) {  // [gpuscenery] (let go of once the frames that use it are done)
    Retire(rec->card.Get());
    cardBuffers_.erase(std::remove(cardBuffers_.begin(), cardBuffers_.end(), buffer.id), cardBuffers_.end());
  }
  *rec = BufferRec{};
  freeBuffers_.push_back(buffer.id - 1);
}

void D3D12Device::Destroy(Texture texture) {
  TextureRec* rec = Find(texture);
  if (!rec) return;
  if (rec->target) {
    if (passColor_ == texture.id) passColor_ = 0;
    if (passDepth_ == texture.id) passDepth_ = 0;
    if (rec->depth) freeDsv_.emplace_back(rec->view, static_cast<uint32_t>(rec->layers));
    else freeRtv_.push_back(rec->view);
  }
  if (rec->storage) {
    freeUav_.emplace_back(rec->uav, static_cast<uint32_t>(rec->mips));
    unordered_.erase(std::remove(unordered_.begin(), unordered_.end(), texture.id), unordered_.end());
    for (auto& s : boundStorage_)
      if (s.first == texture.id) s = {};
  }
  Retire(rec->resource.Get());
  *rec = TextureRec{};
  device_->CopyDescriptorsSimple(1, StagingHandle(texture.id), StagingHandle(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  freeTextures_.push_back(texture.id);
}

void D3D12Device::Destroy(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (!rec) return;
  if (rec->background) {  // [snowhitch] (asked for in the background and not taken: not made, or let go of when made)
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    auto at = std::find(backgroundQueue_.begin(), backgroundQueue_.end(), rec->background);
    if (at != backgroundQueue_.end()) backgroundQueue_.erase(at);
  }
  if (rec->state) Retire(rec->state.Get());
  *rec = PipelineRec{};
  freePipelines_.push_back(pipeline.id - 1);
}

void D3D12Device::BeginFrame(const float clearColor[4]) {
  if (wantGenerating_ != generating_) SwitchFrameGeneration();  // (asked for during the frame before)
  if (wantLatency_ != latency_ || ChainWrongForVsync()) SwitchLatency();
  // (the latency limiter, where the host did not wait before its input: the wait for the swap chain's object here)
  if (latency_ && !generating_ && !latencyTicket_ && !PhotoNow()) LimitFrames(false);  // (not for a photo's frames)
  // (with frame generation, the slots in turn; the presenter done with the slot's pictures)
  cur_ = generating_ ? static_cast<int>(frameNumber_ % kFrames) : static_cast<int>(swapChain_->GetCurrentBackBufferIndex());
  FrameSlot& f = frames_[cur_];
  WaitFence(f.fence);
  if (generating_) WaitPresented(cur_);
  // (the slot's frame before, done now, for the limiter before its record makes way: where the card sets the pace it
  // is done only just in time for this wait, after the limiter last looked)
  if (latency_ && latencySlots_[cur_].timed) TakeWork();
  if (latency_ || measureLatency) {  // (this frame's input read: the host's last WaitForNextFrame, else now)
    latencySlots_[cur_] = LatencySlot{readAt_ > 0 ? readAt_ : Now(), 0, nextDeadline_, nextPaced_, false};
    readAt_ = nextDeadline_ = 0;
    nextPaced_ = false;
  }
  if (timeQueries_ && f.marks.size() > 1) {  // the stages' times of this slot's last frame, done now
    void* mapped = nullptr;
    const size_t first = static_cast<size_t>(cur_) * kMarks;
    D3D12_RANGE range = {first * 8, (first + f.marks.size()) * 8};
    if (SUCCEEDED(timeReadback_->Map(0, &range, &mapped))) {
      const uint64_t* t = static_cast<const uint64_t*>(mapped) + first;
      lastTimes_.clear();
      for (size_t i = 0; i + 1 < f.marks.size(); ++i) {
        const double ms = t[i + 1] >= t[i] ? static_cast<double>(t[i + 1] - t[i]) / ticksPerMs_ : 0;
        auto same = std::find_if(lastTimes_.begin(), lastTimes_.end(), [&](const auto& s) { return std::strcmp(s.first, f.marks[i]) == 0; });
        if (same != lastTimes_.end()) same->second += ms;
        else lastTimes_.emplace_back(f.marks[i], ms);
      }
      D3D12_RANGE none = {0, 0};
      timeReadback_->Unmap(0, &none);
    }
  }
  f.marks.clear();
  LetGo(&f.garbage);  // [bgload] (was f.garbage.clear(): on the releaser's thread now)
  for (const auto& r : f.freedRanges) FreeRange(static_cast<int>(r[0]), r[1], r[2]);
  f.freedRanges.clear();
  f.page = f.offset = 0;
  f.descriptorsUsed = 0;
  f.tables.clear();
  ++frameNumber_;
  inFrame_ = true;
  f.allocator->Reset();
  {  // [mcrec] the frame's first list is its own list again; the slot's further allocators and the recorders' room anew
    for (size_t i = 0; i < f.moreUsed; ++i) f.moreAllocators[i]->Reset();
    f.moreUsed = 0;
    for (FrameSlot::RecorderRoom& r : f.recorders) {
      r.page = r.offset = 0;
      r.tables.clear();
    }
    moreListsUsed_ = 0;
    order_.clear();
    parallel_ = 0;
    set_ = DrawSet{};
    list_ = firstList_;
    list4_ = firstList4_;
  }
  for (uint32_t id : cardBuffers_)  // [gpuscenery] (buffers are back in the common state once a frame's lists have run)
    if (BufferRec* rec = Find(Buffer{id})) rec->state = D3D12_RESOURCE_STATE_COMMON;
  list_->Reset(f.allocator.Get(), nullptr);
  if (latency_ && latencyQueries_) list_->EndQuery(latencyQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * cur_));
  if (!cardBuffers_.empty()) CardsReadable();  // [gpuscenery] (readable by the frame's first draws)

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvStart_, dsv = dsvStart_;
  int viewWidth = width_, viewHeight = height_;
  const bool photo = PhotoNow();
  TextureRec* window = generating_ && !photo ? Find(windowPictures_[cur_]) : nullptr;
  if (photo) {  // (photomode) a photo's frame: the picture of SetPhotoWindow is its window, with a depth of its own
    TextureRec* color = Find(photoColor_);
    TextureRec* depth = Find(photoDepth_);
    D3D12_RESOURCE_BARRIER b[2];
    UINT n = 0;
    for (TextureRec* t : {color, depth}) {
      if (t->writable) continue;
      b[n++] = Transition(t->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          t->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
      t->writable = true;
    }
    if (n) list_->ResourceBarrier(n, b);
    rtv = RtvHandle(color->view);
    dsv = DsvHandle(depth->view);
    viewWidth = photoWidth_;
    viewHeight = photoHeight_;
  } else if (window) {  // frame generation: the slot's window picture drawn into (its pictures back from the presenter)
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b[2];
    UINT n = 0;
    b[n++] = Transition(window->resource.Get(), handedOver_[cur_] ? D3D12_RESOURCE_STATE_COPY_SOURCE : readable, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (handedOver_[cur_]) {
      if (TextureRec* between = Find(betweenPictures_[cur_])) b[n++] = Transition(between->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, readable);
      handedOver_[cur_] = false;
    }
    list_->ResourceBarrier(n, b);
    window->writable = true;
    rtv = RtvHandle(window->view);
  } else {
    D3D12_RESOURCE_BARRIER b = Transition(f.backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &b);
    rtv.ptr += static_cast<SIZE_T>(cur_) * rtvStep_;
  }
  list_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
  list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
  SetTargets(1, &rtv, &dsv);
  SetViewport(viewWidth, viewHeight);
  list_->SetGraphicsRootSignature(rootSignature_.Get());
  ID3D12DescriptorHeap* heaps[] = {ringHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  boundTextures_ = TableKey{};
  texturesDirty_ = true;
  passColor_ = passDepth_ = 0;
  if (window) passColor_ = windowPictures_[cur_].id;  // (readable again once the pass ends, as a target's)
  if (photo) {  // (the same for the photo's picture and depth)
    passColor_ = photoColor_.id;
    passDepth_ = photoDepth_.id;
  }
  boundStorage_ = {};
  std::fill(std::begin(constantsAt_), std::end(constantsAt_), D3D12_GPU_VIRTUAL_ADDRESS{0});
  buffersAt_[0] = buffersAt_[1] = 0;
  computePipeline_ = computeRoot_ = dispatched_ = false;
}

// [mcrec] (the pass's targets, kept to be set again on the lists after a parallel section)
void D3D12Device::SetTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
  list_->OMSetRenderTargets(count, rtv, FALSE, dsv);
  targetCount_ = count;
  targetRtv_ = count ? *rtv : D3D12_CPU_DESCRIPTOR_HANDLE{};
  targetDepth_ = dsv != nullptr;
  targetDsv_ = dsv ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

void D3D12Device::SetViewport(int width, int height) {
  viewportWidth_ = width;  // [mcrec]
  viewportHeight_ = height;
  D3D12_VIEWPORT vp = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  D3D12_RECT scissor = {0, 0, width, height};
  list_->RSSetViewports(1, &vp);
  list_->RSSetScissorRects(1, &scissor);
}

// (photomode) The picture a photo's frames draw their window into, and its depth (gfx.h).
bool D3D12Device::SetPhotoWindow(int width, int height) {
  if (inFrame_) return false;  // (between frames only)
  if (width < 0 || height < 0) {  // let go of
    photoOn_ = false;
    if (photoColor_) Destroy(photoColor_);
    if (photoDepth_) Destroy(photoDepth_);
    photoColor_ = photoDepth_ = {};
    photoWidth_ = photoHeight_ = 0;
    return true;
  }
  photoOn_ = false;
  if (width == 0 || height == 0) return true;  // (the window again; the picture kept)
  if (width > 16384 || height > 16384) return false;
  if (!Find(photoColor_) || !Find(photoDepth_) || width != photoWidth_ || height != photoHeight_) {
    if (photoColor_) Destroy(photoColor_);
    if (photoDepth_) Destroy(photoDepth_);
    TargetDesc td;
    td.width = width;
    td.height = height;
    td.format = Format::RGBA8;
    photoColor_ = CreateTarget(td);
    td.format = Format::D32;  // (cleared to 0: depth is reversed)
    photoDepth_ = CreateTarget(td);
    photoWidth_ = width;
    photoHeight_ = height;
    if (!photoColor_ || !photoDepth_) {
      SetPhotoWindow(-1, -1);
      return false;
    }
  }
  photoOn_ = true;
  return true;
}

void D3D12Device::BindPhotoWindow(bool clearColor, const float clearValue[4], bool clearDepth) {
  TextureRec* color = Find(photoColor_);
  TextureRec* depth = Find(photoDepth_);
  if (passColor_ != photoColor_.id || passDepth_ != photoDepth_.id) ReleasePassTargets();
  for (uint32_t& bound : boundTextures_.t)  // (a target is not read while it is drawn into)
    if (bound && (bound == photoColor_.id || bound == photoDepth_.id)) {
      bound = 0;
      texturesDirty_ = true;
    }
  for (TextureRec* t : {color, depth}) {
    if (t->writable) continue;
    D3D12_RESOURCE_BARRIER b = Transition(t->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          t->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &b);
    t->writable = true;
  }
  passColor_ = photoColor_.id;
  passDepth_ = photoDepth_.id;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(color->view), dsv = DsvHandle(depth->view);
  SetTargets(1, &rtv, &dsv);
  if (clearColor) list_->ClearRenderTargetView(rtv, clearValue, 0, nullptr);
  if (clearDepth) list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
  SetViewport(photoWidth_, photoHeight_);
}

// The targets of the pass that ends become readable as textures again.
void D3D12Device::ReleasePassTargets() {
  for (uint32_t id : {passColor_, passDepth_}) {
    if (!id || id >= textures_.size() || !textures_[id].used || !textures_[id].writable) continue;
    TextureRec& t = textures_[id];
    D3D12_RESOURCE_BARRIER b = Transition(t.resource.Get(), t.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &b);
    t.writable = false;
  }
  passColor_ = passDepth_ = 0;
}

void D3D12Device::ReleaseUnordered() {
  dispatched_ = false;
  if (!cardBuffers_.empty()) CardsReadable();  // [gpuscenery] (what dispatches wrote of card buffers, readable by the draws)
  if (unordered_.empty()) return;
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  for (uint32_t id : unordered_) {
    TextureRec& t = textures_[id];
    if (!t.used || !t.unordered) continue;
    barriers.push_back(Transition(t.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    t.unordered = false;
  }
  unordered_.clear();
  if (!barriers.empty()) list_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

void D3D12Device::Resolve(Texture from, Texture to) {
  TextureRec* src = Find(from);
  TextureRec* dst = Find(to);
  if (!inFrame_ || !src || !dst || !src->target || !dst->target || src->depth || dst->depth || src->samples <= 1 || dst->samples != 1 ||
      src->format != dst->format || src->width != dst->width || src->height != dst->height) {
    fprintf(stderr, "render: Direct3D 12: Resolve needs a multi-sample colour target and an ordinary one of the same size and format\n");
    return;
  }
  ReleasePassTargets();  // both are now in their readable state
  const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_BARRIER in[2] = {Transition(src->resource.Get(), readable, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                                  Transition(dst->resource.Get(), readable, D3D12_RESOURCE_STATE_RESOLVE_DEST)};
  list_->ResourceBarrier(2, in);
  list_->ResolveSubresource(dst->resource.Get(), 0, src->resource.Get(), 0, src->format);
  D3D12_RESOURCE_BARRIER out[2] = {Transition(src->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, readable),
                                   Transition(dst->resource.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, readable)};
  list_->ResourceBarrier(2, out);
}

void D3D12Device::BeginPass(const PassDesc& pass) {
  if (!inFrame_) return;
  if (pass.window && viewColor_) {  // [views] a view's own picture in place of the window (SetViewWindow)
    BeginPass(ViewWindowPass(pass));
    return;
  }
  ReleaseUnordered();  // (what dispatches wrote, readable by the pass's draws)
  if (pass.window && PhotoNow()) {  // (photomode) a photo's frame: its picture and depth in place of the window's
    BindPhotoWindow(pass.clearColor, pass.clearValue, pass.clearDepth);
    return;
  }
  if (pass.window) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(static_cast<uint32_t>(cur_));
    TextureRec* window = generating_ ? Find(windowPictures_[cur_]) : nullptr;
    if (window) {  // frame generation: the slot's window picture, as a target (not read while drawn into)
      const uint32_t id = windowPictures_[cur_].id;
      if (passColor_ != id || passDepth_) ReleasePassTargets();
      for (uint32_t& bound : boundTextures_.t)
        if (bound == id) {
          bound = 0;
          texturesDirty_ = true;
        }
      if (!window->writable) {
        D3D12_RESOURCE_BARRIER b = Transition(window->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->ResourceBarrier(1, &b);
        window->writable = true;
      }
      passColor_ = id;
      rtv = RtvHandle(window->view);
    } else {
      ReleasePassTargets();
    }
    SetTargets(1, &rtv, &dsvStart_);
    if (pass.clearColor) list_->ClearRenderTargetView(rtv, pass.clearValue, 0, nullptr);
    if (pass.clearDepth) list_->ClearDepthStencilView(dsvStart_, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    SetViewport(width_, height_);
    return;
  }
  TextureRec* color = Find(pass.color);
  TextureRec* depth = Find(pass.depth);
  if ((!color && !depth) || (pass.color.id && (!color || !color->target || color->depth)) ||
      (pass.depth.id && (!depth || !depth->target || !depth->depth || pass.depthLayer < 0 || pass.depthLayer >= depth->layers))) {
    fprintf(stderr, "render: Direct3D 12: BeginPass needs a colour target, a depth target, or both\n");
    return;
  }
  // a target that stays in use (the next layer of a shadow map) keeps its state
  if (passColor_ != pass.color.id || passDepth_ != pass.depth.id) ReleasePassTargets();
  // a target cannot be read while it is drawn into
  for (uint32_t& bound : boundTextures_.t)
    if (bound && (bound == pass.color.id || bound == pass.depth.id)) {
      bound = 0;
      texturesDirty_ = true;
    }
  for (TextureRec* t : {color, depth}) {
    if (!t || t->writable) continue;
    D3D12_RESOURCE_BARRIER b = Transition(t->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          t->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &b);
    t->writable = true;
  }
  passColor_ = pass.color.id;
  passDepth_ = pass.depth.id;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth ? DsvHandle(depth->view + static_cast<uint32_t>(pass.depthLayer)) : D3D12_CPU_DESCRIPTOR_HANDLE{};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = color ? RtvHandle(color->view) : D3D12_CPU_DESCRIPTOR_HANDLE{};
  SetTargets(color ? 1 : 0, color ? &rtv : nullptr, depth ? &dsv : nullptr);
  if (color && pass.clearColor) list_->ClearRenderTargetView(rtv, pass.clearValue, 0, nullptr);
  if (depth && pass.clearDepth) list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
  const TextureRec* sized = depth ? depth : color;
  SetViewport(sized->width, sized->height);
}

void D3D12Device::SetPipeline(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (rec && rec->pending) {  // (asked for in a batch and wanted before its end: made now)
    std::string why;
    if (!Realize(pipeline.id - 1, &why) && batchError_.empty()) batchError_ = why;
    rec = Find(pipeline);
  }
  if (rec && rec->background) {  // [snowhitch] (made ahead on the device's own thread: taken now)
    TakeBackground(pipeline.id - 1);
    rec = Find(pipeline);
  }
  if (!rec || !rec->state) return;
  list_->SetPipelineState(rec->state.Get());
  computePipeline_ = rec->compute;
  if (!rec->compute) list_->IASetPrimitiveTopology(rec->topology);
  set_.pipeline = rec->state.Get();  // [mcrec]
  set_.compute = rec->compute;
  if (!rec->compute) set_.topology = rec->topology;
}

void D3D12Device::SetConstants(int slot, const void* data, uint32_t size) {
  if (slot < 0 || slot >= kCbSlots || !size) return;
  uint32_t padded = (size + 255u) & ~255u;
  Span s = Allocate(padded, 256);
  if (!s.cpu) return;
  std::memcpy(s.cpu, data, size);
  list_->SetGraphicsRootConstantBufferView(static_cast<UINT>(slot), s.gpu);
  constantsAt_[slot] = s.gpu;  // (and for the dispatches, which have root arguments of their own)
}

void D3D12Device::SetTexture(int slot, Texture texture) {
  if (slot < 0 || slot >= kTexSlots) return;
  uint32_t id = Find(texture) ? texture.id : 0;
  if (boundTextures_.t[static_cast<size_t>(slot)] == id) return;
  boundTextures_.t[static_cast<size_t>(slot)] = id;
  texturesDirty_ = true;
}

void D3D12Device::BindTextures() {
  if (!texturesDirty_) return;
  texturesDirty_ = false;
  list_->SetGraphicsRootDescriptorTable(kCbSlots, TextureTable());
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::TextureTable() {
  FrameSlot& f = frames_[cur_];
  const uint32_t share = kRingDescriptors / kFrames;
  const uint32_t base = share * static_cast<uint32_t>(cur_);
  uint32_t at;
  auto it = f.tables.find(boundTextures_);
  if (it != f.tables.end()) {
    at = it->second;
  } else {
    if (f.descriptorsUsed + kTexSlots > share) {
      if (!warnedDescriptors_) fprintf(stderr, "render: Direct3D 12: more than %u texture sets in one frame\n", share / kTexSlots);
      warnedDescriptors_ = true;
      f.descriptorsUsed = 0;
      f.tables.clear();
    }
    at = base + f.descriptorsUsed;
    f.descriptorsUsed += kTexSlots;
    for (int i = 0; i < kTexSlots; ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE to = ringCpuStart_;
      to.ptr += static_cast<SIZE_T>(at + static_cast<uint32_t>(i)) * srvStep_;
      device_->CopyDescriptorsSimple(1, to, StagingHandle(boundTextures_.t[static_cast<size_t>(i)]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    f.tables.emplace(boundTextures_, at);
  }
  D3D12_GPU_DESCRIPTOR_HANDLE table = ringGpuStart_;
  table.ptr += static_cast<UINT64>(at) * srvStep_;
  return table;
}

void D3D12Device::SetStorage(int slot, Texture texture, int mip) {
  if (slot < 0 || slot >= kStorageSlots) return;
  const TextureRec* rec = Find(texture);
  boundStorage_[static_cast<size_t>(slot)] = rec && rec->storage && mip >= 0 && mip < rec->mips ? std::pair<uint32_t, int>{texture.id, mip}
                                                                                                : std::pair<uint32_t, int>{0, 0};
}

void D3D12Device::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (!inFrame_ || !computePipeline_ || !x || !y || !z) return;
  ReleasePassTargets();  // (the pass under way ends: its targets readable)
  // what this dispatch writes into the unordered-access state, what it reads out of it; and when the
  // last thing done was a dispatch, its writes finished first
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  for (const auto& s : boundStorage_) {
    if (!s.first || !textures_[s.first].used) continue;
    TextureRec& t = textures_[s.first];
    if (t.unordered) continue;
    barriers.push_back(Transition(t.resource.Get(), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    t.unordered = true;
    unordered_.push_back(s.first);
  }
  for (uint32_t id : boundTextures_.t) {
    if (!id || !textures_[id].used || !textures_[id].unordered) continue;
    bool written = false;
    for (const auto& s : boundStorage_) written = written || s.first == id;
    if (written) continue;  // (read and written at once: left to the shader; gfx.h says not to)
    TextureRec& t = textures_[id];
    barriers.push_back(Transition(t.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable));
    t.unordered = false;
    unordered_.erase(std::remove(unordered_.begin(), unordered_.end(), id), unordered_.end());
  }
  // [gpuscenery] the card buffers in the shader buffers' slots: written (u0, u1 of space1), so in the unordered-access state
  BufferRec* cards[2] = {};
  for (uint32_t id : cardBuffers_) {
    BufferRec* rec = Find(Buffer{id});
    if (!rec) continue;
    for (int i = 0; i < 2; ++i)
      if (buffersAt_[i] && buffersAt_[i] == rec->gpu) cards[i] = rec;
  }
  for (BufferRec* rec : cards)
    if (rec) CardState(*rec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &barriers);
  if (dispatched_) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // (every storage target: the dispatch before has written all of them)
    barriers.push_back(b);
  }
  if (!barriers.empty()) list_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
  if (!computeRoot_) {
    list_->SetComputeRootSignature(rootSignature_.Get());
    computeRoot_ = true;
  }
  for (int i = 0; i < kCbSlots; ++i)
    if (constantsAt_[i]) list_->SetComputeRootConstantBufferView(static_cast<UINT>(i), constantsAt_[i]);
  for (int i = 0; i < 2; ++i)
    if (buffersAt_[i]) list_->SetComputeRootShaderResourceView(static_cast<UINT>(kCbSlots + 1 + i), buffersAt_[i]);
  for (int i = 0; i < 2; ++i)  // [gpuscenery]
    if (cards[i]) list_->SetComputeRootUnorderedAccessView(static_cast<UINT>(kCbSlots + 4 + i), cards[i]->gpu);
  list_->SetComputeRootDescriptorTable(kCbSlots, TextureTable());
  {  // the storage views, copied into this frame's share of the shader-visible heap
    FrameSlot& f = frames_[cur_];
    const uint32_t share = kRingDescriptors / kFrames;
    if (f.descriptorsUsed + kStorageSlots > share) {
      f.descriptorsUsed = 0;
      f.tables.clear();
    }
    const uint32_t at = share * static_cast<uint32_t>(cur_) + f.descriptorsUsed;
    f.descriptorsUsed += kStorageSlots;
    for (int i = 0; i < kStorageSlots; ++i) {
      const auto& s = boundStorage_[static_cast<size_t>(i)];
      const uint32_t view = s.first && textures_[s.first].used ? textures_[s.first].uav + static_cast<uint32_t>(s.second) : 0;
      D3D12_CPU_DESCRIPTOR_HANDLE to = ringCpuStart_;
      to.ptr += static_cast<SIZE_T>(at + static_cast<uint32_t>(i)) * srvStep_;
      device_->CopyDescriptorsSimple(1, to, UavHandle(view), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE table = ringGpuStart_;
    table.ptr += static_cast<UINT64>(at) * srvStep_;
    list_->SetComputeRootDescriptorTable(kCbSlots + 3, table);
  }
  list_->Dispatch(x, y, z);
  dispatched_ = true;
}

Com<ID3D12Resource> D3D12Device::RayBuffer(uint64_t size, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC rd = BufferResourceDesc(size);
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  Com<ID3D12Resource> r;
  const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(r.Put()));
  if (FAILED(hr)) Complain("making a ray tracing structure's buffer", hr);
  return r;
}

bool D3D12Device::GrowScratch(uint64_t size) {
  if (size <= scratchSize_ && scratch_) return true;
  if (scratch_) Retire(scratch_.Get());  // (a build of a frame still on the card may use it)
  const uint64_t room = (std::max(size, scratchSize_ + scratchSize_ / 2) + 65535) / 65536 * 65536;
  scratch_ = RayBuffer(room, D3D12_RESOURCE_STATE_COMMON);  // (a buffer starts in the common state and is promoted)
  scratchSize_ = scratch_ ? room : 0;
  return static_cast<bool>(scratch_);
}

static uint64_t RayAligned(uint64_t n) {
  const uint64_t a = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
  return (n + a - 1) / a * a;
}

Structure D3D12Device::CreateGeometry(const GeometryPart* parts, int count) {
  std::unique_ptr<Prepared> p = DescribeGeometry(parts, count);  // [warmtarget] (the three steps, one after the other)
  SizeGeometry(p.get());
  return FinishGeometry(std::move(p));
}

// [warmtarget] The steps of CreateGeometry (gfx.h, DescribeGeometry): the parts as the driver takes them (their buffers'
// places, from the device's tables: the frames' thread); the driver's sizes and the structure's buffer (any thread: the
// device is free-threaded for these); its handle, and its build queued for the next BuildScene (the frames' thread).
namespace {
struct PreparedGeometry12 final : Device::Prepared {
  StructureRec rec;
  bool ok = false;  // (described, and sized)
};
}  // namespace

std::unique_ptr<Device::Prepared> D3D12Device::DescribeGeometry(const GeometryPart* parts, int count) {
  auto prepared = std::make_unique<PreparedGeometry12>();
  if (!rayTracing_ || !parts || count <= 0) return prepared;
  StructureRec& rec = prepared->rec;
  for (int i = 0; i < count; ++i) {
    const GeometryPart& p = parts[i];
    const BufferRec* v = Find(p.vertices);
    if (!v || v->dynamic || v->kind != BufferKind::Vertex || !p.vertexCount) return prepared;
    D3D12_RAYTRACING_GEOMETRY_DESC g = {};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = p.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    g.Triangles.VertexBuffer.StartAddress = v->gpu + static_cast<uint64_t>(p.firstVertex) * v->stride + p.positionOffset;
    g.Triangles.VertexBuffer.StrideInBytes = v->stride;
    g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexCount = p.vertexCount;
    if (p.indices) {
      const BufferRec* ix = Find(p.indices);
      if (!ix || ix->dynamic || ix->kind != BufferKind::Index || !p.indexCount) return prepared;
      g.Triangles.IndexBuffer = ix->gpu + static_cast<uint64_t>(p.firstIndex) * ix->stride;
      g.Triangles.IndexFormat = ix->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
      g.Triangles.IndexCount = p.indexCount;
    }
    rec.parts.push_back(g);
  }
  prepared->ok = true;
  return prepared;
}

void D3D12Device::SizeGeometry(Prepared* prepared) {
  auto* p = dynamic_cast<PreparedGeometry12*>(prepared);
  if (!p || !p->ok) return;
  p->ok = false;
  StructureRec& rec = p->rec;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
  in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  in.NumDescs = static_cast<UINT>(rec.parts.size());
  in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  in.pGeometryDescs = rec.parts.data();
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
  if (!info.ResultDataMaxSizeInBytes) return;
  rec.size = RayAligned(info.ResultDataMaxSizeInBytes);
  rec.scratch = RayAligned(std::max<uint64_t>(info.ScratchDataSizeInBytes, 1));
  rec.buffer = RayBuffer(rec.size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  p->ok = static_cast<bool>(rec.buffer);
}

Structure D3D12Device::FinishGeometry(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedGeometry12*>(prepared.get());
  if (!p) return Device::FinishGeometry(std::move(prepared));  // (one kept whole: CreateGeometry)
  if (!p->ok) return {};
  StructureRec rec = std::move(p->rec);
  rec.used = true;
  uint32_t index;
  if (!freeStructures_.empty()) {
    index = freeStructures_.back();
    freeStructures_.pop_back();
    structures_[index] = std::move(rec);
  } else {
    index = static_cast<uint32_t>(structures_.size());
    structures_.push_back(std::move(rec));
  }
  pendingGeometry_.push_back(index);
  return {index + 1};
}

void D3D12Device::BuildGeometry() {
  if (pendingGeometry_.empty()) return;
  // (built side by side in the scratch room, in batches of at most 64 MB of it, each batch waited for)
  constexpr uint64_t kBatch = 64ull << 20;
  uint64_t largest = 0;
  for (uint32_t i : pendingGeometry_)
    if (structures_[i].used && !structures_[i].parts.empty()) largest = std::max(largest, structures_[i].scratch);
  if (!largest || !GrowScratch(std::max(largest, std::min<uint64_t>(kBatch, scratchSize_)))) {
    pendingGeometry_.clear();
    return;
  }
  D3D12_RESOURCE_BARRIER wait = {};
  wait.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uint64_t at = 0;
  for (uint32_t i : pendingGeometry_) {
    StructureRec& s = structures_[i];
    if (!s.used || s.parts.empty()) continue;
    if (at + s.scratch > scratchSize_) {
      list_->ResourceBarrier(1, &wait);
      at = 0;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
    b.DestAccelerationStructureData = s.buffer->GetGPUVirtualAddress();
    b.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    b.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    b.Inputs.NumDescs = static_cast<UINT>(s.parts.size());
    b.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    b.Inputs.pGeometryDescs = s.parts.data();
    b.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + at;
    list4_->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    at += s.scratch;
    s.parts.clear();
    s.parts.shrink_to_fit();
  }
  pendingGeometry_.clear();
  list_->ResourceBarrier(1, &wait);
}

void D3D12Device::BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) { MakeScene(scene, instances, count, 0); }

// [cardcost] (gfx.h) Built to be refitted, and refitted in place where it was so built over as many instances as now.
bool D3D12Device::RefitScene(Structure* scene, const SceneInstance* instances, uint32_t count, bool rebuild) {
  return MakeScene(scene, instances, count, rebuild ? 1 : 2);
}

// The scene's structure over the instances: `how` 0 built (BuildScene), 1 built so that it can be refitted, 2 refitted in
// place where it was so built over as many instances (else built so). True where it was made (built or refitted).
bool D3D12Device::MakeScene(Structure* scene, const SceneInstance* instances, uint32_t count, int how) {
  if (!rayTracing_ || !inFrame_ || !scene) return false;
  ReleasePassTargets();  // (the pass under way ends)
  BuildGeometry();
  // the instances, in this frame's upload memory
  const Span s = Allocate(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * std::max<uint32_t>(count, 1), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
  if (!s.cpu) return false;
  auto* out = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(s.cpu);
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const StructureRec* g = instances ? Find(instances[i].geometry) : nullptr;
    if (!g || g->scene) continue;
    D3D12_RAYTRACING_INSTANCE_DESC d = {};
    std::memcpy(d.Transform, instances[i].transform, sizeof d.Transform);
    d.InstanceID = instances[i].id & 0xffffffu;
    d.InstanceMask = instances[i].mask;
    d.AccelerationStructure = g->buffer->GetGPUVirtualAddress();
    out[n++] = d;
  }
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
  in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  if (how > 0) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;  // [cardcost]
  in.NumDescs = n;
  in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  in.InstanceDescs = s.gpu;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
  device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
  StructureRec* rec = Find(*scene);
  // [cardcost] refitted in place: the structure as it was built (to be refitted, over as many instances), its tree kept
  if (how == 2 && rec && rec->scene && rec->buffer && rec->refitCount == n &&
      GrowScratch(RayAligned(std::max<uint64_t>(info.UpdateScratchDataSizeInBytes, 1)))) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
    b.DestAccelerationStructureData = b.SourceAccelerationStructureData = rec->buffer->GetGPUVirtualAddress();
    b.Inputs = in;
    b.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    b.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
    list4_->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER wait = {};
    wait.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list_->ResourceBarrier(1, &wait);
    computeRoot_ = false;
    texturesDirty_ = true;
    return true;
  }
  if (!rec) {  // (the first time: the scene's record, and its view's slot among the textures')
    const uint32_t texture = NewTextureIndex();
    if (!texture) return false;
    textures_[texture] = TextureRec{};
    textures_[texture].used = true;
    StructureRec r;
    r.used = r.scene = true;
    r.texture = texture;
    uint32_t index;
    if (!freeStructures_.empty()) {
      index = freeStructures_.back();
      freeStructures_.pop_back();
      structures_[index] = std::move(r);
    } else {
      index = static_cast<uint32_t>(structures_.size());
      structures_.push_back(std::move(r));
    }
    *scene = {index + 1};
    rec = &structures_[index];
  }
  if (!rec->scene) return false;
  rec->refitCount = ~uint64_t{0};  // [cardcost] (built now: refitted later only where built so, below)
  const uint64_t need = RayAligned(std::max<uint64_t>(info.ResultDataMaxSizeInBytes, 256));
  if (rec->size < need) {  // (made, or grown by a half more than it needs)
    if (rec->buffer) Retire(rec->buffer.Get());
    const uint64_t room = RayAligned(need + need / 2);
    rec->buffer = RayBuffer(room, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    rec->size = rec->buffer ? room : 0;
    if (!rec->buffer) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.RaytracingAccelerationStructure.Location = rec->buffer->GetGPUVirtualAddress();
    device_->CreateShaderResourceView(nullptr, &v, StagingHandle(rec->texture));
    frames_[cur_].tables.clear();  // (tables made this frame hold its view as it was)
    texturesDirty_ = true;
  }
  if (!GrowScratch(RayAligned(std::max<uint64_t>(info.ScratchDataSizeInBytes, 1)))) return false;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
  b.DestAccelerationStructureData = rec->buffer->GetGPUVirtualAddress();
  b.Inputs = in;
  b.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
  list4_->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
  D3D12_RESOURCE_BARRIER wait = {};
  wait.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  list_->ResourceBarrier(1, &wait);
  // (what was set before is set again: the roots, the textures' table)
  computeRoot_ = false;
  texturesDirty_ = true;
  if (how > 0) rec->refitCount = n;  // [cardcost]
  return true;
}

void D3D12Device::SetScene(int slot, Structure scene) {
  if (slot < 0 || slot >= kTexSlots) return;
  const StructureRec* rec = Find(scene);
  const uint32_t id = rec && rec->scene && rec->buffer ? rec->texture : 0;
  if (boundTextures_.t[static_cast<size_t>(slot)] == id) return;
  boundTextures_.t[static_cast<size_t>(slot)] = id;
  texturesDirty_ = true;
}

void D3D12Device::Destroy(Structure structure) {
  StructureRec* rec = Find(structure);
  if (!rec) return;
  Retire(rec->buffer.Get());
  if (rec->scene && rec->texture) {
    for (uint32_t& t : boundTextures_.t)
      if (t == rec->texture) t = 0;
    textures_[rec->texture] = TextureRec{};
    device_->CopyDescriptorsSimple(1, StagingHandle(rec->texture), StagingHandle(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    freeTextures_.push_back(rec->texture);
    texturesDirty_ = true;
  }
  const uint32_t index = structure.id - 1;
  pendingGeometry_.erase(std::remove(pendingGeometry_.begin(), pendingGeometry_.end(), index), pendingGeometry_.end());
  *rec = StructureRec{};
  freeStructures_.push_back(index);
}

void D3D12Device::SetVertexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Vertex || (rec->dynamic && rec->frame != frameNumber_)) return;
  D3D12_VERTEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride};
  list_->IASetVertexBuffers(0, 1, &view);
  set_.vertices = true;  // [mcrec]
  set_.vertexView = view;
}

void D3D12Device::SetIndexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != frameNumber_)) return;
  D3D12_INDEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
  list_->IASetIndexBuffer(&view);
  set_.indices = true;  // [mcrec]
  set_.indexView = view;
}

void D3D12Device::SetShaderBuffer(Buffer buffer, int slot) {
  BufferRec* rec = Find(buffer);
  if (!rec || slot < 0 || slot > 1) return;
  // a dynamic Shader buffer of this frame; or a static buffer of any kind, read where it lies in its block (the
  // block is in the common state, promoted to a read state by this use as by a draw's or a structure build's)
  if (rec->dynamic ? rec->kind != BufferKind::Shader || rec->frame != frameNumber_ : !rec->gpu) return;
  list_->SetGraphicsRootShaderResourceView(static_cast<UINT>(kCbSlots + 1 + slot), rec->gpu);
  buffersAt_[slot] = rec->gpu;
}

void D3D12Device::Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) {
  ++draws_;
  BindTextures();
  list_->DrawInstanced(vertexCount, instances, firstVertex, 0);
}

void D3D12Device::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) {
  ++draws_;
  BindTextures();
  list_->DrawIndexedInstanced(indexCount, instances, firstIndex, baseVertex, 0);
}

void D3D12Device::UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) {
  TextureRec* rec = Find(texture);
  if (!rec || rec->target || rec->storage || !inFrame_ || width <= 0 || height <= 0 || !data || rec->pixelBytes <= 0) return;
  const D3D12_RESOURCE_DESC rd = rec->resource->GetDesc();
  if (rd.MipLevels != 1 || rd.DepthOrArraySize != 1 || x < 0 || y < 0 || static_cast<UINT64>(x + width) > rd.Width ||
      static_cast<UINT>(y + height) > rd.Height)
    return;
  const UINT unit = static_cast<UINT>(rec->pixelBytes);
  const UINT row = static_cast<UINT>(width) * unit;
  const UINT rowPitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  Span s = Allocate(static_cast<size_t>(rowPitch) * static_cast<size_t>(height), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  if (!s.cpu) return;
  for (int r = 0; r < height; ++r)
    std::memcpy(s.cpu + static_cast<size_t>(r) * rowPitch, static_cast<const uint8_t*>(data) + static_cast<size_t>(r) * pitch, row);
  const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_BARRIER b = Transition(rec->resource.Get(), readable, D3D12_RESOURCE_STATE_COPY_DEST);
  list_->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
  to.pResource = rec->resource.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.pResource = s.resource;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Offset = s.offset;
  from.PlacedFootprint.Footprint.Format = rd.Format;
  from.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
  from.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
  from.PlacedFootprint.Footprint.Depth = 1;
  from.PlacedFootprint.Footprint.RowPitch = rowPitch;
  list_->CopyTextureRegion(&to, static_cast<UINT>(x), static_cast<UINT>(y), 0, &from, nullptr);
  b = Transition(rec->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable);
  list_->ResourceBarrier(1, &b);
}

void D3D12Device::EndFrame(std::vector<uint8_t>* capture, bool present) {
  drawsLast_ = draws_;
  draws_ = 0;
  FrameSlot& f = frames_[cur_];
  ReleasePassTargets();
  ReleaseUnordered();
  // frame generation: the picture between this frame and the one before made (it may dispatch and draw); what is
  // read back is the window's picture (or that one), which is readable now
  const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  // (photomode: a photo's frame, drawn into the picture of SetPhotoWindow, makes no picture between and is never shown)
  TextureRec* photo = PhotoNow() ? Find(photoColor_) : nullptr;
  TextureRec* window = generating_ && !photo ? Find(windowPictures_[cur_]) : nullptr;
  TextureRec* between = generating_ && !photo ? Find(betweenPictures_[cur_]) : nullptr;
  const bool made = window && between && makeBetween_ && makeBetween_();
  if (window) {
    ReleasePassTargets();
    ReleaseUnordered();
    // (found again: what `make` made, its first time, may have moved the texture records)
    window = Find(windowPictures_[cur_]);
    between = Find(betweenPictures_[cur_]);
  }
  ID3D12Resource* shown = photo ? photo->resource.Get() : window ? (captureBetween && made ? between : window)->resource.Get() : f.backBuffer.Get();
  const int shownWidth = photo ? photoWidth_ : width_, shownHeight = photo ? photoHeight_ : height_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  if (capture) {
    D3D12_RESOURCE_DESC bd = shown->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bd, 0, 1, 0, &layout, nullptr, nullptr, &total);
    if (!readback_ || readbackSize_ < total) {
      Retire(readback_.Get());
      D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
      D3D12_RESOURCE_DESC rd = BufferResourceDesc(total);
      HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(readback_.Put()));
      readbackSize_ = SUCCEEDED(hr) ? total : 0;
      if (FAILED(hr)) Complain("creating the read-back buffer", hr);
    }
  }
  if (photo) {  // (photomode) its picture read back, then readable again; the window's own back buffer untouched
    if (capture && readback_) {
      D3D12_RESOURCE_BARRIER b = Transition(shown, readable, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list_->ResourceBarrier(1, &b);
      D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
      to.pResource = readback_.Get();
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = layout;
      from.pResource = shown;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      b = Transition(shown, D3D12_RESOURCE_STATE_COPY_SOURCE, readable);
      list_->ResourceBarrier(1, &b);
    }
  } else if (window) {  // both pictures left as copies' sources: the presenter copies them into the swap chain
    D3D12_RESOURCE_BARRIER b[2] = {Transition(window->resource.Get(), readable, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                   Transition(between->resource.Get(), readable, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list_->ResourceBarrier(2, b);
    handedOver_[cur_] = true;
    if (capture && readback_) {
      D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
      to.pResource = readback_.Get();
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = layout;
      from.pResource = shown;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
  } else if (capture && readback_) {
    D3D12_RESOURCE_BARRIER b = Transition(f.backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = readback_.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = layout;
    from.pResource = f.backBuffer.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    b = Transition(f.backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &b);
  } else {
    D3D12_RESOURCE_BARRIER b = Transition(f.backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &b);
  }
  if (timeQueries_ && !f.marks.empty()) {  // the frame's end, and its times copied out for later
    const UINT first = static_cast<UINT>(cur_) * kMarks;
    list_->EndQuery(timeQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + static_cast<UINT>(f.marks.size()));
    f.marks.push_back("(end)");
    list_->ResolveQueryData(timeQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, static_cast<UINT>(f.marks.size()), timeReadback_.Get(),
                            static_cast<UINT64>(first) * 8);
  }
  if (latency_ && latencyQueries_) {  // (the latency limiter: the frame's last timestamp, and both copied out)
    list_->EndQuery(latencyQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * cur_ + 1));
    list_->ResolveQueryData(latencyQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * cur_), 2, latencyTimes_.Get(),
                            static_cast<UINT64>(cur_) * 16);
    latencySlots_[cur_].timed = true;
  }
  list_->Close();
  FlushUploads();  // (what this frame draws with is on the card before it)
  order_.push_back(list_.Get());  // [mcrec] (with the lists of the parallel sections before it, in their order)
  queue_->ExecuteCommandLists(static_cast<UINT>(order_.size()), order_.data());
  order_.clear();
  if (latency_ || measureLatency) latencySlots_[cur_].handed = Now();
  if (present && !window && !photo) {
    HRESULT hr = swapChain_->Present(vsync_ ? Interval() : 0, !vsync_ && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) Complain("Present", hr);
    latencyTicket_ = false;  // (the limiter: the Present used the wait on the swap chain's object)
    if (measureLatency) {
      latencyRecord_.Presented(chainBase_ + LastPresent(), latencySlots_[cur_].read);
      TakeStatistics();
    }
  }
  if (infoQueue_) {  // --debug: the validation layer's messages, which otherwise only a debugger sees
    std::vector<char> buf;
    for (UINT64 i = 0, n = infoQueue_->GetNumStoredMessages(); i < n; ++i) {
      SIZE_T size = 0;
      infoQueue_->GetMessage(i, nullptr, &size);
      buf.resize(size);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
      if (SUCCEEDED(infoQueue_->GetMessage(i, m, &size))) fprintf(stderr, "render: D3D12 says: %s\n", m->pDescription);
    }
    infoQueue_->ClearStoredMessages();
  }
  f.fence = fenceNext_++;
  queue_->Signal(fence_.Get(), f.fence);
  inFrame_ = false;
  if (present && window) {  // handed to the presenter, which shows it once the card has finished it
    {
      std::lock_guard<std::mutex> lock(presentMutex_);
      slotShowing_[cur_] = true;
      presentJobs_.push_back({cur_, f.fence, made ? between->resource.Get() : nullptr, window->resource.Get(), vsync_, latencySlots_[cur_].read});
    }
    presentWake_.notify_one();
  }

  if (capture) {
    capture->clear();
    if (!readback_) return;
    WaitFence(f.fence);
    void* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readbackSize_)};
    if (FAILED(readback_->Map(0, &range, &mapped))) return;
    capture->resize(static_cast<size_t>(shownWidth) * static_cast<size_t>(shownHeight) * 4);
    for (int y = 0; y < shownHeight; ++y)
      std::memcpy(capture->data() + static_cast<size_t>(y) * static_cast<size_t>(shownWidth) * 4,
                  static_cast<const uint8_t*>(mapped) + layout.Offset + static_cast<size_t>(y) * layout.Footprint.RowPitch,
                  static_cast<size_t>(shownWidth) * 4);
    D3D12_RANGE none = {0, 0};
    readback_->Unmap(0, &none);
  }
}

void D3D12Device::Mark(const char* name) {
  FrameSlot& f = frames_[cur_];
  if (!timeQueries_ || !inFrame_ || f.marks.size() >= kMarks - 1) return;
  list_->EndQuery(timeQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(cur_) * kMarks + static_cast<UINT>(f.marks.size()));
  f.marks.push_back(name);
}

void D3D12Device::TakeTimes(std::vector<std::pair<const char*, double>>* out) { *out = lastTimes_; }


// A recorder's texture set in the frame's share of the shader-visible heap (as TextureTable makes the device's own): made
// once a frame for each set it binds, its room taken from the share by many recorders at once.
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::RecorderTable(int index, const TableKey& key) {
  FrameSlot& f = frames_[cur_];
  std::unordered_map<TableKey, uint32_t, TableHash>& tables = f.recorders[static_cast<size_t>(index)].tables;
  const uint32_t share = kRingDescriptors / kFrames;
  const uint32_t base = share * static_cast<uint32_t>(cur_);
  uint32_t at;
  auto it = tables.find(key);
  if (it != tables.end()) {
    at = it->second;
  } else {
    uint32_t used = parallelDescriptors_.fetch_add(kTexSlots);
    if (used + kTexSlots > share) {  // (the share is full: as the device's own, from its start again)
      if (!warnedParallel_.exchange(true)) fprintf(stderr, "render: Direct3D 12: more than %u texture sets in one frame\n", share / kTexSlots);
      used = 0;
    }
    at = base + used;
    for (int i = 0; i < kTexSlots; ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE to = ringCpuStart_;
      to.ptr += static_cast<SIZE_T>(at + static_cast<uint32_t>(i)) * srvStep_;
      device_->CopyDescriptorsSimple(1, to, StagingHandle(key.t[static_cast<size_t>(i)]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    tables.emplace(key, at);
  }
  D3D12_GPU_DESCRIPTOR_HANDLE table = ringGpuStart_;
  table.ptr += static_cast<UINT64>(at) * srvStep_;
  return table;
}

// The next of the frame's further lists, recording (its allocator the slot's next).
bool D3D12Device::NextList(ID3D12GraphicsCommandList** list, ID3D12GraphicsCommandList4** list4) {
  FrameSlot& f = frames_[cur_];
  if (f.moreUsed == f.moreAllocators.size()) {
    Com<ID3D12CommandAllocator> a;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.Put())))) return false;
    f.moreAllocators.push_back(std::move(a));
  }
  ID3D12CommandAllocator* allocator = f.moreAllocators[f.moreUsed].Get();
  if (moreListsUsed_ == moreLists_.size()) {
    Com<ID3D12GraphicsCommandList> l;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(l.Put())))) return false;
    Com<ID3D12GraphicsCommandList4> l4;
    if (rayTracing_ && FAILED(l->QueryInterface(IID_PPV_ARGS(l4.Put())))) return false;
    moreLists_.push_back(std::move(l));  // (made recording)
    moreLists4_.push_back(std::move(l4));
  } else if (FAILED(moreLists_[moreListsUsed_]->Reset(allocator, nullptr))) {
    return false;
  }
  *list = moreLists_[moreListsUsed_].Get();
  *list4 = moreLists4_[moreListsUsed_].Get();
  ++moreListsUsed_;
  ++f.moreUsed;
  return true;
}

// A list made ready to go on where the frame's own list is: the root signature, the heap, the pass's targets and viewport,
// and what the draws are given (the texture table is bound by the first draw).
void D3D12Device::SetUp(ID3D12GraphicsCommandList* list, const DrawSet& s) {
  list->SetGraphicsRootSignature(rootSignature_.Get());
  ID3D12DescriptorHeap* heaps[] = {ringHeap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->OMSetRenderTargets(targetCount_, targetCount_ ? &targetRtv_ : nullptr, FALSE, targetDepth_ ? &targetDsv_ : nullptr);
  const D3D12_VIEWPORT vp = {0, 0, static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), 0, 1};
  const D3D12_RECT scissor = {0, 0, viewportWidth_, viewportHeight_};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &scissor);
  if (s.pipeline) list->SetPipelineState(s.pipeline);
  list->IASetPrimitiveTopology(s.topology);
  for (int i = 0; i < kCbSlots; ++i)
    if (s.constants[i]) list->SetGraphicsRootConstantBufferView(static_cast<UINT>(i), s.constants[i]);
  for (int i = 0; i < 2; ++i)
    if (s.buffers[i]) list->SetGraphicsRootShaderResourceView(static_cast<UINT>(kCbSlots + 1 + i), s.buffers[i]);
  if (s.vertices) list->IASetVertexBuffers(0, 1, &s.vertexView);
  if (s.indices) list->IASetIndexBuffer(&s.indexView);
}

bool D3D12Device::BeginParallel(int count) {
  if (!inFrame_ || parallel_ || count < 1 || count > kMostRecorders) return false;
  FrameSlot& f = frames_[cur_];
  // the lists first: the recorders' and the frame's own after them (none handed out when one cannot be made)
  ID3D12GraphicsCommandList* lists[kMostRecorders + 1];
  ID3D12GraphicsCommandList4* lists4[kMostRecorders + 1];
  for (int i = 0; i <= count; ++i)
    if (!NextList(&lists[i], &lists4[i])) {
      Complain("making a command list for a recorder", E_FAIL);
      for (int k = 0; k < i; ++k) lists[k]->Close();  // (never handed to the card: reset again next time)
      return false;
    }
  while (recorders_.size() < static_cast<size_t>(count)) recorders_.push_back(std::make_unique<ParallelRecorder>(this, static_cast<int>(recorders_.size())));
  if (f.recorders.size() < static_cast<size_t>(count)) f.recorders.resize(static_cast<size_t>(count));
  // what the draws are given now, the recorders' start
  DrawSet now = set_;
  std::copy(std::begin(constantsAt_), std::end(constantsAt_), now.constants);
  now.buffers[0] = buffersAt_[0];
  now.buffers[1] = buffersAt_[1];
  now.textures = boundTextures_;
  list_->Close();
  order_.push_back(list_.Get());
  for (int i = 0; i < count; ++i) {
    SetUp(lists[i], now);
    recorders_[static_cast<size_t>(i)]->Start(lists[i], now);
  }
  afterList_ = lists[count];
  afterList4_ = lists4[count];
  parallelDescriptors_.store(f.descriptorsUsed);
  parallel_ = count;
  return true;
}

Recorder* D3D12Device::Parallel(int index) {
  return index >= 0 && index < parallel_ ? recorders_[static_cast<size_t>(index)].get() : nullptr;
}

void D3D12Device::EndParallel() {
  if (!parallel_) return;
  for (int i = 0; i < parallel_; ++i) {
    ParallelRecorder& r = *recorders_[static_cast<size_t>(i)];
    r.list_->Close();
    order_.push_back(r.list_);
    draws_ += r.draws_;
  }
  // the frame's own list again, set as the last recorder left things
  const DrawSet& last = recorders_[static_cast<size_t>(parallel_ - 1)]->set_;
  set_ = last;
  std::copy(std::begin(last.constants), std::end(last.constants), constantsAt_);
  buffersAt_[0] = last.buffers[0];
  buffersAt_[1] = last.buffers[1];
  boundTextures_ = last.textures;
  texturesDirty_ = true;
  computePipeline_ = last.compute;
  computeRoot_ = false;
  const uint32_t share = kRingDescriptors / kFrames;
  frames_[cur_].descriptorsUsed = std::min(parallelDescriptors_.load(), share);
  for (size_t i = 0; i < moreLists_.size(); ++i)  // (the Com that keeps it: list_ is always one)
    if (moreLists_[i].Get() == afterList_) {
      list_ = moreLists_[i];
      list4_ = moreLists4_[i];
      break;
    }
  SetUp(list_.Get(), set_);
  afterList_ = nullptr;
  afterList4_ = nullptr;
  parallel_ = 0;
}

uint64_t D3D12Device::MemoryInUse() {
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (!adapter3_ || FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return 0;
  return info.CurrentUsage;
}

// (photofix2) the system's budget for this program on the card, and what it uses of it; the budget no more than this
// program could still take of what all programs together leave free on the card (card_memory.h: the system's own
// budget barely heeds the others)
bool D3D12Device::MemoryBudget(uint64_t* budget, uint64_t* used) {
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (!adapter3_ || FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || !info.Budget) return false;
  *budget = info.Budget;
  *used = info.CurrentUsage;
  DXGI_ADAPTER_DESC1 desc = {};
  uint64_t all = 0;
  if (SUCCEEDED(adapter3_->GetDesc1(&desc)) && desc.DedicatedVideoMemory > 0 &&
      CardMemoryUsedByAll(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart, &all)) {
    const uint64_t card = desc.DedicatedVideoMemory, left = card > all ? card - all : 0;
    *budget = std::min<uint64_t>(*budget, info.CurrentUsage + left);
  }
  return true;
}

}  // namespace

std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& desc, std::string* error) {
  auto d = std::make_unique<D3D12Device>();
  if (!d->Init(desc, error)) return nullptr;
  return d;
}

}  // namespace render::gfx

#endif  // _WIN32
