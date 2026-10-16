// The thin graphics device interface. Everything the renderer draws goes through it, so a second
// backend (Vulkan) only has to implement this file. The first backend is Direct3D 12 (gfx_d3d12.cpp).
//
// Fixed conventions, the same for every backend:
//  - One window, one swap chain; BeginFrame binds the back buffer and the depth buffer.
//  - Depth is reversed (cleared to 0, a fragment passes when its depth is greater or equal), stored as
//    32-bit float: together with linalg.h's Perspective this keeps precision at strategic-zoom distances.
//  - Shaders are HLSL source (model 5.1), compiled when a pipeline is created.
//    Constant buffers: registers b0..b3, set per draw with SetConstants (the data is copied).
//    Textures: registers t0..t31. Two buffers of plain data (SetShaderBuffer, slots 0 and 1), read
//    in a shader as `StructuredBuffer<T> name : register(t0, space1)` and `register(t1, space1)`.
//    Samplers are fixed:
//      s0 anisotropic wrap, s1 trilinear clamp, s2 point clamp, s3 trilinear wrap, s4 point wrap,
//      s5 shadow comparison (1 where the given depth is nearer to the light than the stored one or
//      equal, the four nearest results blended; outside the map: 1).
//  - Passes: BeginFrame starts one into the window; BeginPass starts one into targets (textures made
//    with CreateTarget) or into the window again. A pipeline says which colour format it draws into.
//  - Handles are plain numbers; 0 means "none". Destroying is deferred until the GPU is done with them.
//  - Buffers: a static buffer gets its data at creation. A dynamic buffer holds no data of its own:
//    UpdateBuffer gives it this frame's contents, which last until EndFrame.
#pragma once
#include <cstddef>
#include <cstdint>
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
  RGBA16F,
  R32F,
  D32,  // depth; only for targets
  BC1,  // DXT1
  BC1_sRGB,
  BC2,  // DXT3
  BC2_sRGB,
  BC3,  // DXT5
  BC3_sRGB,
  BC4,
  BC5,
  BC7,  // (only in files with the newer header; the game's own files have none)
};
bool IsBlockCompressed(Format f);
int BytesPerPixelOrBlock(Format f);  // per pixel, or per 4x4 block for the BC formats

enum class BufferKind : uint8_t { Vertex, Index, Shader };  // Shader: data a shader reads (SetShaderBuffer); dynamic only
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
};

struct DeviceDesc {
  // Direct3D 12 (Windows), or Vulkan (gfx_vulkan.cpp: other systems, and tried on Windows)
  enum class Backend : uint8_t { Direct3D12, Vulkan };
  Backend backend = Backend::Direct3D12;
  // The window, as its system has it. Windows: its HWND in `window`. Elsewhere (Vulkan): Xlib, the
  // X11 Display* in `display` and the Window id in `window`; Wayland, the wl_display* in `display` and
  // the wl_surface* in `window`; Metal (macOS, through MoltenVK), the view's CAMetalLayer* in `window`.
  enum class WindowSystem : uint8_t { Win32, Xlib, Wayland, Metal };
#if defined(_WIN32)
  WindowSystem windowSystem = WindowSystem::Win32;
#elif defined(__APPLE__)
  WindowSystem windowSystem = WindowSystem::Metal;
#else
  WindowSystem windowSystem = WindowSystem::Xlib;
#endif
  void* window = nullptr;
  void* display = nullptr;
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
  virtual Texture CreateTexture(const TextureDesc& desc) = 0;
  // Replaces a rectangle of a texture's first level with `data` (rows `pitch` bytes apart), within a
  // frame (between BeginFrame and EndFrame), before a draw reads it. An uncompressed texture of one
  // level and one layer (RGBA8, BGRA8, R16, ...). For what changes every frame (an interface drawn on
  // the processor), and the ground's heights where the simulation changes them.
  virtual void UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) = 0;
  virtual Texture CreateTarget(const TargetDesc& desc) = 0;
  virtual Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) = 0;
  virtual void Destroy(Buffer buffer) = 0;
  virtual void Destroy(Texture texture) = 0;
  virtual void Destroy(Pipeline pipeline) = 0;

  // Starts the frame and a pass into the window, cleared to that colour.
  virtual void BeginFrame(const float clearColor[4]) = 0;
  // Starts another pass; the targets of the pass before it become readable as textures.
  // The viewport is the whole target. A target must not be bound as a texture in its own pass.
  virtual void BeginPass(const PassDesc& pass) = 0;
  // Averages a multi-sample colour target into an ordinary one (same size and format). Ends the
  // current pass; begin another before drawing again.
  virtual void Resolve(Texture from, Texture to) = 0;
  virtual void SetPipeline(Pipeline pipeline) = 0;
  virtual void SetConstants(int slot, const void* data, uint32_t size) = 0;
  virtual void SetTexture(int slot, Texture texture) = 0;
  virtual void SetVertexBuffer(Buffer buffer) = 0;
  virtual void SetIndexBuffer(Buffer buffer) = 0;
  // A buffer shaders read as a StructuredBuffer (a dynamic buffer of kind Shader, updated this
  // frame), in slot 0 or 1.
  virtual void SetShaderBuffer(Buffer buffer, int slot = 0) = 0;
  virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instances = 1) = 0;
  virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0,
                           uint32_t instances = 1) = 0;
  // Presents the frame. With `capture`, also reads the finished image back: width * height * 4 bytes,
  // RGBA, top row first.
  // `present` false: the frame is drawn (and read back with `capture`) but not shown; the next frame
  // draws into the same picture.
  virtual void EndFrame(std::vector<uint8_t>* capture = nullptr, bool present = true) = 0;
  // The graphics card's time at this point of the frame: a stage of that name starts here and lasts
  // until the next mark or the frame's end. `name` must last (a string literal). At most 62 a frame.
  virtual void Mark(const char* name) = 0;
  // The stages of the newest frame the card has finished, in milliseconds, in their order (a stage
  // marked more than once in a frame is added up). Empty until a frame with marks has finished.
  virtual void TakeTimes(std::vector<std::pair<const char*, double>>* out) = 0;
};

}  // namespace render::gfx
