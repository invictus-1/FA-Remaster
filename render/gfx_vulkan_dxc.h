// HLSL to SPIR-V with Microsoft's DirectX Shader Compiler (DXC), loaded at run time, for the Vulkan
// backend (gfx_vulkan.cpp). The few calls used are declared here rather than through dxcapi.h, so that no
// Windows headers or compiler extensions are needed on any system (on Linux and macOS DXC's objects have a
// virtual destructor in their base, which this file allows for).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace render::gfx {

struct SpirvImage {
  enum Kind : uint32_t { Plain, Array, Cube, MultiSample };  // Texture2D, Texture2DArray, TextureCube, Texture2DMS
  uint32_t binding = 0, count = 1, kind = Plain;
  bool operator==(const SpirvImage& o) const { return binding == o.binding && count == o.count && kind == o.kind; }
  bool operator<(const SpirvImage& o) const { return binding < o.binding; }
};

struct SpirvShader {
  std::vector<uint32_t> words;
  // the vertex shader's inputs: each HLSL semantic (upper case, with its index: "TEXCOORD0") and the
  // location DXC gave it
  std::vector<std::pair<std::string, uint32_t>> inputs;
  // the textures it reads (set 0): each binding, how many it holds (an array of textures, as
  // `Texture2D maps[10] : register(t6)`, is one binding of 10) and their kind
  std::vector<SpirvImage> images;
};

// Compiles `source` (HLSL) at `entry` for `stage` ("vs" or "ps") into SPIR-V for Vulkan 1.3, the renderer's
// registers mapped as gfx_vulkan.cpp lays out its descriptor sets. Kept in `cacheDir` (empty: not kept)
// under a hash of everything that goes into it. False with the compiler's messages in `error`.
bool CompileSpirv(const std::string& source, const char* entry, const char* stage, const std::string& cacheDir, SpirvShader* out,
                  std::string* error);

}  // namespace render::gfx
