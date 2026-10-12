// The thin graphics device interface. Everything the renderer draws goes through it, so a second
// backend (Vulkan) only has to implement this file. The first backend is Direct3D 12 (gfx_d3d12.cpp).
//
// Fixed conventions, the same for every backend:
//  - One window, one swap chain; BeginFrame binds the back buffer and the depth buffer.
//  - Depth is reversed (cleared to 0, a fragment passes when its depth is greater or equal), stored as
//    32-bit float: together with linalg.h's Perspective this keeps precision at strategic-zoom distances.
//  - Shaders are HLSL source (model 5.1), compiled when a pipeline is created.
//    Constant buffers: registers b0..b3, set per draw with SetConstants (the data is copied).
//    Textures: registers t0..t31. Samplers are fixed:
//      s0 anisotropic wrap, s1 trilinear clamp, s2 point clamp, s3 trilinear wrap, s4 point wrap.
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
  BC1,  // DXT1
  BC1_sRGB,
  BC2,  // DXT3
  BC2_sRGB,
  BC3,  // DXT5
  BC3_sRGB,
  BC4,
  BC5,
};
bool IsBlockCompressed(Format f);
int BytesPerPixelOrBlock(Format f);  // per pixel, or per 4x4 block for the BC formats

enum class BufferKind : uint8_t { Vertex, Index };
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

enum class AttrType : uint8_t { Float1, Float2, Float3, Float4, UByte4, UByte4N, Short2N, Short4N };
struct VertexAttr {
  const char* semantic = nullptr;  // "POSITION", "NORMAL", "TEXCOORD", "COLOR", ...
  int index = 0;
  AttrType type = AttrType::Float3;
  uint32_t offset = 0;
};

enum class Topology : uint8_t { Triangles, Lines };
enum class Cull : uint8_t { None, Back, Front };
enum class Blend : uint8_t { Opaque, Alpha, Premultiplied, Additive };

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
};

struct DeviceDesc {
  void* window = nullptr;  // the platform's window handle (HWND)
  int width = 0, height = 0;
  bool vsync = true;
  bool debug = false;      // the API's validation layer, when installed
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
  virtual Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) = 0;
  virtual void Destroy(Buffer buffer) = 0;
  virtual void Destroy(Texture texture) = 0;
  virtual void Destroy(Pipeline pipeline) = 0;

  virtual void BeginFrame(const float clearColor[4]) = 0;
  virtual void SetPipeline(Pipeline pipeline) = 0;
  virtual void SetConstants(int slot, const void* data, uint32_t size) = 0;
  virtual void SetTexture(int slot, Texture texture) = 0;
  virtual void SetVertexBuffer(Buffer buffer) = 0;
  virtual void SetIndexBuffer(Buffer buffer) = 0;
  virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0, uint32_t instances = 1) = 0;
  virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0,
                           uint32_t instances = 1) = 0;
  // Presents the frame. With `capture`, also reads the finished image back: width * height * 4 bytes,
  // RGBA, top row first.
  virtual void EndFrame(std::vector<uint8_t>* capture = nullptr) = 0;
};

}  // namespace render::gfx
