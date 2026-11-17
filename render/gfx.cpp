// What gfx.h's backends share: format sizes, and the choice of backend.
#include "gfx.h"

#include "gfx_vulkan_dxc.h"  // [nodxc] (DxcLoad)

#include <cstdio>   // [cardlog] (PrintReport)
#include <cstring>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // [cardlog] (the shader cache's files listed)
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include "log_print.h"  // [logsink] (the last include: its printf / fprintf macros)

namespace render::gfx {

bool IsBlockCompressed(Format f) { return f >= Format::BC1 && f <= Format::BC7; }

int BytesPerPixelOrBlock(Format f) {
  switch (f) {
    case Format::R8: case Format::A8: return 1;
    case Format::RG8: case Format::R16: case Format::R16F: return 2;
    case Format::RGBA8: case Format::RGBA8_sRGB: case Format::BGRA8: case Format::BGRA8_sRGB: case Format::RG16F: case Format::R32F: case Format::R32UI: case Format::D32: return 4;
    case Format::RGBA16F: return 8;
    case Format::RGBA32F: return 16;
    case Format::BC1: case Format::BC1_sRGB: case Format::BC4: return 8;
    case Format::BC2: case Format::BC2_sRGB: case Format::BC3: case Format::BC3_sRGB: case Format::BC5: case Format::BC7: return 16;
    default: return 0;
  }
}

// [spikes] (Device::PrepareTexture and the like, for a backend without halves of its own: the description and its data
// kept, Create* called with them at the finish)
namespace {
struct KeptTexture final : Device::Prepared {
  TextureDesc desc;
  std::vector<uint8_t> data;
};
struct KeptBuffer final : Device::Prepared {
  BufferDesc desc;
  std::vector<uint8_t> data;
};
struct KeptTarget final : Device::Prepared {  // [warmtarget]
  TargetDesc desc;
};
struct KeptGeometry final : Device::Prepared {  // [warmtarget]
  std::vector<GeometryPart> parts;
};
}  // namespace

std::unique_ptr<Device::Prepared> Device::DescribeGeometry(const GeometryPart* parts, int count) {  // [warmtarget]
  auto kept = std::make_unique<KeptGeometry>();
  if (parts && count > 0) kept->parts.assign(parts, parts + count);
  return kept;
}

Structure Device::FinishGeometry(std::unique_ptr<Prepared> prepared) {
  auto* kept = dynamic_cast<KeptGeometry*>(prepared.get());
  return kept ? CreateGeometry(kept->parts.data(), static_cast<int>(kept->parts.size())) : Structure{};
}

std::unique_ptr<Device::Prepared> Device::PrepareTarget(const TargetDesc& desc) {  // [warmtarget]
  auto kept = std::make_unique<KeptTarget>();
  kept->desc = desc;
  return kept;
}

Texture Device::FinishTarget(std::unique_ptr<Prepared> prepared) {
  auto* kept = dynamic_cast<KeptTarget*>(prepared.get());
  return kept ? CreateTarget(kept->desc) : Texture{};
}

std::unique_ptr<Device::Prepared> Device::PrepareTexture(const TextureDesc& desc) {
  if (!desc.data || !desc.dataSize) return nullptr;
  auto kept = std::make_unique<KeptTexture>();
  kept->desc = desc;
  kept->data.assign(static_cast<const uint8_t*>(desc.data), static_cast<const uint8_t*>(desc.data) + desc.dataSize);
  kept->desc.data = kept->data.data();
  return kept;
}

Texture Device::FinishTexture(std::unique_ptr<Prepared> prepared) {
  auto* kept = dynamic_cast<KeptTexture*>(prepared.get());
  return kept ? CreateTexture(kept->desc) : Texture{};
}

std::unique_ptr<Device::Prepared> Device::PrepareBuffer(const BufferDesc& desc) {
  if (desc.dynamic || !desc.data || !desc.size) return nullptr;
  auto kept = std::make_unique<KeptBuffer>();
  kept->desc = desc;
  kept->data.assign(static_cast<const uint8_t*>(desc.data), static_cast<const uint8_t*>(desc.data) + desc.size);
  kept->desc.data = kept->data.data();
  return kept;
}

Buffer Device::FinishBuffer(std::unique_ptr<Prepared> prepared) {
  auto* kept = dynamic_cast<KeptBuffer*>(prepared.get());
  return kept ? CreateBuffer(kept->desc) : Buffer{};
}

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string* error);  // gfx_vulkan.cpp
#if defined(_WIN32)
std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& desc, std::string* error);  // gfx_d3d12.cpp
uint64_t D3D12AdapterMemory();  // gfx_d3d12.cpp ([vram])
#endif

uint64_t Device::AdapterMemory() {  // [vram] (gfx.h)
#if defined(_WIN32)
  return D3D12AdapterMemory();
#else
  return 0;
#endif
}

// [nodxc] (gfx.h) One sentence a player can act on: what is missing, and the one place it must be.
bool ShaderCompilerReady(std::string* message) {
  std::string where, why;
  if (DxcLoad(&where, &why)) return true;
#if defined(_WIN32)
  if (message)
    *message = "the shader compiler is missing: dxcompiler.dll must be beside the program, at " + where + " (" + why +
               "); copy it there from the folder FA Remastered came in, or install FA Remastered again";
#else
  if (message)
    *message = "the shader compiler is missing: " + where + " (" + why + "); it comes with FA Remastered: put it where the system finds " +
               "libraries, or install FA Remastered again";
#endif
  return false;
}

