// Direct3D 12 backend of gfx.h.
//
// How it is put together:
//  - One root signature for every pipeline: four root constant-buffer views (b0..b3), one table of 32
//    textures (t0..t31) and the fixed samplers.
//  - Per back buffer ("frame slot"): a command allocator, a ring of upload pages for this frame's
//    constants and dynamic buffers, a share of the shader-visible descriptor heap, and the list of
//    resources destroyed while it was recorded. All of it is reused when the slot comes round again,
//    after the GPU has finished that frame.
//  - Every texture has one descriptor in a CPU-side heap; a draw's 32 textures are copied from there
//    into the frame's share of the shader-visible heap (once per distinct set per frame).
//  - Static buffers and textures are uploaded at creation through a separate command list, waiting for
//    the GPU each time (load time only).
#include "gfx.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace render::gfx {

bool IsBlockCompressed(Format f) { return f >= Format::BC1 && f <= Format::BC5; }

int BytesPerPixelOrBlock(Format f) {
  switch (f) {
    case Format::R8: case Format::A8: return 1;
    case Format::RG8: case Format::R16: case Format::R16F: return 2;
    case Format::RGBA8: case Format::RGBA8_sRGB: case Format::BGRA8: case Format::BGRA8_sRGB: case Format::R32F: case Format::D32: return 4;
    case Format::RGBA16F: return 8;
    case Format::BC1: case Format::BC1_sRGB: case Format::BC4: return 8;
    case Format::BC2: case Format::BC2_sRGB: case Format::BC3: case Format::BC3_sRGB: case Format::BC5: return 16;
    default: return 0;
  }
}

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
constexpr int kTexSlots = 32;
constexpr int kCbSlots = 4;
constexpr uint32_t kMaxTextures = 16384;       // CPU-side descriptors, one per texture (slot 0: the null texture)
constexpr uint32_t kRingDescriptors = kFrames * kTexSlots * 4096;  // shader-visible, split between the frame slots
constexpr uint32_t kMaxColorTargets = 64;      // render-target views beyond the window's
constexpr uint32_t kMaxDepthViews = 128;       // depth views beyond the window's (one per layer of a depth target)
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
    case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
    case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
    case Format::BC1_sRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case Format::BC2: return DXGI_FORMAT_BC2_UNORM;
    case Format::BC2_sRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
    case Format::BC3_sRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case Format::BC4: return DXGI_FORMAT_BC4_UNORM;
    case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
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
};

struct BufferRec {
  bool used = false;
  BufferKind kind = BufferKind::Vertex;
  bool dynamic = false;
  uint32_t stride = 0;
  uint32_t size = 0;
  Com<ID3D12Resource> resource;        // static
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;   // static: the resource; dynamic: this frame's span
  uint64_t frame = 0;                  // dynamic: the frame its contents belong to
};

struct TextureRec {
  bool used = false;
  Com<ID3D12Resource> resource;
  // targets only
  bool target = false, depth = false;
  bool writable = false;  // currently in its render-target / depth-write state
  int width = 0, height = 0, layers = 1, samples = 1;
  uint32_t view = 0;      // its first render-target or depth view (index into that heap)
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct PipelineRec {
  bool used = false;
  Com<ID3D12PipelineState> state;
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
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
  Texture CreateTexture(const TextureDesc& desc) override;
  Texture CreateTarget(const TargetDesc& desc) override;
  Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) override;
  void Destroy(Buffer buffer) override;
  void Destroy(Texture texture) override;
  void Destroy(Pipeline pipeline) override;

