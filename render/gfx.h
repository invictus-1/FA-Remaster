// The thin graphics device interface. Everything the renderer draws goes through it, so a second
// backend (Vulkan) only has to implement this file. The first backend is Direct3D 12 (gfx_d3d12.cpp).
//
// Fixed conventions, the same for every backend:
//  - One window, one swap chain; BeginFrame binds the back buffer and the depth buffer.
//  - Depth is reversed (cleared to 0, a fragment passes when its depth is greater or equal), stored as
//    32-bit float: together with linalg.h's Perspective this keeps precision at strategic-zoom distances.
//  - Shaders are HLSL source (model 5.1), compiled when a pipeline is created.
//    Constant buffers: registers b0..b3, set per draw with SetConstants (the data is copied).
//    Textures: registers t0..t43. Two buffers of plain data (SetShaderBuffer, slots 0 and 1), read
//    in a shader as `StructuredBuffer<T> name : register(t0, space1)` and `register(t1, space1)`; a
//    static buffer bound there (a model's vertices or indices) is read as `ByteAddressBuffer`.
//    Samplers are fixed:
//      s0 anisotropic wrap, s1 trilinear clamp, s2 point clamp, s3 trilinear wrap, s4 point wrap,
//      s5 shadow comparison (1 where the given depth is nearer to the light than the stored one or
//      equal, the four nearest results blended; outside the map: 1).
//  - Passes: BeginFrame starts one into the window; BeginPass starts one into targets (textures made
//    with CreateTarget) or into the window again. A pipeline says which colour format it draws into.
//  - Handles are plain numbers; 0 means "none". Destroying is deferred until the GPU is done with them.
//  - Buffers: a static buffer gets its data at creation. A dynamic buffer holds no data of its own:
//    UpdateBuffer gives it this frame's contents, which last until EndFrame.
//  - Compute shaders (CreateCompute, Dispatch) do work on the card that draws nothing (the modern look's
//    lighting: volumes of light in the air, reflections, light thrown back). They read what draws read and
//    write storage targets (TargetDesc::storage), registers u0..u15. A dispatch lies between passes.
//  - Ray tracing, where the card can (Device::RayTracing; Direct3D 12's DXR 1.1, Vulkan's ray queries; never
//    on macOS): structures over triangles that any shader traces rays through, inline, with RayQuery. A
//    shader that names RayQuery is compiled for shader model 6.5.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace render::gfx {