// [cardlog] (gfx.h)
void PrintReport(Device& device) {
  for (const std::string& line : device.Report()) printf("render: card: %s\n", line.c_str());
  fflush(stdout);
}

std::string ShaderWorkLine() {
  const ShaderCounts c = ShaderCountsNow();
  char buf[256];
  snprintf(buf, sizeof buf, "shaders this start: %d compiled by DXC%s; %d shaders taken from the shader cache (those just compiled into it among them)",
           c.compiled, c.failed ? (", " + std::to_string(c.failed) + " refused by it (each pipeline's message says why)").c_str() : "", c.fromCache);
  return buf;
}

std::vector<std::string> ShaderReport(const std::string& cacheDir) {
  std::vector<std::string> out;
  bool loaded = false;
  const std::string where = DxcPlace(&loaded);
  std::string ignored, why;
  const bool there = loaded || DxcLoad(&ignored, &why);
  out.push_back("shader compiler: DXC from " + where + (there ? (loaded ? ", loaded" : ", there (loaded at the first shader)") : ", NOT usable: " + why));
  if (cacheDir.empty()) {
    out.push_back("shader cache: none kept (every shader compiled at every start)");
  } else {
    // (what the folder holds: Direct3D 12's shaders end .dxil, Vulkan's .spv; a .part is one being written or left by a crash)
    int dxil = 0, spirv = 0, other = 0;
    uint64_t bytes = 0;
    bool listed = false;
#if defined(_WIN32)
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((cacheDir + "\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      listed = true;
      do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = f.cFileName;
        const auto ends = [&](const char* e) { return name.size() > strlen(e) && name.compare(name.size() - strlen(e), strlen(e), e) == 0; };
        ++(ends(".dxil") ? dxil : ends(".spv") ? spirv : other);
        bytes += (static_cast<uint64_t>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
      } while (FindNextFileA(h, &f));
      FindClose(h);
    }
#else
    if (DIR* d = opendir(cacheDir.c_str())) {
      listed = true;
      while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const auto ends = [&](const char* x) { return name.size() > strlen(x) && name.compare(name.size() - strlen(x), strlen(x), x) == 0; };
        ++(ends(".dxil") ? dxil : ends(".spv") ? spirv : other);
        struct stat st{};
        if (stat((cacheDir + "/" + name).c_str(), &st) == 0) bytes += static_cast<uint64_t>(st.st_size);
      }
      closedir(d);
    }
#endif
    char buf[512];
    if (listed)
      snprintf(buf, sizeof buf, "shader cache: %s, holding %d shaders for Direct3D 12 and %d for Vulkan (%d other files, the driver's own "
               "pipelines among them), %.1f MB", cacheDir.c_str(), dxil, spirv, other, static_cast<double>(bytes) / (1 << 20));
    else
      snprintf(buf, sizeof buf, "shader cache: %s, whose files could not be listed", cacheDir.c_str());
    out.push_back(buf);
  }
  out.push_back(ShaderWorkLine() + " (so far: the world's shaders come after this report)");
  return out;
}

std::string SystemLine() {
  char buf[256];
#if defined(_WIN32)
  // (RtlGetVersion: the real version; GetVersionEx answers what the program's manifest claims)
  struct OsVersion {
    ULONG size, major, minor, build, platform;
    WCHAR service[128];
  } v = {sizeof(OsVersion), 0, 0, 0, 0, {}};
  using GetVersionFn = LONG(WINAPI*)(OsVersion*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get = ntdll ? reinterpret_cast<GetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
  const bool versioned = get && get(&v) == 0;
  MEMORYSTATUSEX m = {};
  m.dwLength = sizeof m;
  const bool memory = GlobalMemoryStatusEx(&m) != FALSE;
  snprintf(buf, sizeof buf, "system: Windows %s, %lu processor threads, memory %s", versioned ? (std::to_string(v.major) + "." + std::to_string(v.minor) +
           " build " + std::to_string(v.build)).c_str() : "(version not read)", static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
           memory ? (std::to_string(m.ullTotalPhys >> 20) + " MB, " + std::to_string(m.ullAvailPhys >> 20) + " MB free now").c_str() : "not read");
#else
  snprintf(buf, sizeof buf, "system: not Windows (its version is not read here)");
#endif
  return buf;
}

// Direct3D 12 on Windows unless Vulkan is asked for; Vulkan on every other system.
std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, std::string* error) {
  // [nodxc] No device without DXC: every shader of either backend is compiled by it (Vulkan has no other compiler; Direct3D
  // 12 used to go on with its own old one, which hangs in the horizon's pixel shader). Said here, before a window's swap
  // chain or any shader is made, so that a host gets the reason in a second and not a frozen window. Direct3D 12 with the
  // old compiler asked for by name (oldShaderCompiler: the viewer's --fxc) is made as before.
#if defined(_WIN32)
  const bool wantsDxc = desc.backend != DeviceDesc::Backend::Direct3D12 || !desc.oldShaderCompiler;
#else
  const bool wantsDxc = true;
#endif
  if (std::string missing; wantsDxc && !ShaderCompilerReady(&missing)) {
    fprintf(stderr, "render: %s\n", missing.c_str());
    if (error) *error = missing;
    return nullptr;
  }
#if defined(_WIN32)
  if (desc.backend == DeviceDesc::Backend::Direct3D12) return CreateD3D12Device(desc, error);
#endif
  return CreateVulkanDevice(desc, error);
}

}  // namespace render::gfx