  void BeginFrame(const float clearColor[4]) override;
  void BeginPass(const PassDesc& pass) override;
  void Resolve(Texture from, Texture to) override;
  void SetPipeline(Pipeline pipeline) override;
  void SetConstants(int slot, const void* data, uint32_t size) override;
  void SetTexture(int slot, Texture texture) override;
  void SetVertexBuffer(Buffer buffer) override;
  void SetIndexBuffer(Buffer buffer) override;
  void Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) override;
  void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override;
  void EndFrame(std::vector<uint8_t>* capture) override;

 private:
  bool CreateTargets();
  void WaitFence(uint64_t value);
  void WaitIdle();
  bool BeginUpload();
  void EndUpload();
  Span Allocate(size_t size, size_t align);
  void BindTextures();
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
  std::string adapterName_;

  Com<IDXGIFactory4> factory_;
  Com<ID3D12Device> device_;
  Com<ID3D12InfoQueue> infoQueue_;
  Com<ID3D12CommandQueue> queue_;
  Com<IDXGISwapChain3> swapChain_;
  Com<ID3D12GraphicsCommandList> list_, uploadList_;
  Com<ID3D12CommandAllocator> uploadAllocator_;
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
};

D3D12Device::~D3D12Device() {
  if (queue_ && fence_) WaitIdle();
  if (fenceEvent_) CloseHandle(fenceEvent_);
}

bool D3D12Device::Init(const DeviceDesc& desc, std::string* error) {
  auto fail = [&](const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "Direct3D 12: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return false;
  };
  hwnd_ = static_cast<HWND>(desc.window);
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
  if (factoryFlags & DXGI_CREATE_FACTORY_DEBUG) device_->QueryInterface(IID_PPV_ARGS(infoQueue_.Put()));
  if (adapter) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    char name[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    adapterName_ = n > 0 ? name : "";
  }

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.Put()));
  if (FAILED(hr)) return fail("CreateCommandQueue", hr);

  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = static_cast<UINT>(width_);
  sd.Height = static_cast<UINT>(height_);
  sd.Format = kBackFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kFrames;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  Com<IDXGISwapChain1> sc1;
  hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &sd, nullptr, nullptr, sc1.Put());
  if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);
  hr = sc1->QueryInterface(IID_PPV_ARGS(swapChain_.Put()));
  if (FAILED(hr)) return fail("IDXGISwapChain3", hr);
  factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

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
      FAILED(hr = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRingDescriptors, true, ringHeap_)))
    return fail("CreateDescriptorHeap", hr);
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

  for (FrameSlot& f : frames_) {
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(f.allocator.Put()));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
  }
  hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(uploadAllocator_.Put()));
  if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
  hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(list_.Put()));
  if (FAILED(hr)) return fail("CreateCommandList", hr);
  list_->Close();
  hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator_.Get(), nullptr, IID_PPV_ARGS(uploadList_.Put()));
  if (FAILED(hr)) return fail("CreateCommandList", hr);
  uploadList_->Close();
  hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.Put()));
  if (FAILED(hr)) return fail("CreateFence", hr);
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // the one root signature
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = kTexSlots;
  D3D12_ROOT_PARAMETER params[kCbSlots + 1] = {};
  for (int i = 0; i < kCbSlots; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[kCbSlots].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[kCbSlots].DescriptorTable.NumDescriptorRanges = 1;
  params[kCbSlots].DescriptorTable.pDescriptorRanges = &range;
  params[kCbSlots].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
  rd.NumParameters = kCbSlots + 1;
  rd.pParameters = params;
  rd.NumStaticSamplers = 6;
  rd.pStaticSamplers = samplers;
  rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Com<ID3DBlob> blob, errors;
  hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, blob.Put(), errors.Put());
  if (FAILED(hr)) return fail("D3D12SerializeRootSignature", hr);
  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature_.Put()));
  if (FAILED(hr)) return fail("CreateRootSignature", hr);

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

void D3D12Device::WaitFence(uint64_t value) {
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fenceEvent_);
  WaitForSingleObject(fenceEvent_, INFINITE);
}

void D3D12Device::WaitIdle() {
  uint64_t v = fenceNext_++;
  queue_->Signal(fence_.Get(), v);
  WaitFence(v);
}

void D3D12Device::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  WaitIdle();
  for (FrameSlot& f : frames_) {
    f.backBuffer.Reset();
    f.garbage.clear();
  }
  depth_.Reset();
  width_ = width;
  height_ = height;
  HRESULT hr = swapChain_->ResizeBuffers(kFrames, static_cast<UINT>(width), static_cast<UINT>(height), kBackFormat,
                                         tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr)) Complain("ResizeBuffers", hr);
  CreateTargets();
}