struct Buffer {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
struct Texture {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
struct Pipeline {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
struct Structure {  // a ray tracing structure: a geometry's (bottom level) or a scene's (top level)
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};

enum class Format : uint8_t {
  Unknown,
  R8,
  A8,  // reads as (0, 0, 0, a)
  RG8,
  RGBA8,
  RGBA8_sRGB,
  BGRA8,
  BGRA8_sRGB,
  R16,
  R16F,
  RG16F,
  RGBA16F,
  R32F,
  R32UI,    // a whole number of 32 bits a pixel (read as uint; compute shaders may add to it atomically)
  RGBA32F,
  D32,  // depth; only for targets
  BC1,  // DXT1
  BC1_sRGB,
  BC2,  // DXT3
  BC2_sRGB,
  BC3,  // DXT5
  BC3_sRGB,
  BC4,
  BC5,  // read as (red, green, 0, red): two-channel normal maps read as the game's DXT5 ones do (their .ga)
  BC7,  // (only in files with the newer header; the game's own files have none)
};
bool IsBlockCompressed(Format f);
int BytesPerPixelOrBlock(Format f);  // per pixel, or per 4x4 block for the BC formats

// Shader: data a shader reads (SetShaderBuffer); dynamic only. (A static Vertex or Index buffer can be read by
// shaders too: SetShaderBuffer.)
// [gpuscenery] Card: a static buffer the card itself writes (Device::IndirectDraws; card_scenery.h): its `size` bytes (a
// multiple of 16) given at creation (`data`, or zeros where it is null), then changed only by compute shaders and by
// UpdateBufferRange. SetShaderBuffer binds it as any static buffer: draws and dispatches read it at t0 / t1 of space1
// (StructuredBuffer, ByteAddressBuffer), and a compute shader WRITES it at the same slot's u register of space1,
// `RWByteAddressBuffer name : register(u0, space1)` (or u1; a dispatch reads such a buffer through that alone, not through
// its t register too). Indirect draws take their arguments from it (DrawIndexedIndirect). What a dispatch writes, the
// dispatches and passes after it read, as with storage targets.
enum class BufferKind : uint8_t { Vertex, Index, Shader, Card };
struct BufferDesc {
  BufferKind kind = BufferKind::Vertex;
  uint32_t size = 0;    // bytes (dynamic: unused)
  uint32_t stride = 0;  // vertex size, or 2 / 4 for an index buffer
  bool dynamic = false;
  const void* data = nullptr;  // static buffers: `size` bytes
};

struct TextureDesc {
  int width = 0, height = 0;
  int mips = 1;
  int layers = 1;     // 6 for a cube map
  bool cube = false;
  Format format = Format::RGBA8;
  // All images tightly packed: layer 0 mip 0, layer 0 mip 1, ... layer 1 mip 0, ...
  // (rows of width * bytes per pixel; BC formats: rows of blocks).
  const void* data = nullptr;
  size_t dataSize = 0;
};

// A texture that can be drawn into (BeginPass) and then read like any other (SetTexture).
// Colour targets take a colour format; a depth target is Format::D32 and reads as its depth value in
// the red channel. With several layers it is an array (a shadow map's cascades), drawn one layer at a time.
struct TargetDesc {
  int width = 0, height = 0;
  int layers = 1;
  Format format = Format::RGBA8;
  float clearValue[4] = {0, 0, 0, 0};  // the colour it will usually be cleared to (clearing to it is fastest)
  // Samples per pixel (1, 2, 4 or 8) for anti-aliasing. A target with more than one cannot be read
  // as a texture: draw into it, then Resolve it into an ordinary target of the same size and format.
  int samples = 1;
  // A storage target: written by compute shaders (SetStorage, Dispatch), never by a pass; read as any
  // texture. One sample a pixel, one layer, in a format a compute shader can write: RGBA8, R8, R16F, RG16F,
  // RGBA16F, R32F, R32UI or RGBA32F.
  bool storage = false;
  int depth = 1;  // storage: more than 1 makes it a volume of that many slices (Texture3D, RWTexture3D)
  int mips = 1;   // storage: its levels, each half the one before (SetStorage writes one; a read sees all)
};

// A compute shader: HLSL, `[numthreads(x, y, z)] void CS(uint3 id : SV_DispatchThreadID)`. It reads
// constants (b0..b3), textures (t0..t43; a volume as Texture3D), the shader buffers and the fixed
// samplers as draws do, and writes the storage targets SetStorage binds: `RWTexture2D<float4> out :
// register(u0)` (RWTexture3D for a volume), u0..u15.
struct ComputeDesc {
  const char* name = "";  // for error messages
  std::string_view source;
  const char* entry = "CS";
};

// Ray tracing (Device::RayTracing). A geometry structure is made once over a model's triangles, from parts of
// its static buffers; the scene's structure is built again every frame over instances of those, each placed by
// its own matrix. A shader reads the scene as `RaytracingAccelerationStructure scene : register(tN)` (SetScene)
// and traces with RayQuery.
struct GeometryPart {
  Buffer vertices;  // a static vertex buffer: each vertex's position three floats at `positionOffset`
  uint32_t positionOffset = 0, firstVertex = 0, vertexCount = 0;
  Buffer indices;   // a static index buffer (its stride says 16 or 32 bits), counted from firstVertex; none: the vertices in threes
  uint32_t firstIndex = 0, indexCount = 0;
  bool opaque = true;  // false: a ray meeting one of its triangles leaves it to the shader to take or not (holes, leaves)
};
struct SceneInstance {
  float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // a 3x4 matrix's rows: world = transform * (x, y, z, 1)
  Structure geometry;
  uint32_t id = 0;      // 24 bits, what a shader reads back of what a ray met (CommittedInstanceID)
  uint8_t mask = 0xff;  // a ray query sees it when this and its own mask have a bit in common
};

// What a pass draws into: a colour target, a depth target, or both.
struct PassDesc {
  bool window = false;    // draw to the window (with the window's own depth buffer); the two below are unused
  Texture color;          // a colour target, or none for a depth-only pass
  Texture depth;          // a depth target, or none (pipelines drawn then say depthTarget = false)
  int depthLayer = 0;
  bool clearColor = true;
  float clearValue[4] = {0, 0, 0, 0};
  bool clearDepth = true;
};

enum class AttrType : uint8_t { Float1, Float2, Float3, Float4, UByte4, UByte4N, Short2N, Short4N };
struct VertexAttr {
  const char* semantic = nullptr;  // "POSITION", "NORMAL", "TEXCOORD", "COLOR", ...
  int index = 0;
  AttrType type = AttrType::Float3;
  uint32_t offset = 0;
};

enum class Topology : uint8_t { Triangles, Lines };
enum class Cull : uint8_t { None, Back, Front };
// Additive: what is there + what is drawn x its alpha. Multiply: what is there x what is drawn.
// InverseModulate: what is there x (1 - what is drawn), a darkening (smoke that dims what is behind it).
enum class Blend : uint8_t { Opaque, Alpha, Premultiplied, Additive, Multiply, InverseModulate };

struct PipelineDesc {
  const char* name = "";         // for error messages
  std::string_view vertexSource; // HLSL; may be the same text as pixelSource
  std::string_view pixelSource;
  const char* vertexEntry = "VS";
  const char* pixelEntry = "PS";
  std::vector<VertexAttr> attrs;
  uint32_t stride = 0;
  Topology topology = Topology::Triangles;
  Cull cull = Cull::Back;
  bool frontCounterClockwise = true;
  bool depthTest = true;
  bool depthWrite = true;
  float depthBias = 0;  // in depth-buffer units toward the camera (decals, lines on terrain)
  Blend blend = Blend::Opaque;
  // What it draws into: the colour format of the pass (the window's is RGBA8), or no colour at all
  // (shadow maps; the pixel shader may then be left out, or return nothing and only discard).
  Format colorFormat = Format::RGBA8;
  bool depthOnly = false;
  int samples = 1;  // the samples per pixel of the targets it draws into
  // With several samples per pixel: the alpha the pixel shader returns says how many of them the
  // pixel covers (smooth edges on surfaces with holes, without blending or sorting).
  bool alphaToCoverage = false;
  bool depthTarget = true;  // false for a pass that has a colour target only (full-screen work)
  bool colorWrite = true;   // false: the colour target is left as it is (a pass that only lays depth)
  // false: the colour's alpha is left as it is (the scene's alpha holds the original look's glow, which
  // only some of what is drawn writes, as the original's)
  bool alphaWrite = true;
};

struct DeviceDesc {
  // Direct3D 12 (Windows), or Vulkan (gfx_vulkan.cpp: other systems, and tried on Windows)
  enum class Backend : uint8_t { Direct3D12, Vulkan };
  Backend backend = Backend::Direct3D12;
  // The window, as its system has it. Windows: its HWND in `window`. Elsewhere (Vulkan): Xlib, the
  // X11 Display* in `display` and the Window id in `window`; Wayland, the wl_display* in `display` and
  // the wl_surface* in `window`; Metal (macOS, through MoltenVK), the view's CAMetalLayer* in `window`.
  // Host (Vulkan, any system): the host makes the window's surface itself (SDL3's SDL_Vulkan_GetInstanceExtensions
  // and SDL_Vulkan_CreateSurface): `hostExtensions` the instance extensions its window needs, `hostSurface` makes
  // the surface for the VkInstance it is given and returns the VkSurfaceKHR (0: it could not); `window` unused.
  enum class WindowSystem : uint8_t { Win32, Xlib, Wayland, Metal, Host };
#if defined(_WIN32)
  WindowSystem windowSystem = WindowSystem::Win32;
#elif defined(__APPLE__)
  WindowSystem windowSystem = WindowSystem::Metal;
#else
  WindowSystem windowSystem = WindowSystem::Xlib;
#endif
  void* window = nullptr;
  void* display = nullptr;
  std::vector<std::string> hostExtensions;
  std::function<uint64_t(void* instance)> hostSurface;
  int width = 0, height = 0;
  bool vsync = true;
  bool debug = false;      // the API's validation layer, when installed
  // Where compiled shaders are kept from one run to the next (compiling them all takes seconds; a
  // shader whose source has not changed is read back instead). Empty: the user's local application
  // data, "FA Remastered\shaders"; "-": none kept.
  std::string shaderCache;
  // Direct3D 12: the shaders compiled by Direct3D's own compiler (FXC, shader model 5.1) even where DXC
  // (dxcompiler.dll) is beside the program
  bool oldShaderCompiler = false;
};

// [mcrec] Draws recorded on another thread (Device::BeginParallel): a recorder takes the draw calls alone, with the same
// meanings as the device's own below. Everything else (passes, dispatches, marks, buffers' and textures' updates, what
// is made or let go of) stays with the device, on the frames' thread.
class Recorder {
 public:
  virtual ~Recorder() = default;
  virtual void SetPipeline(Pipeline pipeline) = 0;  // (a drawing pipeline; a compute one is passed over)
  virtual void SetConstants(int slot, const void* data, uint32_t size) = 0;
  virtual void SetTexture(int slot, Texture texture) = 0;
  virtual void SetVertexBuffer(Buffer buffer) = 0;
  virtual void SetIndexBuffer(Buffer buffer) = 0;
  virtual void SetShaderBuffer(Buffer buffer, int slot = 0) = 0;
  virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instances = 1) = 0;
  virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t instances = 1) = 0;
  virtual void DrawIndexedIndirect(Buffer arguments, uint32_t offset) = 0;  // [gpuscenery] (Device's)
};

// [vkpresent] (checks: the viewer's --staging-reserve) Vulkan's staging reserve in MB in place of its own reckoning (0: its own)
inline int gStagingReserveMB = 0;

class Device {
 public:
  static std::unique_ptr<Device> Create(const DeviceDesc& desc, std::string* error);
  virtual ~Device() = default;

