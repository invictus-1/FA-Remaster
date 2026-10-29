// HLSL to SPIR-V with Microsoft's DirectX Shader Compiler (DXC), loaded at run time, for the Vulkan
// backend (gfx_vulkan.cpp); and to DXIL for Direct3D 12 (gfx_d3d12.cpp). The few calls used are declared
// here rather than through dxcapi.h, so that no Windows headers or compiler extensions are needed on any
// system. DXC's IUnknown has three methods and no virtual destructor on each platform.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx.h"

namespace render::gfx {

struct SpirvImage {
  // Texture2D, Texture2DArray, TextureCube, Texture2DMS, Texture3D; a ray tracing scene (RaytracingAccelerationStructure,
  // in a texture's register); a compute shader's storage: RWTexture2D, RWTexture3D
  enum Kind : uint32_t { Plain, Array, Cube, MultiSample, Volume, Scene, Storage, StorageVolume };
  uint32_t binding = 0, count = 1, kind = Plain;
  bool operator==(const SpirvImage& o) const { return binding == o.binding && count == o.count && kind == o.kind; }
  bool operator<(const SpirvImage& o) const { return binding < o.binding; }
};

struct SpirvShader {
  std::vector<uint32_t> words;
  // the vertex shader's inputs: each HLSL semantic (upper case, with its index: "TEXCOORD0") and the
  // location DXC gave it
  std::vector<std::pair<std::string, uint32_t>> inputs;
  // the textures it reads and writes (set 0): each binding, how many it holds (an array of textures, as
  // `Texture2D maps[10] : register(t6)`, is one binding of 10) and their kind; a scene it traces rays
  // through is among them
  std::vector<SpirvImage> images;
};

// Compiles `source` (HLSL) at `entry` for `stage` ("vs", "ps" or "cs") into SPIR-V for Vulkan 1.3, the renderer's
// registers mapped as gfx_vulkan.cpp lays out its descriptor sets. Kept in `cacheDir` (empty: not kept)
// under a hash of everything that goes into it. False with the compiler's messages in `error`.
bool CompileSpirv(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, SpirvShader* out,
                  std::string* error);

// Whether DXC is there, without loading it: on Windows a dxcompiler.dll beside the program (the only place it is
// loaded from). Direct3D 12 decides by it, once, which compiler all its shaders come from: CompileDxil takes kept
// shaders from the cache even where DXC is missing, and a program that switched to Direct3D's own compiler at
// its first shader not kept would draw with a mix of both, different from run to run as the cache fills.
bool DxcPresent();

// Compiles `source` at `entry` for `stage` ("vs", "ps" or "cs") into DXIL, shader model 6.0, for Direct3D 12,
// signed (by DXC itself, or by an older DXC's dxil.dll beside the program). Kept in `cacheDir` as
// CompileSpirv's. False with the compiler's messages in `error`; `missing` is set when that is because DXC
// is not there or cannot sign.
bool CompileDxil(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, std::vector<uint8_t>* out,
                 std::string* error, bool* missing);

// A shader a pipeline will want: its source, entry and stage ("vs", "ps" or "cs").
struct ShaderJob {
  std::string source, entry, stage;
};
// Those of `jobs` the cache (`cacheDir`) lacks, compiled on several threads at once and kept there, as CompileSpirv
// and CompileDxil keep them; they then find them. (What fails here they compile again, and give its messages.)
void PrewarmSpirv(const std::vector<ShaderJob>& jobs, const std::string& cacheDir);
void PrewarmDxil(const std::vector<ShaderJob>& jobs, const std::string& cacheDir);
// [snowhitch] A shader as CompileDxil would give it, taken from the cache alone, never compiled (false: the cache lacks it):
// for a thread of the device's own (Device::BeginBackgroundPipelines), which compiles what the cache lacks by PrewarmDxil
// (a compiler of its own) and must not use the one CompileDxil shares with the frames' thread.
bool CachedDxil(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, std::vector<uint8_t>* out);
// [snowhitch] The same for CompileSpirv's (read as it reads it), after PrewarmSpirv.
bool CachedSpirv(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, SpirvShader* out);

// A pipeline asked for in a batch (Device::BeginPipelineBatch) and not made yet: what it is to be made from, its
// texts owned (the desc's views and names point into them). Both backends keep one in their pipeline's record.
struct PendingPipeline {
  bool compute = false;
  PipelineDesc desc;
  ComputeDesc computeDesc;
  std::string name, vertexSource, pixelSource, vertexEntry, pixelEntry, source, entry;

  static std::shared_ptr<PendingPipeline> Of(const PipelineDesc& d) {
    auto p = std::make_shared<PendingPipeline>();
    p->desc = d;
    p->name = d.name;
    p->vertexSource = d.vertexSource;
    p->pixelSource = d.pixelSource;
    p->vertexEntry = d.vertexEntry;
    p->pixelEntry = d.pixelEntry;
    p->desc.name = p->name.c_str();
    p->desc.vertexSource = p->vertexSource;
    p->desc.pixelSource = p->pixelSource;
    p->desc.vertexEntry = p->vertexEntry.c_str();
    p->desc.pixelEntry = p->pixelEntry.c_str();
    return p;
  }
  static std::shared_ptr<PendingPipeline> Of(const ComputeDesc& d) {
    auto p = std::make_shared<PendingPipeline>();
    p->compute = true;
    p->computeDesc = d;
    p->name = d.name;
    p->source = d.source;
    p->entry = d.entry;
    p->computeDesc.name = p->name.c_str();
    p->computeDesc.source = p->source;
    p->computeDesc.entry = p->entry.c_str();
    return p;
  }
  // its shaders, for Prewarm
  void Jobs(std::vector<ShaderJob>* out) const {
    if (compute) {
      out->push_back({source, entry, "cs"});
      return;
    }
    out->push_back({vertexSource, vertexEntry, "vs"});
    if (!pixelSource.empty()) out->push_back({pixelSource, pixelEntry, "ps"});
  }
};

}  // namespace render::gfx