bool D3D12Device::BeginUpload() {
  if (FAILED(uploadAllocator_->Reset())) return false;
  return SUCCEEDED(uploadList_->Reset(uploadAllocator_.Get(), nullptr));
}

void D3D12Device::EndUpload() {
  uploadList_->Close();
  ID3D12CommandList* lists[] = {uploadList_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  WaitIdle();
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
  for (;;) {
    if (f.page < f.pages.size()) {
      Page& p = f.pages[f.page];
      size_t at = (f.offset + align - 1) / align * align;
      if (at + size <= p.size) {
        f.offset = at + size;
        return {p.cpu + at, p.gpu + at};
      }
      ++f.page;
      f.offset = 0;
      continue;
    }
    Page p;
    p.size = std::max(kPageSize, (size + 65535) / 65536 * 65536);
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
    f.pages.push_back(std::move(p));
  }
}

Buffer D3D12Device::CreateBuffer(const BufferDesc& desc) {
  BufferRec rec;
  rec.used = true;
  rec.kind = desc.kind;
  rec.dynamic = desc.dynamic;
  rec.stride = desc.stride;
  if (!desc.dynamic) {
    if (!desc.data || !desc.size) return {};
    rec.size = desc.size;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC rd = BufferResourceDesc(desc.size);
    // buffers start in the common state and are promoted by their first use (the copy, later the draw)
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(rec.resource.Put()));
    if (FAILED(hr)) return Complain("creating a buffer", hr), Buffer{};
    Com<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(staging.Put()));
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) hr = staging->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !BeginUpload()) return Complain("uploading a buffer", hr), Buffer{};
    std::memcpy(mapped, desc.data, desc.size);
    staging->Unmap(0, nullptr);
    uploadList_->CopyBufferRegion(rec.resource.Get(), 0, staging.Get(), 0, desc.size);
    EndUpload();
    rec.gpu = rec.resource->GetGPUVirtualAddress();
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
  return {index + 1};
}

void D3D12Device::UpdateBuffer(Buffer buffer, const void* data, uint32_t size) {
  BufferRec* rec = Find(buffer);
  if (!rec || !rec->dynamic || !inFrame_ || !size) return;
  Span s = Allocate(size, 16);
  if (!s.cpu) return;
  std::memcpy(s.cpu, data, size);
  rec->gpu = s.gpu;
  rec->size = size;
  rec->frame = frameNumber_;
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
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = static_cast<UINT16>(desc.layers);
  rd.MipLevels = static_cast<UINT16>(desc.mips);
  rd.Format = format;
  rd.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(rec.resource.Put()));
  if (FAILED(hr)) return Complain("creating a texture", hr), Texture{};

  const UINT subresources = static_cast<UINT>(desc.mips * desc.layers);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> rowBytes(subresources);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&rd, 0, subresources, 0, layouts.data(), rows.data(), rowBytes.data(), &total);
  Com<ID3D12Resource> staging;
  D3D12_HEAP_PROPERTIES up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC sd = BufferResourceDesc(total);
  hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(staging.Put()));
  void* mapped = nullptr;
  if (SUCCEEDED(hr)) hr = staging->Map(0, nullptr, &mapped);
  if (FAILED(hr)) return Complain("uploading a texture", hr), Texture{};

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
  staging->Unmap(0, nullptr);
  if (truncated) {
    fprintf(stderr, "render: a %dx%d texture with %d mips has too little data (%zu bytes)\n", desc.width, desc.height, desc.mips,
            desc.dataSize);
    return {};
  }
  if (!BeginUpload()) return {};
  for (UINT sub = 0; sub < subresources; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = rec.resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = sub;
    from.pResource = staging.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = layouts[sub];
    uploadList_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  D3D12_RESOURCE_BARRIER b = Transition(rec.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  uploadList_->ResourceBarrier(1, &b);
  EndUpload();

  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
  view.Format = format;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
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

Texture D3D12Device::CreateTarget(const TargetDesc& desc) {
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

  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = static_cast<UINT16>(desc.layers);
  rd.MipLevels = 1;
  rd.Format = depth ? DXGI_FORMAT_R32_TYPELESS : format;
  rd.SampleDesc.Count = static_cast<UINT>(samples);
  rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  rec.samples = samples;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = format;
  if (!depth) std::memcpy(clear.Color, desc.clearValue, sizeof clear.Color);
  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                &clear, IID_PPV_ARGS(rec.resource.Put()));
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

Pipeline D3D12Device::CreatePipeline(const PipelineDesc& desc, std::string* error) {
  auto compile = [&](std::string_view source, const char* entry, const char* target, Com<ID3DBlob>& out) {
    Com<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source.data(), source.size(), desc.name, nullptr, nullptr, entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.Put(), errors.Put());
    if (FAILED(hr)) {
      std::string text = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
      if (error) *error = std::string(desc.name) + " (" + entry + "): " + text;
      return false;
    }
    return true;
  };
  Com<ID3DBlob> vs, ps;
  if (!compile(desc.vertexSource, desc.vertexEntry, "vs_5_1", vs)) return {};
  const bool hasPixelShader = !desc.pixelSource.empty();
  if (hasPixelShader && !compile(desc.pixelSource, desc.pixelEntry, "ps_5_1", ps)) return {};

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
  D3D12_RENDER_TARGET_BLEND_DESC& bt = pd.BlendState.RenderTarget[0];
  bt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
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
  pd.DSVFormat = kDepthFormat;
  pd.SampleDesc.Count = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? static_cast<UINT>(desc.samples) : 1;
  pd.RasterizerState.MultisampleEnable = pd.SampleDesc.Count > 1;

  PipelineRec rec;
  rec.used = true;
  rec.topology = desc.topology == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(rec.state.Put()));
  if (FAILED(hr)) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s: CreateGraphicsPipelineState failed (0x%08lx)", desc.name, static_cast<unsigned long>(hr));
    if (error) *error = buf;
    return {};
  }
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

