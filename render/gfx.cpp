// What gfx.h's backends share: format sizes, and the choice of backend.
#include "gfx.h"

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

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string* error);  // gfx_vulkan.cpp
#if defined(_WIN32)
std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& desc, std::string* error);  // gfx_d3d12.cpp
#endif

// Direct3D 12 on Windows unless Vulkan is asked for; Vulkan on every other system.
std::unique_ptr<Device> Device::Create(const DeviceDesc& desc, std::string* error) {
#if defined(_WIN32)
  if (desc.backend == DeviceDesc::Backend::Direct3D12) return CreateD3D12Device(desc, error);
#endif
  return CreateVulkanDevice(desc, error);
}

}  // namespace render::gfx