  virtual const char* BackendName() const = 0;
  virtual std::string AdapterName() const = 0;
  virtual int Width() const = 0;
  virtual int Height() const = 0;
  virtual void Resize(int width, int height) = 0;
  virtual void SetVsync(bool on) = 0;

  virtual Buffer CreateBuffer(const BufferDesc& desc) = 0;
  virtual void UpdateBuffer(Buffer buffer, const void* data, uint32_t size) = 0;  // dynamic buffers
  // The same without a copy: room for `size` bytes of a dynamic buffer's contents for this frame, to be
  // written (by any thread) before the draws that read it are handed to the card; null when there is no
  // room. Within a frame only, as UpdateBuffer.
  virtual void* WriteBuffer(Buffer buffer, uint32_t size) = 0;
  virtual Texture CreateTexture(const TextureDesc& desc) = 0;
  // Replaces a rectangle of a texture's first level with `data` (rows `pitch` bytes apart), within a
  // frame (between BeginFrame and EndFrame), before a draw reads it. An uncompressed texture of one
  // level and one layer (RGBA8, BGRA8, R16, ...). For what changes every frame (an interface drawn on
  // the processor), and the ground's heights where the simulation changes them.
  virtual void UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) = 0;
  virtual Texture CreateTarget(const TargetDesc& desc) = 0;
  virtual Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) = 0;
  virtual Pipeline CreateCompute(const ComputeDesc& desc, std::string* error) = 0;
  // Pipelines asked for between these two (a world's opening) are made together at the end: first their shaders
  // compiled on several cores at once into the shader cache (where it lacks them: the first start, or after the
  // shaders changed), then the pipelines from it. Their handles are given at once; one used before the end is made
  // then. EndPipelineBatch: false with the first one's error that could not be made. (Where there is no shader
  // cache, or no DXC, each is made at once, as outside a batch.)
  virtual void BeginPipelineBatch() {}
  virtual bool EndPipelineBatch(std::string* error) { (void)error; return true; }
  // [snowhitch] Pipelines asked for between these two (what a game will want later: World::PrepareAhead's, the snow's before its
  // first flake) are made on a thread of the device's own while the frames go on: their shaders compiled there into the shader
  // cache where it lacks them, then the pipeline by the driver (on a new build up to two seconds each, which a frame would
  // wait for). Their handles are given at once; the first frame that sets one takes it as made, or waits for it there (made
  // then if its turn has not come): the picture is the same, only the waiting moves. One that cannot be made draws nothing
  // (as in a batch; its messages said). Where a backend cannot (no shader cache, no DXC, or not written), each is made at
  // once, as outside these.
  virtual void BeginBackgroundPipelines() {}
  virtual void EndBackgroundPipelines() {}
  // [bgload] The pipelines asked for in the background that the device's own thread has not made yet (queued, or being
  // made): a world opened behind the one drawn is ready when none of its are left (WorldPicture::MapReady). Any thread.
  virtual int BackgroundPipelinesWaiting() { return 0; }
  // [bgload] Whether a pipeline is made: one asked for in the background and not made yet by the device's own thread is
  // not (setting it would wait for it, or make it then); any other is. The frames' thread.
  virtual bool PipelineReady(Pipeline pipeline) {
    (void)pipeline;
    return true;
  }
  // [spikes] A texture, or a static buffer, made in two halves, so that what takes time is off the frames' thread:
  // PrepareTexture / PrepareBuffer may be called on ANY thread (a worker's: TextureCache::MakeAhead, ModelCache's meshes
  // made ahead) and do the slow part - Direct3D 12: the texture's resource made and its images copied into an upload
  // buffer of its own; Vulkan: the image and its memory made and the images copied into a staging buffer of its own;
  // FinishTexture / FinishBuffer, on the frames' thread (where CreateTexture is called, and never inside a frame's pass
  // that reads it), give it its handle and record the copy onto the card: a small part of CreateTexture's time. What
  // they make is what CreateTexture / CreateBuffer would make of the same description (the same formats, levels, views
  // and contents), only its slow part done elsewhere and earlier. A prepared one that is never finished is let go of when
  // its object is. Null from Prepare*: the description is not one Create* would take (then Create* is told so too).
  // Where a backend has no halves of its own (this default), Prepare keeps a copy of the description and its data, and
  // Finish calls Create* with it.
  struct Prepared {
    virtual ~Prepared() = default;
  };
  virtual std::unique_ptr<Prepared> PrepareTexture(const TextureDesc& desc);
  virtual Texture FinishTexture(std::unique_ptr<Prepared> prepared);
  virtual std::unique_ptr<Prepared> PrepareBuffer(const BufferDesc& desc);  // (static buffers only)
  virtual Buffer FinishBuffer(std::unique_ptr<Prepared> prepared);
  // [warmtarget] A target made in two halves, as a texture is above: PrepareTarget on ANY thread makes its memory (Direct3D
  // 12: its resource; Vulkan: its image, memory and views), the slow part of a target at the window's size (tens of
  // milliseconds for the largest); FinishTarget on the frames' thread (where CreateTarget is called, between frames or
  // before the passes that use it) gives it its handle and what the device's own tables keep of it. What they make is
  // what CreateTarget makes of the same description. Null from PrepareTarget: CreateTarget is to be called instead (it says
  // why where it fails). The default keeps the description, and FinishTarget calls CreateTarget with it.
  virtual std::unique_ptr<Prepared> PrepareTarget(const TargetDesc& desc);
  virtual Texture FinishTarget(std::unique_ptr<Prepared> prepared);
  virtual void Destroy(Buffer buffer) = 0;
  virtual void Destroy(Texture texture) = 0;
  virtual void Destroy(Pipeline pipeline) = 0;

  // Ray tracing (above). False: the card or its driver cannot; the calls below then do nothing.
  virtual bool RayTracing() const { return false; }
  // A geometry structure over `count` parts, built on the card at the next BuildScene (its buffers must
  // stay until then). None where it cannot be made.
  virtual Structure CreateGeometry(const GeometryPart*, int) { return {}; }
  // [warmtarget] The same in three steps, so that the driver's sizing of a large one (40 ms for the ground's on Direct3D 12)
  // is off the frames' thread: DescribeGeometry on the frames' thread (it reads the device's own tables of buffers: quick;
  // the parts' buffers as CreateGeometry wants them), SizeGeometry on ANY thread (the driver's sizes and the structure's
  // memory: the slow part), FinishGeometry on the frames' thread (its handle; built at the next BuildScene, as
  // CreateGeometry's). What they make is what CreateGeometry makes of the same parts. The default keeps the parts,
  // sizes nothing, and FinishGeometry calls CreateGeometry with them.
  virtual std::unique_ptr<Prepared> DescribeGeometry(const GeometryPart* parts, int count);
  virtual void SizeGeometry(Prepared* prepared) { (void)prepared; }
  virtual Structure FinishGeometry(std::unique_ptr<Prepared> prepared);
  // The scene's structure over these instances, built again now, after the geometry made since the last
  // time. Between passes (it ends the pass under way, as Dispatch does); what comes after reads it. `scene`
  // is made the first time and grows as needed.
  virtual void BuildScene(Structure*, const SceneInstance*, uint32_t) {}
  // [cardcost] BuildScene's, built so that later frames can refit it: with `rebuild` false, a scene last made here and
  // built over as many instances as now (those whose structure is made) is refitted in place instead (the instances'
  // matrices, masks, ids and structures may all differ; the tree over them stays as it was built, so it serves the rays
  // the worse the more they have moved since: rebuild now and then). The same rays meet the same things either way.
  // False where the device cannot (nothing done; BuildScene then).
  virtual bool RefitScene(Structure*, const SceneInstance*, uint32_t, bool) { return false; }
  // A scene's structure for the shaders, in a texture slot (none: empty).
  virtual void SetScene(int, Structure) {}
  virtual void Destroy(Structure) {}

  // Starts the frame and a pass into the window, cleared to that colour.
  virtual void BeginFrame(const float clearColor[4]) = 0;
  // Starts another pass; the targets of the pass before it become readable as textures.
  // The viewport is the whole target. A target must not be bound as a texture in its own pass.
  virtual void BeginPass(const PassDesc& pass) = 0;
  // Averages a multi-sample colour target into an ordinary one (same size and format). Ends the
  // current pass; begin another before drawing again.
  virtual void Resolve(Texture from, Texture to) = 0;
  // A drawing pipeline for the draws after it, or a compute pipeline for the dispatches after it.
  // Constants, textures and shader buffers set are seen by both kinds.
  virtual void SetPipeline(Pipeline pipeline) = 0;
  virtual void SetConstants(int slot, const void* data, uint32_t size) = 0;
  // (a texture is not read by a dispatch that also writes it: SetTexture and SetStorage, not both)
  virtual void SetTexture(int slot, Texture texture) = 0;
  // A storage target for the dispatches to write, at one of its levels, in slot u0..u15 (none: empty).
  virtual void SetStorage(int slot, Texture texture, int mip = 0) = 0;
  // Runs the compute pipeline set last over x * y * z groups of its threads. It ends the pass under way,
  // as Resolve does (its targets become readable; begin another before drawing again). What a dispatch
  // writes, the dispatches and passes after it read.
  virtual void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) = 0;
  virtual void SetVertexBuffer(Buffer buffer) = 0;
  virtual void SetIndexBuffer(Buffer buffer) = 0;
  // A buffer shaders read, in slot 0 or 1 (t0, t1 of space1): a dynamic buffer of kind Shader, updated this
  // frame, read as a StructuredBuffer; or any static buffer (a model's vertex or index buffer), read as a
  // ByteAddressBuffer from its first byte (Load at multiples of 4 bytes; 16-bit indices two to a word, the
  // first in the low half; the last word of a buffer whose size is not a multiple of 4 can be read whole, its
  // bytes past the end unspecified). Direct3D 12: a root view at its place in its block (blocks are made in
  // the common state, which a read promotes); Vulkan: its own buffer (static vertex and index buffers are made
  // storage buffers too for this). What is bound stays until the next SetShaderBuffer of that slot or the
  // frame's end.
  virtual void SetShaderBuffer(Buffer buffer, int slot = 0) = 0;
  virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instances = 1) = 0;
  virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0,
                           uint32_t instances = 1) = 0;
  // [gpuscenery] Work the card gives itself (BufferKind::Card above). IndirectDraws: whether the device has card buffers and
  // the two calls below (false: CreateBuffer makes no card buffer, and the calls do nothing). UpdateBufferRange: `size`
  // bytes of a card buffer from `offset` (multiples of 4) replaced by `data`, within a frame, outside a parallel section,
  // before the dispatches and draws after it read them (a copy on the card; a pass under way goes on after it).
  // DrawIndexedIndirect: DrawIndexed with its arguments read by the card, when it draws, from a card buffer at `offset` (a
  // multiple of 4): five 32-bit numbers, as Direct3D 12's D3D12_DRAW_INDEXED_ARGUMENTS and Vulkan's
  // VkDrawIndexedIndirectCommand have them: the index count, the instances (0: nothing drawn), the first index, the base
  // vertex and the first instance, which must be 0 (SV_InstanceID counts from 0 on both backends).
  virtual bool IndirectDraws() const { return false; }
  virtual void UpdateBufferRange(Buffer buffer, uint32_t offset, const void* data, uint32_t size) {
    (void)buffer, (void)offset, (void)data, (void)size;
  }
  virtual void DrawIndexedIndirect(Buffer arguments, uint32_t offset) { (void)arguments, (void)offset; }
  // Presents the frame. With `capture`, also reads the finished image back: width * height * 4 bytes,
  // RGBA, top row first.
  // `present` false: the frame is drawn (and read back with `capture`) but not shown; the next frame
  // draws into the same picture.
  virtual void EndFrame(std::vector<uint8_t>* capture = nullptr, bool present = true) = 0;

  // Frame generation (render/frame_generation.h makes the pictures): every frame shown twice over, first a
  // picture made between the frame before and this one, then the frame itself, evenly apart in time (a thread
  // of the device's own shows them, half the time between frames apart: gfx_pacing.h). While it is on, the
  // window a frame draws into is a target of the device's own, this frame's (WindowPicture: readable as a
  // texture once a pass into the window has ended), and EndFrame, after the frame's last pass, calls `make`
  // (it may dispatch and draw): true when it wrote BetweenPicture (a storage target of the window's size,
  // RGBA8, this frame's own), false to show the frame alone (a cut, or nothing to make one from). Empty: off.
  // `make` is replaced at once (one given before is never called after); switching on or off happens at once
  // between frames, else at the next BeginFrame, and waits for the card. False where the device cannot (it
  // stays off); FrameGenerating says whether it is on for the frame under way.
  virtual bool SetFrameGeneration(std::function<bool()> make) { return !make; }
  virtual bool FrameGenerating() const { return false; }
  virtual Texture WindowPicture() { return {}; }
  virtual Texture BetweenPicture() { return {}; }
  // (checks) EndFrame's capture reads the picture made between, when there is one, instead of the frame
  bool captureBetween = false;
  // (photomode) A photo's frames (WorldPicture::StartPhoto): while on, the window a frame draws into is a picture of the
  // device's own, `width` x `height` pixels (RGBA8, as the window's; at most 16384 a side) with a depth of its own, in
  // place of the window and its depth: passes into the window draw there, at that size, EndFrame reads it back with
  // `capture` and shows nothing, and no picture is made between frames. The window, its size (Width, Height) and what it
  // last showed stay as they are. (0, 0): off, the next frame draws into the window again, the picture kept for another
  // photo of its size; (-1, -1): off and the picture let go of. Between frames. False where the picture cannot be made
  // (then it is off).
  virtual bool SetPhotoWindow(int width, int height) { return width <= 0 || height <= 0; }
  // [weather] A read-back that does not wait for the card (the minimap's ground drawn anew as the hour and the weather move:
  // WorldPicture::DrawMinimap). After CaptureLater(true), the next EndFrame with a capture only has the picture copied out on
  // the card and leaves `capture` empty; TakeCapture then hands it over once the card has finished that frame: 1 taken, 0
  // not yet, -1 none waiting (none asked for, or another capture has used the read-back since). Where a device cannot
  // (CapturesLater false), EndFrame reads back at once and waits, as before.
  virtual bool CapturesLater() const { return false; }
  virtual void CaptureLater(bool later) { (void)later; }
  virtual int TakeCapture(std::vector<uint8_t>* capture) { (void)capture; return -1; }
  // [views] Within a frame: the passes into the window from here on draw into these targets instead (an RGBA8 colour
  // target and a D32 depth target of one size; the viewport theirs), until set to none (both empty) again - the window's
  // passes then go to the window. For a picture with several views (WorldPicture::SetViews): each view's frame drawn
  // into a picture of its own, as if that were the window, and laid into the window after. Each backend's BeginPass
  // turns such a pass into one into these targets (ViewWindowPass).
  void SetViewWindow(Texture color, Texture depth) {
    viewColor_ = color;
    viewDepth_ = depth;
  }
  bool ViewWindowOn() const { return static_cast<bool>(viewColor_); }
  // The latency limiter ("Low Latency", a player's option; off as at first): the camera, the cursor's feedback and
  // the interface answer the mouse and the keys sooner. With it on, the frames waiting to be shown are kept to one
  // (Direct3D 12: the swap chain made with its frame latency waitable object, at a frame latency of 1; Vulkan: the
  // driver's present wait where it has one, else the frames on the card kept to two), and a frame then begins as
  // late as the frames before it allow (gfx_latency.h): never so early that it would only wait for the card to
  // finish the frames before, and on Direct3D 12 with vsync, still in time for the refresh it is for, less a margin
  // that grows at once when a frame was too late and shrinks slowly while none is (Vulkan has no such wait: the
  // present wait's times are too uneven to pace by). Frames that cannot be made one after the other within a
  // refresh are let two ahead, so it shows as many frames as without. Without vsync only the wait for the card is
  // kept (the swap chain and the frames waiting to be shown as without the limiter). Where the frames do not keep up
  // (without vsync, or slower than the refresh), it tries the way without its waits now and then and keeps to that
  // where frames come faster so (other programs sharing the card: gfx_latency.h, Keep).
  // With frame generation, only the real frames waiting for the presenter are kept to one (or none, where they are
  // made well within a refresh); the presenter and its swap chain are as without. A host calls WaitForNextFrame once
  // a frame, just before it reads its input (the window's events, the mouse's place): the waits are there, so that
  // what is read is as fresh as it can be; with the limiter off it returns at once (a host that does not call it
  // still gets the waits, in BeginFrame, only after its input read). Switching happens at once between frames, else
  // at the next WaitForNextFrame or BeginFrame (Direct3D 12 makes its swap chain anew, as frame generation does, and
  // waits for the card). With it off, the swap chain and the waits are as they always were. False where the device
  // cannot.
  virtual bool SetLatencyLimiter(bool on) { return !on; }
  virtual bool LatencyLimiter() const { return false; }
  virtual void WaitForNextFrame() {}
  // (checks) the latency measured: from each input read (WaitForNextFrame's return) to the refresh its frame was
  // first shown at (Direct3D 12: the swap chain's frame statistics; Vulkan: the present wait, where the driver has
  // it), and the gaps between the frames shown; one line printed when the device goes. Set before the first frame.
  bool measureLatency = false;
  // (checks) with vsync, each picture shown for this many refreshes (1 to 4; Direct3D 12's sync interval; Vulkan: 1
  // always): a screen of 240 Hz shows 60 pictures a second with 4
  int syncInterval = 1;
  // The graphics card's time at this point of the frame: a stage of that name starts here and lasts
  // until the next mark or the frame's end. `name` must last (a string literal). At most 62 a frame.
  virtual void Mark(const char* name) = 0;
  // The stages of the newest frame the card has finished, in milliseconds, in their order (a stage
  // marked more than once in a frame is added up). Empty until a frame with marks has finished.
  virtual void TakeTimes(std::vector<std::pair<const char*, double>>* out) = 0;
  // The card's memory this program uses, in bytes (Direct3D 12: as the system counts it; Vulkan: the
  // blocks it has taken of the card's own memory); 0 when not known.
  virtual uint64_t MemoryInUse() = 0;
  // (photofix2) The card's own memory as the system shares it out now: `budget`, how much of it this program may use
  // before the system has to move some of what it holds out to the computer's memory, and `used`, how much it uses
  // (Direct3D 12: IDXGIAdapter3::QueryVideoMemoryInfo, the local group; Vulkan: VK_EXT_memory_budget, the card's
  // largest heap of its own, where the driver has it); the budget no more than `used` and what all programs together
  // leave free on the card (card_memory.h: the system's own budget barely heeds other programs). Bytes; false when not
  // known. Asked when wanted (a photo's plan), not every frame.
  virtual bool MemoryBudget(uint64_t* budget, uint64_t* used) {
    (void)budget;
    (void)used;
    return false;
  }
  // [mcrec] A pass's draws recorded on several threads at once (Recorder above). Within a pass (after BeginFrame or
  // BeginPass, before the next pass, Resolve or Dispatch), BeginParallel(n) hands out n recorders (n from 1 to
  // ParallelRecorders()), each set up as the device is set now (its drawing pipeline, constants, textures, shader buffers,
  // vertex and index buffers, the pass's targets); Parallel(i) is the i-th, which one thread at a time records into (all n
  // at once, on n threads). EndParallel puts what they recorded into the frame in their order, 0 to n-1, after what the
  // device recorded before BeginParallel and before what it records after, and leaves the device set as the last recorder
  // left it (what that one set; the rest as before BeginParallel). Between the two nothing else of the device is called.
  // The picture is the one the same calls made on the device one after the other give, where each recorder's draws set
  // all they read beyond what was set before BeginParallel (each recorder starts from that, not from where the one before
  // it ended). ParallelRecorders 0 (this default): the device cannot; BeginParallel is then false, and the caller draws on
  // the device itself.
  virtual int ParallelRecorders() const { return 0; }
  virtual bool BeginParallel(int count) {
    (void)count;
    return false;
  }
  virtual Recorder* Parallel(int index) {
    (void)index;
    return nullptr;
  }
  virtual void EndParallel() {}
  // The draw calls of the last frame ended (EndFrame).
  int DrawCallsLastFrame() const { return drawsLast_; }

 protected:
  int draws_ = 0, drawsLast_ = 0;  // (each backend counts its draws, and turns them over at EndFrame)
  // [views] SetViewWindow's targets, and a pass into the window made a pass into them
  Texture viewColor_, viewDepth_;
  PassDesc ViewWindowPass(const PassDesc& pass) const {
    PassDesc p = pass;
    p.window = false;
    p.color = viewColor_;
    p.depth = viewDepth_;
    p.depthLayer = 0;
    return p;
  }
};

}  // namespace render::gfx