void D3D12Device::Destroy(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec) return;
  Retire(rec->resource.Get());
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
  Retire(rec->resource.Get());
  *rec = TextureRec{};
  device_->CopyDescriptorsSimple(1, StagingHandle(texture.id), StagingHandle(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  freeTextures_.push_back(texture.id);
}

void D3D12Device::Destroy(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (!rec) return;
  Retire(rec->state.Get());
  *rec = PipelineRec{};
  freePipelines_.push_back(pipeline.id - 1);
}

void D3D12Device::BeginFrame(const float clearColor[4]) {
  cur_ = static_cast<int>(swapChain_->GetCurrentBackBufferIndex());
  FrameSlot& f = frames_[cur_];
  WaitFence(f.fence);
  f.garbage.clear();
  f.page = f.offset = 0;
  f.descriptorsUsed = 0;
  f.tables.clear();
  ++frameNumber_;
  inFrame_ = true;
  f.allocator->Reset();
  list_->Reset(f.allocator.Get(), nullptr);

  D3D12_RESOURCE_BARRIER b = Transition(f.backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list_->ResourceBarrier(1, &b);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvStart_;
  rtv.ptr += static_cast<SIZE_T>(cur_) * rtvStep_;
  list_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
  list_->ClearDepthStencilView(dsvStart_, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
  list_->OMSetRenderTargets(1, &rtv, FALSE, &dsvStart_);
  SetViewport(width_, height_);
  list_->SetGraphicsRootSignature(rootSignature_.Get());
  ID3D12DescriptorHeap* heaps[] = {ringHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  boundTextures_ = TableKey{};
  texturesDirty_ = true;
  passColor_ = passDepth_ = 0;
}

void D3D12Device::SetViewport(int width, int height) {
  D3D12_VIEWPORT vp = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  D3D12_RECT scissor = {0, 0, width, height};
  list_->RSSetViewports(1, &vp);
  list_->RSSetScissorRects(1, &scissor);
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
  if (pass.window) {
    ReleasePassTargets();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(static_cast<uint32_t>(cur_));
    list_->OMSetRenderTargets(1, &rtv, FALSE, &dsvStart_);
    if (pass.clearColor) list_->ClearRenderTargetView(rtv, pass.clearValue, 0, nullptr);
    if (pass.clearDepth) list_->ClearDepthStencilView(dsvStart_, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    SetViewport(width_, height_);
    return;
  }
  TextureRec* color = Find(pass.color);
  TextureRec* depth = Find(pass.depth);
  if (!depth || !depth->target || !depth->depth || pass.depthLayer < 0 || pass.depthLayer >= depth->layers ||
      (pass.color.id && (!color || !color->target || color->depth))) {
    fprintf(stderr, "render: Direct3D 12: BeginPass needs a depth target, and a colour target or none\n");
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
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = DsvHandle(depth->view + static_cast<uint32_t>(pass.depthLayer));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = color ? RtvHandle(color->view) : D3D12_CPU_DESCRIPTOR_HANDLE{};
  list_->OMSetRenderTargets(color ? 1 : 0, color ? &rtv : nullptr, FALSE, &dsv);
  if (color && pass.clearColor) list_->ClearRenderTargetView(rtv, pass.clearValue, 0, nullptr);
  if (pass.clearDepth) list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
  SetViewport(depth->width, depth->height);
}

void D3D12Device::SetPipeline(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (!rec) return;
  list_->SetPipelineState(rec->state.Get());
  list_->IASetPrimitiveTopology(rec->topology);
}

void D3D12Device::SetConstants(int slot, const void* data, uint32_t size) {
  if (slot < 0 || slot >= kCbSlots || !size) return;
  uint32_t padded = (size + 255u) & ~255u;
  Span s = Allocate(padded, 256);
  if (!s.cpu) return;
  std::memcpy(s.cpu, data, size);
  list_->SetGraphicsRootConstantBufferView(static_cast<UINT>(slot), s.gpu);
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
  list_->SetGraphicsRootDescriptorTable(kCbSlots, table);
}

void D3D12Device::SetVertexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Vertex || (rec->dynamic && rec->frame != frameNumber_)) return;
  D3D12_VERTEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride};
  list_->IASetVertexBuffers(0, 1, &view);
}

void D3D12Device::SetIndexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != frameNumber_)) return;
  D3D12_INDEX_BUFFER_VIEW view = {rec->gpu, rec->size, rec->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
  list_->IASetIndexBuffer(&view);
}

void D3D12Device::Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) {
  BindTextures();
  list_->DrawInstanced(vertexCount, instances, firstVertex, 0);
}

void D3D12Device::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) {
  BindTextures();
  list_->DrawIndexedInstanced(indexCount, instances, firstIndex, baseVertex, 0);
}

void D3D12Device::EndFrame(std::vector<uint8_t>* capture) {
  FrameSlot& f = frames_[cur_];
  ReleasePassTargets();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  if (capture) {
    D3D12_RESOURCE_DESC bd = f.backBuffer->GetDesc();
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
  if (capture && readback_) {
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
  list_->Close();
  ID3D12CommandList* lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  HRESULT hr = swapChain_->Present(vsync_ ? 1 : 0, !vsync_ && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (FAILED(hr)) Complain("Present", hr);
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

  if (capture) {
    capture->clear();
    if (!readback_) return;
    WaitFence(f.fence);
    void* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readbackSize_)};
    if (FAILED(readback_->Map(0, &range, &mapped))) return;
    capture->resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
    for (int y = 0; y < height_; ++y)
      std::memcpy(capture->data() + static_cast<size_t>(y) * static_cast<size_t>(width_) * 4,
                  static_cast<const uint8_t*>(mapped) + layout.Offset + static_cast<size_t>(y) * layout.Footprint.RowPitch,
                  static_cast<size_t>(width_) * 4);
    D3D12_RANGE none = {0, 0};
    readback_->Unmap(0, &none);
  }
}

}  // namespace

std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, std::string* error) {
  auto d = std::make_unique<D3D12Device>();
  if (!d->Init(desc, error)) return nullptr;
  return d;
}

}  // namespace render::gfx
