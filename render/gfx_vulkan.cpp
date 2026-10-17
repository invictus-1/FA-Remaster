// The Vulkan backend of gfx.h: the same calls as gfx_d3d12.cpp, for systems other than Windows (Linux,
// macOS through MoltenVK) and tried on Windows against Direct3D 12, picture for picture. Chris, 2026-10-15:
// after the optimisation, Vulkan next; "I don't have linux so we just have to try and make it as good as
// possible same with mac os on m1". docs/render-vulkan.md has the plan and what is done.
//
// - The window's picture is drawn into an image of our own (RGBA8, with its depth, as Direct3D 12's back
//   buffer) and blitted onto the swap chain's image at the frame's end: pipelines need not know the swap
//   chain's format, and a capture reads our own image.
// - Bindings as Direct3D 12's root signature has them: set 0 holds t0-t39 (images, at 16-55), t0-t1 of
//   space1 (storage buffers, 56-57) and the six fixed samplers (64-69), one set for each combination used
//   in a frame (cached, as Direct3D 12's descriptor tables); set 1 holds b0-b3 (uniform buffers at 0-3),
//   dynamic, pointing into the frame's ring of constants, so a draw only changes four offsets. A compute
//   shader's storage images (u0-u15) are at 80-95 of set 0; a storage target is in the general layout from
//   a dispatch that writes it until a dispatch reads it or a pass begins.
// - Memory comes from large blocks shared out (a card allows only some thousands of allocations).
// - Uploads (a new texture's or buffer's contents) gather in a batch sent ahead of the frame, as in
//   Direct3D 12's backend; the frame's commands begin with a barrier that waits for them.
// - Vulkan's functions are fetched from the loader at run time (vulkan-1.dll, libvulkan.so.1,
//   libMoltenVK.dylib): nothing to link; a machine without Vulkan only fails to make this backend.
// - Ray tracing (gfx.h) where the card offers acceleration structures and ray queries (never MoltenVK): the
//   structures in buffers of their own, built in the frame's commands as Direct3D 12's backend builds them;
//   a scene is bound in a texture's binding of set 0 as an acceleration structure descriptor.

#include "gfx.h"

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define VK_NO_PROTOTYPES 1
#include "third_party/vulkan/include/vulkan/vulkan.h"
#if !defined(_WIN32)
// The other systems' surfaces, declared without their window systems' own headers: only a pointer and
// an id pass through here.
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#include "third_party/vulkan/include/vulkan/vulkan_xlib.h"
struct wl_display;
struct wl_surface;
#include "third_party/vulkan/include/vulkan/vulkan_wayland.h"
#include "third_party/vulkan/include/vulkan/vulkan_metal.h"
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx_vulkan_dxc.h"

// (Vulkan's structures are begun with their type alone, the rest zero)
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace render::gfx {
namespace {

// --- the functions, fetched at run time ---
#define VK_GLOBAL_FUNCTIONS(X)              \
  X(vkCreateInstance)                       \
  X(vkEnumerateInstanceExtensionProperties) \
  X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE_FUNCTIONS(X)               \
  X(vkDestroyInstance)                         \
  X(vkEnumeratePhysicalDevices)                \
  X(vkGetPhysicalDeviceProperties)             \
  X(vkGetPhysicalDeviceQueueFamilyProperties)  \
  X(vkGetPhysicalDeviceMemoryProperties)       \
  X(vkGetPhysicalDeviceFeatures2)              \
  X(vkGetPhysicalDeviceProperties2)            \
  X(vkGetPhysicalDeviceFormatProperties)       \
  X(vkEnumerateDeviceExtensionProperties)      \
  X(vkCreateDevice)                            \
  X(vkGetDeviceProcAddr)                       \
  X(vkDestroySurfaceKHR)                       \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#if defined(_WIN32)
#define VK_SURFACE_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#else
#define VK_SURFACE_FUNCTIONS(X) X(vkCreateXlibSurfaceKHR) X(vkCreateWaylandSurfaceKHR) X(vkCreateMetalSurfaceEXT)
#endif
#define VK_DEVICE_FUNCTIONS(X)          \
  X(vkDestroyDevice)                    \
  X(vkGetDeviceQueue)                   \
  X(vkDeviceWaitIdle)                   \
  X(vkCreateSwapchainKHR)               \
  X(vkDestroySwapchainKHR)              \
  X(vkGetSwapchainImagesKHR)            \
  X(vkAcquireNextImageKHR)              \
  X(vkQueuePresentKHR)                  \
  X(vkQueueSubmit)                      \
  X(vkCreateCommandPool)                \
  X(vkDestroyCommandPool)               \
  X(vkAllocateCommandBuffers)           \
  X(vkResetCommandPool)                 \
  X(vkBeginCommandBuffer)               \
  X(vkEndCommandBuffer)                 \
  X(vkCreateFence)                      \
  X(vkDestroyFence)                     \
  X(vkWaitForFences)                    \
  X(vkResetFences)                      \
  X(vkGetFenceStatus)                   \
  X(vkCreateSemaphore)                  \
  X(vkDestroySemaphore)                 \
  X(vkCreateImage)                      \
  X(vkDestroyImage)                     \
  X(vkCreateImageView)                  \
  X(vkDestroyImageView)                 \
  X(vkCreateBuffer)                     \
  X(vkDestroyBuffer)                    \
  X(vkGetImageMemoryRequirements)       \
  X(vkGetBufferMemoryRequirements)      \
  X(vkAllocateMemory)                   \
  X(vkFreeMemory)                       \
  X(vkBindImageMemory)                  \
  X(vkBindBufferMemory)                 \
  X(vkMapMemory)                        \
  X(vkUnmapMemory)                      \
  X(vkCreateSampler)                    \
  X(vkDestroySampler)                   \
  X(vkCreateDescriptorSetLayout)        \
  X(vkDestroyDescriptorSetLayout)       \
  X(vkCreatePipelineLayout)             \
  X(vkDestroyPipelineLayout)            \
  X(vkCreateDescriptorPool)             \
  X(vkDestroyDescriptorPool)            \
  X(vkResetDescriptorPool)              \
  X(vkAllocateDescriptorSets)           \
  X(vkUpdateDescriptorSets)             \
  X(vkCreateShaderModule)               \
  X(vkDestroyShaderModule)              \
  X(vkCreateGraphicsPipelines)          \
  X(vkCreateComputePipelines)           \
  X(vkDestroyPipeline)                  \
  X(vkCreateQueryPool)                  \
  X(vkDestroyQueryPool)                 \
  X(vkGetQueryPoolResults)              \
  X(vkCmdResetQueryPool)                \
  X(vkCmdWriteTimestamp2)               \
  X(vkCmdPipelineBarrier2)              \
  X(vkCmdBeginRendering)                \
  X(vkCmdEndRendering)                  \
  X(vkCmdSetViewport)                   \
  X(vkCmdSetScissor)                    \
  X(vkCmdBindPipeline)                  \
  X(vkCmdBindDescriptorSets)            \
  X(vkCmdBindVertexBuffers)             \
  X(vkCmdBindIndexBuffer)               \
  X(vkCmdDraw)                          \
  X(vkCmdDrawIndexed)                   \
  X(vkCmdDispatch)                      \
  X(vkCmdBlitImage)                     \
  X(vkCmdCopyImageToBuffer)             \
  X(vkCmdCopyBufferToImage)             \
  X(vkCmdCopyBuffer)                    \
  X(vkCmdResolveImage)
// (fetched only where ray tracing is enabled)
#define VK_RAY_FUNCTIONS(X)                     \
  X(vkGetBufferDeviceAddress)                   \
  X(vkCreateAccelerationStructureKHR)           \
  X(vkDestroyAccelerationStructureKHR)          \
  X(vkGetAccelerationStructureBuildSizesKHR)    \
  X(vkGetAccelerationStructureDeviceAddressKHR) \
  X(vkCmdBuildAccelerationStructuresKHR)
#define VK_DECLARE(name) PFN_##name name = nullptr;
VK_GLOBAL_FUNCTIONS(VK_DECLARE)
VK_INSTANCE_FUNCTIONS(VK_DECLARE)
VK_SURFACE_FUNCTIONS(VK_DECLARE)
VK_DEVICE_FUNCTIONS(VK_DECLARE)
VK_RAY_FUNCTIONS(VK_DECLARE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#undef VK_DECLARE

bool LoadLoader(std::string* error) {
  if (vkGetInstanceProcAddr) return true;
#if defined(_WIN32)
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (lib) vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#elif defined(__APPLE__)
  // (the Vulkan SDK's loader, or MoltenVK itself, which also answers vkGetInstanceProcAddr)
  void* lib = nullptr;
  for (const char* name : {"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"})
    if (!lib) lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (lib) vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#else
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (lib) vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
  if (!vkGetInstanceProcAddr) {
    if (error) *error = "Vulkan: no Vulkan loader on this system";
    return false;
  }
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
  VK_GLOBAL_FUNCTIONS(VK_LOAD)
#undef VK_LOAD
  return vkCreateInstance != nullptr;
}

const char* Result(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "success";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_DEVICE_LOST: return "device lost";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
    case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
    case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
    case VK_SUBOPTIMAL_KHR: return "suboptimal";
    default: return "error";
  }
}

constexpr int kFrames = 3;
constexpr int kTexSlots = 40;
constexpr int kCbSlots = 4;
constexpr uint32_t kTexBinding = 16, kBufferBinding = 56, kSamplerBinding = 64, kStorageBinding = 80;
constexpr int kStorageSlots = 16;  // u0-u15
// (every set's bindings are seen by the drawing stages and by compute shaders alike)
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
// (the shaders that may trace rays through a scene, inline: RayQuery)
constexpr VkPipelineStageFlags2 kRayShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkFormat kBackFormat = VK_FORMAT_R8G8B8A8_UNORM;  // our own picture of the window (as Direct3D 12's)
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkDeviceSize kConstantRing = 32u << 20;  // a frame's constants (grown when a frame needs more)
constexpr VkDeviceSize kUniformRange = 65536;      // what one uniform buffer binding covers
constexpr uint32_t kMarks = 64;

VkFormat ToVk(Format f) {
  switch (f) {
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::A8: return VK_FORMAT_R8_UNORM;  // (its view reads it as alpha)
    case Format::RG8: return VK_FORMAT_R8G8_UNORM;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_sRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_sRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16: return VK_FORMAT_R16_UNORM;
    case Format::R16F: return VK_FORMAT_R16_SFLOAT;
    case Format::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::R32UI: return VK_FORMAT_R32_UINT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32: return VK_FORMAT_D32_SFLOAT;
    case Format::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC1_sRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::BC2_sRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC3_sRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}

VkFormat ToVk(AttrType t) {
  switch (t) {
    case AttrType::Float1: return VK_FORMAT_R32_SFLOAT;
    case AttrType::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case AttrType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case AttrType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case AttrType::UByte4: return VK_FORMAT_R8G8B8A8_UINT;
    case AttrType::UByte4N: return VK_FORMAT_R8G8B8A8_UNORM;
    case AttrType::Short2N: return VK_FORMAT_R16G16_SNORM;
    case AttrType::Short4N: return VK_FORMAT_R16G16B16A16_SNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

// --- memory: large blocks, shared out first-fit; buffers and images in blocks of their own ---
struct Allocation {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, size = 0;
  int block = -1;
  uint8_t* mapped = nullptr;  // host-visible blocks: where it lies in the processor's view
};

class Memory {
 public:
  // (`deviceAddresses`: the buffers' blocks allow their buffers' device addresses, which ray tracing's
  // builds read from)
  void Init(VkDevice device, const VkPhysicalDeviceMemoryProperties& props, bool deviceAddresses) {
    device_ = device;
    props_ = props;
    deviceAddresses_ = deviceAddresses;
  }
  ~Memory() { Release(); }
  void Release() {
    for (Block& b : blocks_)
      if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    blocks_.clear();
  }
  // the blocks taken of the card's own memory, in bytes
  uint64_t DeviceLocalBytes() const {
    uint64_t sum = 0;
    for (const Block& b : blocks_)
      if (b.memory && (props_.memoryTypes[b.type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) sum += b.size;
    return sum;
  }
  uint32_t Type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < props_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (props_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
  }
  bool Get(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool linear, Allocation* out) {
    const uint32_t type = Type(req.memoryTypeBits, want);
    if (type == UINT32_MAX) return false;
    for (size_t i = 0; i < blocks_.size(); ++i) {
      Block& b = blocks_[i];
      if (b.type != type || b.linear != linear) continue;
      for (size_t k = 0; k < b.free.size(); ++k) {
        const VkDeviceSize at = (b.free[k].first + req.alignment - 1) / req.alignment * req.alignment;
        const VkDeviceSize end = b.free[k].first + b.free[k].second;
        if (at + req.size > end) continue;
        // (what is left before and after the piece taken stays free)
        const VkDeviceSize before = at - b.free[k].first, after = end - (at + req.size);
        const VkDeviceSize start = b.free[k].first;
        b.free.erase(b.free.begin() + static_cast<std::ptrdiff_t>(k));
        if (after) b.free.push_back({at + req.size, after});
        if (before) b.free.push_back({start, before});
        *out = {b.memory, at, req.size, static_cast<int>(i), b.mapped ? b.mapped + at : nullptr};
        return true;
      }
    }
    // a new block: 256 MB, or the size asked for when larger
    Block b;
    b.type = type;
    b.linear = linear;
    b.size = std::max<VkDeviceSize>(256ull << 20, (req.size + 0xffff) & ~VkDeviceSize{0xffff});
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = b.size;
    mai.memoryTypeIndex = type;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (linear && deviceAddresses_) mai.pNext = &flags;
    if (vkAllocateMemory(device_, &mai, nullptr, &b.memory) != VK_SUCCESS) return false;
    if (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      void* p = nullptr;
      vkMapMemory(device_, b.memory, 0, b.size, 0, &p);
      b.mapped = static_cast<uint8_t*>(p);
    }
    b.free.push_back({0, b.size});
    blocks_.push_back(std::move(b));
    return Get(req, want, linear, out);
  }
  void Free(const Allocation& a) {
    if (a.block < 0 || static_cast<size_t>(a.block) >= blocks_.size()) return;
    Block& b = blocks_[static_cast<size_t>(a.block)];
    b.free.push_back({a.offset, a.size});
    // (neighbours joined again, so that the block does not crumble)
    std::sort(b.free.begin(), b.free.end());
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> joined;
    for (const auto& r : b.free) {
      if (!joined.empty() && joined.back().first + joined.back().second == r.first) joined.back().second += r.second;
      else joined.push_back(r);
    }
    b.free.swap(joined);
  }

 private:
  struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    bool linear = false;
    VkDeviceSize size = 0;
    uint8_t* mapped = nullptr;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;  // offset, size
  };
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties props_{};
  bool deviceAddresses_ = false;
  std::vector<Block> blocks_;
};

struct BufferRec {
  bool used = false;
  BufferKind kind = BufferKind::Vertex;
  bool dynamic = false;
  uint32_t stride = 0, size = 0;
  VkBuffer buffer = VK_NULL_HANDLE;  // static: its own; dynamic: this frame's ring page
  VkDeviceSize offset = 0;
  Allocation memory;                 // static only
  uint64_t frame = 0;                // dynamic: the frame its contents belong to
};

struct TextureRec {
  bool used = false;
  bool target = false, depth = false;
  int width = 0, height = 0, layers = 1, samples = 1, mips = 1;
  VkFormat format = VK_FORMAT_UNDEFINED;
  int pixelBytes = 4;  // textures: bytes a pixel (UpdateTexture; 0 for the block-compressed)
  VkImage image = VK_NULL_HANDLE;
  Allocation memory;
  VkImageView view = VK_NULL_HANDLE;      // as shaders read it (2D, array, cube, multi-sample)
  std::vector<VkImageView> layerViews;    // depth arrays: each layer as an attachment
  VkImageView attachView = VK_NULL_HANDLE;  // colour targets and single depth targets: as an attachment
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool storage = false;                   // a storage target (written by compute shaders)
  int slices = 1;                         // its slices, when a volume
  std::vector<VkImageView> storageViews;  // storage targets: each level as compute shaders write it
};

struct PipelineRec {
  bool used = false;
  bool compute = false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const void* layout = nullptr;  // VulkanDevice::Layout
  bool depth = true;  // drawn with a depth attachment (Vulkan wants the pass to match)
};

// A ray tracing structure (gfx.h): the structure, the buffer it lies in, and its device address (what an
// instance names it by); for a geometry not built yet, what it is built from (its parts, each part's
// triangles) and the scratch room its build takes.
struct StructureRec {
  bool used = false;
  bool scene = false;
  VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  Allocation memory;
  VkDeviceSize size = 0;
  VkDeviceAddress address = 0;
  std::vector<VkAccelerationStructureGeometryKHR> parts;
  std::vector<uint32_t> triangles;
  VkDeviceSize scratch = 0;
};

struct Page {
  VkBuffer buffer = VK_NULL_HANDLE;
  Allocation memory;
  VkDeviceSize size = 0;
};

struct Span {
  uint8_t* cpu = nullptr;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};

class VulkanDevice final : public Device {
 public:
  ~VulkanDevice() override;
  bool Init(const DeviceDesc& desc, std::string* error);

  const char* BackendName() const override { return "Vulkan"; }
  std::string AdapterName() const override { return adapterName_; }
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  void Resize(int width, int height) override;
  void SetVsync(bool on) override;

  Buffer CreateBuffer(const BufferDesc& desc) override;
  void UpdateBuffer(Buffer buffer, const void* data, uint32_t size) override;
  void* WriteBuffer(Buffer buffer, uint32_t size) override;
  Texture CreateTexture(const TextureDesc& desc) override;
  void UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) override;
  Texture CreateTarget(const TargetDesc& desc) override;
  Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) override;
  Pipeline CreateCompute(const ComputeDesc& desc, std::string* error) override;
  void Destroy(Buffer buffer) override;
  void Destroy(Texture texture) override;
  void Destroy(Pipeline pipeline) override;
  bool RayTracing() const override { return rayTracing_; }
  Structure CreateGeometry(const GeometryPart* parts, int count) override;
  void BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) override;
  void SetScene(int slot, Structure scene) override;
  void Destroy(Structure structure) override;
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
  void EndFrame(std::vector<uint8_t>* capture, bool present) override;
  void Mark(const char* name) override;
  uint64_t MemoryInUse() override { return memory_.DeviceLocalBytes(); }
  void TakeTimes(std::vector<std::pair<const char*, double>>* out) override { *out = lastTimes_; }

 private:
  struct Image {  // the window's picture and its depth (not texture records)
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  bool CreateSwapchain(std::string* error);
  void DestroySwapchain();
  bool CreateWindowImages(std::string* error);
  bool NextConstantPage();
  bool MakeImage(Image* image, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
  void FreeImage(Image* image);
  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect);
  void Transition(TextureRec* t, VkImageLayout to);
  void EndRendering();
  void StartRendering(bool withDepth);
  void ReleasePassTargets();
  void ReleaseGeneral();  // the storage targets dispatches wrote, readable again (outside a pass)
  bool PrepareDraw();
  Span Allocate(VkDeviceSize size, VkDeviceSize align);
  // uploads
  VkCommandBuffer UploadCommands();
  void KeepStaging(VkBuffer buffer, const Allocation& memory, VkDeviceSize bytes);
  void FlushUploads();
  bool StagingBuffer(VkDeviceSize size, VkBuffer* buffer, Allocation* memory);
  // ray tracing
  VkDeviceAddress Address(VkBuffer buffer);
  bool RayBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, Allocation* memory);
  bool MakeStructure(StructureRec* rec, VkAccelerationStructureTypeKHR type, VkDeviceSize size);
  void RetireStructure(StructureRec* rec);
  bool GrowScratch(VkDeviceSize size);
  void BuildGeometry();
  void RayBarrier(VkPipelineStageFlags2 from, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 to, VkAccessFlags2 toAccess);
  VkAccelerationStructureKHR SceneIn(int slot) const;
  // deferred destruction: let go once the card is done with the frames that might use it
  struct Garbage {
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkAccelerationStructureKHR> structures;  // (each before its buffer)
    std::vector<VkBuffer> buffers;
    std::vector<VkPipeline> pipelines;
    std::vector<Allocation> memory;
  };
  void Collect(Garbage& g);
  Garbage& Bin() { return inFrame_ ? frames_[cur_].garbage : frames_[cur_].later; }
  BufferRec* Find(Buffer b) { return b.id && b.id <= buffers_.size() && buffers_[b.id - 1].used ? &buffers_[b.id - 1] : nullptr; }
  TextureRec* Find(Texture t) { return t.id && t.id < textures_.size() && textures_[t.id].used ? &textures_[t.id] : nullptr; }
  PipelineRec* Find(Pipeline p) { return p.id && p.id <= pipelines_.size() && pipelines_[p.id - 1].used ? &pipelines_[p.id - 1] : nullptr; }
  StructureRec* Find(Structure s) { return s.id && s.id <= structures_.size() && structures_[s.id - 1].used ? &structures_[s.id - 1] : nullptr; }

  void* window_ = nullptr;
  int width_ = 0, height_ = 0;
  bool vsync_ = true;
  std::string adapterName_, shaderCache_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props_{};
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  Memory memory_;
  bool nullDescriptors_ = false;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> swapImages_;
  std::vector<VkImageLayout> swapLayouts_;
  std::vector<VkSemaphore> rendered_;  // per swap chain image: the frame drawn, for its showing to wait on
  bool imageHeld_ = false;             // an image of the chain acquired and not yet shown
  bool chainStale_ = false;            // the chain no longer fits the window: made again before the next frame
  VkExtent2D swapExtent_{};
  Image back_, depth_;

  VkSampler samplers_[6] = {};
  VkDescriptorSetLayout constantsLayout_ = VK_NULL_HANDLE;  // set 1: b0-b3
  // Set 0 as each pipeline's shaders declare it: the textures they read (an array of textures is one
  // binding holding several), the two shader buffers and the six samplers. One per arrangement.
  struct Layout {
    std::vector<SpirvImage> images;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline = VK_NULL_HANDLE;
  };
  std::vector<std::unique_ptr<Layout>> layouts_;
  const Layout* LayoutFor(const std::vector<SpirvImage>& images);
  const Layout* layout_ = nullptr;  // the bound pipeline's
  TextureRec dummy_[5];  // what an empty texture slot shows when the card has no null descriptors (by SpirvImage::Kind, to Volume)
  VkBuffer dummyBuffer_ = VK_NULL_HANDLE;  // and an empty shader buffer slot

  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<Page> pages;  // dynamic buffers and staging of UpdateTexture
    size_t page = 0;
    VkDeviceSize offset = 0;
    // the frame's constants (uniform buffers): pages, each with its set (made again each frame), the
    // next one taken when one is full
    std::vector<Page> constantPages;
    std::vector<VkDescriptorSet> constantSets;
    size_t constantPage = 0;
    VkDeviceSize constantsUsed = 0;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkDescriptorSet> sets;  // set 0 by what is bound
    Garbage garbage;  // let go of when this frame is done
    Garbage later;    // let go of outside a frame: after the next frame of this slot (its uploads included)
    std::vector<const char*> marks;
  };
  Frame frames_[kFrames];
  int cur_ = 0;
  uint64_t frameNumber_ = 0;
  uint32_t swapIndex_ = 0;
  bool waitAcquired_ = false;
  bool inFrame_ = false, rendering_ = false;
  // the pass begun: its attachments, and whether it has begun drawing (begun again after a copy in its
  // middle, or without its depth for a pipeline drawn without, it loads what is there)
  uint32_t passColor_ = 0, passDepth_ = 0;
  int passWidth_ = 0, passHeight_ = 0;
  VkImageView passColorView_ = VK_NULL_HANDLE, passDepthView_ = VK_NULL_HANDLE;
  bool passClearColor_ = false, passClearDepth_ = false, passStarted_ = false, renderingDepth_ = false;
  float passClearValue_[4] = {};

  // what the next draw binds
  uint32_t boundTextures_[kTexSlots] = {};
  struct BoundBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
  };
  BoundBuffer boundBuffers_[2];
  bool pipelineDepth_ = true;
  uint32_t constantOffsets_[kCbSlots] = {};
  std::vector<uint8_t> constants_[kCbSlots];  // what each slot was last given (copied on when a page is full)
  bool setDirty_ = true;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // compute: the pipeline set last and its layout, the storage targets bound (texture, level), those in
  // the general layout, and whether the last thing recorded was a dispatch (the next waits for its writes)
  VkPipeline computePipeline_ = VK_NULL_HANDLE;
  const Layout* computeLayout_ = nullptr;
  std::pair<uint32_t, int> boundStorage_[kStorageSlots] = {};
  std::vector<uint32_t> general_;
  bool dispatched_ = false;

  std::vector<BufferRec> buffers_;     // id = index + 1
  std::vector<TextureRec> textures_;   // id = index; 0 is none
  std::vector<PipelineRec> pipelines_; // id = index + 1
  std::vector<uint32_t> freeBuffers_, freeTextures_, freePipelines_;

  // ray tracing (where the card can): the structures, the geometry made since the last scene was built, the
  // scratch room builds share (its address aligned as the card wants a build's scratch), and the scenes
  // bound in the texture slots (their own list: a binding's kind in the shader says which is read)
  bool rayTracing_ = false;
  std::vector<StructureRec> structures_;  // id = index + 1
  std::vector<uint32_t> freeStructures_, pendingGeometry_;
  VkBuffer scratch_ = VK_NULL_HANDLE;
  Allocation scratchMemory_;
  VkDeviceSize scratchSize_ = 0, scratchAlign_ = 256;
  VkDeviceAddress scratchAddress_ = 0;
  uint32_t boundScenes_[kTexSlots] = {};
  // what a slot whose shader declares a scene shows when none is bound there: a scene of nothing, built at
  // the first frame
  Structure emptyScene_;

  // uploads: batches sent ahead of the frame
  static constexpr int kUploadBatches = 4;
  struct Upload {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    bool submitted = false;
    std::vector<std::pair<VkBuffer, Allocation>> staging;
  };
  Upload uploads_[kUploadBatches];
  int upload_ = 0;
  bool uploadOpen_ = false;
  VkDeviceSize uploadBytes_ = 0;

  // timestamps
  VkQueryPool queries_ = VK_NULL_HANDLE;
  double msPerTick_ = 0;
  std::vector<std::pair<const char*, double>> lastTimes_;

  // read-back
  VkBuffer readback_ = VK_NULL_HANDLE;
  Allocation readbackMemory_;
  VkDeviceSize readbackSize_ = 0;
};

VulkanDevice::~VulkanDevice() {
  if (!device_) {
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    return;
  }
  vkDeviceWaitIdle(device_);
  for (Frame& f : frames_) {
    Collect(f.garbage);
    Collect(f.later);
    for (Page& p : f.pages) vkDestroyBuffer(device_, p.buffer, nullptr);
    for (Page& p : f.constantPages) vkDestroyBuffer(device_, p.buffer, nullptr);
    if (f.descriptors) vkDestroyDescriptorPool(device_, f.descriptors, nullptr);
    if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    if (f.done) vkDestroyFence(device_, f.done, nullptr);
    if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
  }
  for (Upload& u : uploads_) {
    for (auto& s : u.staging) vkDestroyBuffer(device_, s.first, nullptr);
    if (u.pool) vkDestroyCommandPool(device_, u.pool, nullptr);
    if (u.done) vkDestroyFence(device_, u.done, nullptr);
  }
  for (BufferRec& b : buffers_)
    if (b.used && !b.dynamic && b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
  for (TextureRec& t : textures_) {
    if (!t.used) continue;
    if (t.view) vkDestroyImageView(device_, t.view, nullptr);
    if (t.attachView) vkDestroyImageView(device_, t.attachView, nullptr);
    for (VkImageView v : t.layerViews) vkDestroyImageView(device_, v, nullptr);
    for (VkImageView v : t.storageViews) vkDestroyImageView(device_, v, nullptr);
    if (t.image) vkDestroyImage(device_, t.image, nullptr);
  }
  for (PipelineRec& p : pipelines_)
    if (p.used && p.pipeline) vkDestroyPipeline(device_, p.pipeline, nullptr);
  for (StructureRec& s : structures_) {
    if (s.used && s.structure) vkDestroyAccelerationStructureKHR(device_, s.structure, nullptr);
    if (s.used && s.buffer) vkDestroyBuffer(device_, s.buffer, nullptr);
  }
  if (scratch_) vkDestroyBuffer(device_, scratch_, nullptr);
  if (queries_) vkDestroyQueryPool(device_, queries_, nullptr);
  if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
  for (const auto& l : layouts_) {
    vkDestroyPipelineLayout(device_, l->pipeline, nullptr);
    vkDestroyDescriptorSetLayout(device_, l->set, nullptr);
  }
  if (constantsLayout_) vkDestroyDescriptorSetLayout(device_, constantsLayout_, nullptr);
  for (VkSampler s : samplers_)
    if (s) vkDestroySampler(device_, s, nullptr);
  DestroySwapchain();
  FreeImage(&back_);
  FreeImage(&depth_);
  memory_.Release();
  vkDestroyDevice(device_, nullptr);
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanDevice::Init(const DeviceDesc& desc, std::string* error) {
  auto fail = [&](const char* what, VkResult r) {
    if (error) *error = std::string("Vulkan: ") + what + ": " + Result(r);
    return false;
  };
  if (!LoadLoader(error)) return false;
  window_ = desc.window;
  width_ = std::max(desc.width, 1);
  height_ = std::max(desc.height, 1);
  vsync_ = desc.vsync;
  // compiled shaders are kept beside Direct3D 12's (gfx_d3d12.cpp's folder), as SPIR-V
  if (desc.shaderCache != "-") {
    std::string dir = desc.shaderCache;
#if defined(_WIN32)
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
#else
    if (dir.empty()) {  // (the usual place for a program's cache: ~/.cache, or XDG_CACHE_HOME)
      const char* xdg = std::getenv("XDG_CACHE_HOME");
      const char* home = std::getenv("HOME");
      std::string base = xdg && *xdg ? std::string(xdg) : home ? std::string(home) + "/.cache" : std::string();
      if (!base.empty()) {
        mkdir(base.c_str(), 0755);
        dir = base + "/fa-remastered-shaders";
      }
    }
    struct stat st{};
    if (!dir.empty() && (mkdir(dir.c_str(), 0755) == 0 || (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)))) shaderCache_ = dir;
#endif
  }

  // the instance: the window system's surface; on macOS MoltenVK, which a loader lists only when asked
  // for "portability" drivers; and (with debug) the validation layer when installed
  bool portability = false;
  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
  {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
    auto offered = [&](const char* name) {
      for (const VkExtensionProperties& e : have)
        if (std::strcmp(e.extensionName, name) == 0) return true;
      return false;
    };
    const char* surface = nullptr;
    if (desc.windowSystem == DeviceDesc::WindowSystem::Host) {  // (the host's own list; each once)
      for (const std::string& name : desc.hostExtensions) {
        if (!offered(name.c_str())) {
          if (error) *error = "Vulkan: this system's Vulkan does not offer " + name + ", which the host's window needs";
          return false;
        }
        bool listed = false;
        for (const char* had : extensions) listed = listed || name == had;
        if (!listed) extensions.push_back(name.c_str());
      }
      surface = VK_KHR_SURFACE_EXTENSION_NAME;  // (listed already)
    }
    switch (desc.windowSystem) {
#if defined(_WIN32)
      case DeviceDesc::WindowSystem::Win32: surface = VK_KHR_WIN32_SURFACE_EXTENSION_NAME; break;
#else
      case DeviceDesc::WindowSystem::Xlib: surface = VK_KHR_XLIB_SURFACE_EXTENSION_NAME; break;
      case DeviceDesc::WindowSystem::Wayland: surface = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME; break;
      case DeviceDesc::WindowSystem::Metal: surface = VK_EXT_METAL_SURFACE_EXTENSION_NAME; break;
#endif
      default: break;
    }
    if (!surface || !offered(surface)) {
      if (error) *error = std::string("Vulkan: this system's Vulkan cannot show in such a window (") + (surface ? surface : "unknown window system") + ")";
      return false;
    }
    if (desc.windowSystem != DeviceDesc::WindowSystem::Host) extensions.push_back(surface);
    if (offered(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      portability = true;
    }
  }
  std::vector<const char*> layers;
  if (desc.debug) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> have(n);
    vkEnumerateInstanceLayerProperties(&n, have.data());
    for (const VkLayerProperties& l : have)
      if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (layers.empty()) fprintf(stderr, "render: Vulkan: --debug: the validation layer is not installed\n");
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "FA Remastered";
  app.pEngineName = "moho64";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  ici.ppEnabledExtensionNames = extensions.data();
  ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  if (portability) ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
  if (r != VK_SUCCESS) return fail("creating the instance", r);
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance_, #name));
  VK_INSTANCE_FUNCTIONS(VK_LOAD)
  VK_SURFACE_FUNCTIONS(VK_LOAD)
#undef VK_LOAD

  if (desc.windowSystem == DeviceDesc::WindowSystem::Host) {  // (the host makes it: SDL3's SDL_Vulkan_CreateSurface)
    const uint64_t made = desc.hostSurface ? desc.hostSurface(static_cast<void*>(instance_)) : 0;
    static_assert(sizeof(VkSurfaceKHR) == sizeof(uint64_t), "a surface handle is 64 bits");
    std::memcpy(&surface_, &made, sizeof surface_);
    r = made ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  } else {
#if defined(_WIN32)
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = GetModuleHandleW(nullptr);
  sci.hwnd = static_cast<HWND>(window_);
  r = vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_);
#else
  // (written to the specification; not tried: there is no Linux or macOS here)
  r = VK_ERROR_INITIALIZATION_FAILED;
  if (desc.windowSystem == DeviceDesc::WindowSystem::Xlib && vkCreateXlibSurfaceKHR) {
    VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = static_cast<Display*>(desc.display);
    sci.window = static_cast<Window>(reinterpret_cast<uintptr_t>(window_));
    r = vkCreateXlibSurfaceKHR(instance_, &sci, nullptr, &surface_);
  } else if (desc.windowSystem == DeviceDesc::WindowSystem::Wayland && vkCreateWaylandSurfaceKHR) {
    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    sci.display = static_cast<wl_display*>(desc.display);
    sci.surface = static_cast<wl_surface*>(window_);
    r = vkCreateWaylandSurfaceKHR(instance_, &sci, nullptr, &surface_);
  } else if (desc.windowSystem == DeviceDesc::WindowSystem::Metal && vkCreateMetalSurfaceEXT) {
    VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    sci.pLayer = static_cast<const CAMetalLayer*>(window_);
    r = vkCreateMetalSurfaceEXT(instance_, &sci, nullptr, &surface_);
  }
#endif
  }
  if (r != VK_SUCCESS) return fail("the window's surface", r);

  // the card: a discrete one first, with a queue that draws and shows on the surface; Vulkan 1.3, or 1.2
  // with dynamic rendering and the second synchronization as extensions (MoltenVK, older drivers)
  auto deviceOffers = [&](VkPhysicalDevice card, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(card, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateDeviceExtensionProperties(card, nullptr, &n, have.data());
    for (const VkExtensionProperties& e : have)
      if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
  };
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> cards(count);
  vkEnumeratePhysicalDevices(instance_, &count, cards.data());
  int bestScore = -1;
  for (VkPhysicalDevice card : cards) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(card, &p);
    if (p.apiVersion < VK_API_VERSION_1_3 &&
        (p.apiVersion < VK_API_VERSION_1_2 || !deviceOffers(card, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
         !deviceOffers(card, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)))
      continue;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(card, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(card, &families, fam.data());
    for (uint32_t i = 0; i < families; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(card, i, surface_, &present);
      if (!(fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
      if (score > bestScore) {
        bestScore = score;
        physical_ = card;
        queueFamily_ = i;
        props_ = p;
        adapterName_ = p.deviceName;
      }
      break;
    }
  }
  if (!physical_) {
    if (error) *error = "Vulkan: no card with Vulkan 1.3 (or 1.2 with dynamic rendering) that can draw into this window";
    return false;
  }
  VkPhysicalDeviceMemoryProperties memoryProps;
  vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProps);

  // the device: Vulkan 1.3's dynamic rendering and second synchronization, anisotropic filtering, and
  // (where the card has them) null descriptors for empty texture slots
  {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, have.data());
    for (const VkExtensionProperties& e : have) nullDescriptors_ = nullDescriptors_ || std::strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0;
  }
  VkPhysicalDeviceRobustness2FeaturesEXT haveRobust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  if (nullDescriptors_) have.pNext = &haveRobust;
  // ray tracing, only where the card offers all it takes: acceleration structures (with the deferred host
  // operations they ask for), ray queries in any shader, and buffers' device addresses (what builds read from)
  bool rays = deviceOffers(physical_, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && deviceOffers(physical_, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
              deviceOffers(physical_, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR haveStructures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR haveQueries{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  if (rays) {
    have12.pNext = &haveStructures;
    haveStructures.pNext = &haveQueries;
    haveQueries.pNext = have.pNext;
    have.pNext = &have12;
  }
  vkGetPhysicalDeviceFeatures2(physical_, &have);
  nullDescriptors_ = nullDescriptors_ && haveRobust.nullDescriptor;
  rays = rays && have12.bufferDeviceAddress && haveStructures.accelerationStructure && haveQueries.rayQuery;
  if (!have.features.textureCompressionBC) fprintf(stderr, "render: Vulkan: this card reads no BC textures (most of the game's)\n");
  float priority = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  robust.nullDescriptor = VK_TRUE;
  const bool core13 = props_.apiVersion >= VK_API_VERSION_1_3;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
  dynamicRendering.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceSynchronization2FeaturesKHR sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
  sync2.synchronization2 = VK_TRUE;
  if (!core13) {
    deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    dynamicRendering.pNext = &sync2;
  }
  // (a driver that is a layer over another API, MoltenVK, says what it cannot do; it must be named)
  if (deviceOffers(physical_, "VK_KHR_portability_subset")) deviceExtensions.push_back("VK_KHR_portability_subset");
  if (nullDescriptors_) {
    deviceExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    (core13 ? f13.pNext : sync2.pNext) = &robust;
  }
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = core13 ? static_cast<void*>(&f13) : static_cast<void*>(&dynamicRendering);
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.bufferDeviceAddress = VK_TRUE;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR structures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  structures.accelerationStructure = VK_TRUE;
  VkPhysicalDeviceRayQueryFeaturesKHR queries{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  queries.rayQuery = VK_TRUE;
  if (rays) {  // (ahead of the rest of the chain)
    for (const char* name : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
      deviceExtensions.push_back(name);
    queries.pNext = features.pNext;
    structures.pNext = &queries;
    f12.pNext = &structures;
    features.pNext = &f12;
  }
  features.features.samplerAnisotropy = have.features.samplerAnisotropy;
  features.features.textureCompressionBC = have.features.textureCompressionBC;
  // (compute shaders write storage images declared without a format, some in the extended formats: R16F, RG16F)
  features.features.shaderStorageImageExtendedFormats = have.features.shaderStorageImageExtendedFormats;
  features.features.shaderStorageImageReadWithoutFormat = have.features.shaderStorageImageReadWithoutFormat;
  features.features.shaderStorageImageWriteWithoutFormat = have.features.shaderStorageImageWriteWithoutFormat;
  if (!have.features.shaderStorageImageWriteWithoutFormat) fprintf(stderr, "render: Vulkan: this card's compute shaders cannot write images without a format\n");
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &features;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  dci.ppEnabledExtensionNames = deviceExtensions.data();
  r = vkCreateDevice(physical_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) return fail("creating the device", r);
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
  VK_DEVICE_FUNCTIONS(VK_LOAD)
  if (rays) {
    VK_RAY_FUNCTIONS(VK_LOAD)
  }
#undef VK_LOAD
  if (!core13) {  // (the same calls under their extensions' names)
#define VK_LOAD_KHR(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name "KHR"));
    VK_LOAD_KHR(vkCmdBeginRendering)
    VK_LOAD_KHR(vkCmdEndRendering)
    VK_LOAD_KHR(vkCmdPipelineBarrier2)
    VK_LOAD_KHR(vkCmdWriteTimestamp2)
#undef VK_LOAD_KHR
  }
#define VK_HAVE(name) && name
  rayTracing_ = rays VK_RAY_FUNCTIONS(VK_HAVE);
#undef VK_HAVE
  if (rayTracing_ && vkGetPhysicalDeviceProperties2) {  // (where a build's scratch room may start)
    VkPhysicalDeviceAccelerationStructurePropertiesKHR sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sp;
    vkGetPhysicalDeviceProperties2(physical_, &p2);
    scratchAlign_ = std::max<VkDeviceSize>(sp.minAccelerationStructureScratchOffsetAlignment, 1);
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  memory_.Init(device_, memoryProps, rayTracing_);

  // the six fixed samplers (as Direct3D 12's static samplers: s0 anisotropic wrap, s1 linear clamp, s2 point
  // clamp, s3 linear wrap, s4 point wrap, s5 the shadow maps' comparison)
  for (int i = 0; i < 6; ++i) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const bool linear = i == 0 || i == 1 || i == 3 || i == 5;
    si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = linear && i != 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const VkSamplerAddressMode mode = i == 1 || i == 2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                      : i == 5         ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                                       : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeU = si.addressModeV = si.addressModeW = mode;
    si.anisotropyEnable = i == 0 && have.features.samplerAnisotropy;
    si.maxAnisotropy = i == 0 ? std::min(16.0f, props_.limits.maxSamplerAnisotropy) : 1;
    si.compareEnable = i == 5;
    si.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.maxLod = VK_LOD_CLAMP_NONE;
    // NVIDIA's Direct3D 12 driver takes its anisotropic readings a quarter of a level sharper than its
    // Vulkan driver (measured 2026-10-15 on an RTX 4090: with -0.25 the ground matches Direct3D 12's to a
    // mean of 0.03 in 255; without anisotropy the two match with no bias). Other makers: not measured.
    if (i == 0 && props_.vendorID == 0x10de) si.mipLodBias = -0.25f;
    if ((r = vkCreateSampler(device_, &si, nullptr, &samplers_[i])) != VK_SUCCESS) return fail("a sampler", r);
  }
  {  // the constants' set layout (the textures' come with the pipelines, LayoutFor)
    std::vector<VkDescriptorSetLayoutBinding> c;
    for (uint32_t i = 0; i < kCbSlots; ++i) c.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, kStages, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(c.size());
    li.pBindings = c.data();
    if ((r = vkCreateDescriptorSetLayout(device_, &li, nullptr, &constantsLayout_)) != VK_SUCCESS) return fail("the constants' set layout", r);
  }

  for (Frame& f : frames_) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    if ((r = vkCreateCommandPool(device_, &pci, nullptr, &f.pool)) != VK_SUCCESS) return fail("a command pool", r);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = f.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(device_, &cai, &f.cmd)) != VK_SUCCESS) return fail("a command buffer", r);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if ((r = vkCreateFence(device_, &fci, nullptr, &f.done)) != VK_SUCCESS) return fail("a fence", r);
    VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if ((r = vkCreateSemaphore(device_, &smi, nullptr, &f.acquired)) != VK_SUCCESS) return fail("a semaphore", r);
    // the frame's descriptor sets (all let go together each time the frame begins)
    // (the fixed samplers count against the pool too)
    // (with ray tracing, a scene for each set of draws and of dispatches)
    VkDescriptorPoolSize sizes[6] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 * kTexSlots},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 * 2},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 8192 * 6},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64 * kCbSlots},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 * kStorageSlots},  // (a set for each dispatch)
                                     {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8192 + 1024}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 8192 + 64 + 1024;
    dpi.poolSizeCount = rayTracing_ ? 6 : 5;
    dpi.pPoolSizes = sizes;
    if ((r = vkCreateDescriptorPool(device_, &dpi, nullptr, &f.descriptors)) != VK_SUCCESS) return fail("a descriptor pool", r);
  }
  for (Upload& u : uploads_) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    if ((r = vkCreateCommandPool(device_, &pci, nullptr, &u.pool)) != VK_SUCCESS) return fail("a command pool", r);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = u.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(device_, &cai, &u.cmd)) != VK_SUCCESS) return fail("a command buffer", r);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if ((r = vkCreateFence(device_, &fci, nullptr, &u.done)) != VK_SUCCESS) return fail("a fence", r);
  }
  {  // the stages' times
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = kFrames * kMarks;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, fam.data());
    if (fam[queueFamily_].timestampValidBits && props_.limits.timestampPeriod > 0 && vkCreateQueryPool(device_, &qpi, nullptr, &queries_) == VK_SUCCESS)
      msPerTick_ = static_cast<double>(props_.limits.timestampPeriod) / 1e6;
  }
  textures_.resize(1);  // (texture 0 is none)
  if (!nullDescriptors_) {  // an empty texture slot then shows black, of the kind the shader reads
    std::vector<uint8_t> black(4 * 6, 0);
    TextureDesc td;
    td.width = td.height = 1;
    td.data = black.data();
    td.dataSize = 4;
    if (TextureRec* rec = Find(CreateTexture(td))) dummy_[SpirvImage::Plain] = *rec;
    td.layers = 2;  // (an array of two: a view of the whole is an array)
    td.dataSize = 8;
    if (TextureRec* rec = Find(CreateTexture(td))) dummy_[SpirvImage::Array] = *rec;
    td.layers = 6;
    td.cube = true;
    td.dataSize = 24;
    if (TextureRec* rec = Find(CreateTexture(td))) dummy_[SpirvImage::Cube] = *rec;
    TargetDesc md;
    md.width = md.height = 1;
    md.format = Format::R32F;
    md.samples = 4;
    if (TextureRec* rec = Find(CreateTarget(md))) dummy_[SpirvImage::MultiSample] = *rec;
    TargetDesc vd;  // (a volume of two slices: one would not be a volume)
    vd.width = vd.height = 1;
    vd.depth = 2;
    vd.format = Format::RGBA8;
    vd.storage = true;
    if (TextureRec* rec = Find(CreateTarget(vd))) dummy_[SpirvImage::Volume] = *rec;
    std::vector<uint8_t> zeros(256, 0);
    BufferDesc bd;
    bd.kind = BufferKind::Shader;
    bd.size = 256;
    bd.data = zeros.data();
    if (BufferRec* rec = Find(CreateBuffer(bd))) dummyBuffer_ = rec->buffer;
  }
  return CreateWindowImages(error) && CreateSwapchain(error);
}

// --- the window ---

bool VulkanDevice::MakeImage(Image* image, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  if (vkCreateImage(device_, &ii, nullptr, &image->image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, image->image, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &image->memory)) return false;
  vkBindImageMemory(device_, image->image, image->memory.memory, image->memory.offset);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image->image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vi, nullptr, &image->view) != VK_SUCCESS) return false;
  image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void VulkanDevice::FreeImage(Image* image) {
  if (image->view) vkDestroyImageView(device_, image->view, nullptr);
  if (image->image) vkDestroyImage(device_, image->image, nullptr);
  if (image->memory.memory) memory_.Free(image->memory);
  *image = Image{};
}

bool VulkanDevice::CreateSwapchain(std::string* error) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
  VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
  for (const VkSurfaceFormatKHR& f : formats)  // (a plain 8-bit one: the picture is already as a screen shows it)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  swapFormat_ = chosen.format;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // (vertical sync: always there)
  if (!vsync_)
    for (VkPresentModeKHR m : modes)
      if (m == VK_PRESENT_MODE_IMMEDIATE_KHR || (m == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) mode = m;
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    if (error) *error = "Vulkan: this window's swap chain cannot be copied into (the picture is shown by a copy)";
    return false;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)};
  if (extent.width == 0 || extent.height == 0) return true;  // (a minimised window: none until it has a size again)
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent;
  swapExtent_ = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  VkResult r = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_);
  if (r != VK_SUCCESS) {
    if (error) *error = std::string("Vulkan: the swap chain: ") + Result(r);
    return false;
  }
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  swapImages_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
  swapLayouts_.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
  rendered_.assign(n, VK_NULL_HANDLE);
  for (VkSemaphore& sem : rendered_) {
    VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &smi, nullptr, &sem);
  }
  return true;
}

// The window's own picture and its depth (the frame is drawn into them, whatever the swap chain does).
bool VulkanDevice::CreateWindowImages(std::string* error) {
  if (!MakeImage(&back_, kBackFormat, width_, height_,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
      !MakeImage(&depth_, kDepthFormat, width_, height_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
    if (error) *error = "Vulkan: the window's picture";
    return false;
  }
  return true;
}

void VulkanDevice::DestroySwapchain() {
  imageHeld_ = false;
  chainStale_ = false;
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  for (VkSemaphore sem : rendered_)
    if (sem) vkDestroySemaphore(device_, sem, nullptr);
  rendered_.clear();
  swapImages_.clear();
  swapLayouts_.clear();
}

void VulkanDevice::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  FlushUploads();
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  FreeImage(&back_);
  FreeImage(&depth_);
  width_ = width;
  height_ = height;
  std::string error;
  if (!CreateWindowImages(&error) || !CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
}

void VulkanDevice::SetVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  FlushUploads();
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  std::string error;
  if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
}

// --- barriers ---

void VulkanDevice::Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &di);
}

void VulkanDevice::Transition(TextureRec* t, VkImageLayout to) {
  if (!t || t->layout == to) return;
  Barrier(frames_[cur_].cmd, t->image, t->layout, to, t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  t->layout = to;
}

// --- memory for the frame: constants, dynamic buffers, staging ---

Span VulkanDevice::Allocate(VkDeviceSize size, VkDeviceSize align) {
  Frame& f = frames_[cur_];
  for (;;) {
    if (f.page < f.pages.size()) {
      Page& p = f.pages[f.page];
      const VkDeviceSize at = (f.offset + align - 1) / align * align;
      if (at + size <= p.size) {
        f.offset = at + size;
        return {p.memory.mapped + at, p.buffer, at};
      }
      ++f.page;
      f.offset = 0;
      continue;
    }
    Page p;
    p.size = std::max<VkDeviceSize>(16ull << 20, (size + 0xffff) & ~VkDeviceSize{0xffff});
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = p.size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (rayTracing_)  // (a scene's instances are written here, and read by its build)
      bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (vkCreateBuffer(device_, &bci, nullptr, &p.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, p.buffer, &req);
    if (!memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, &p.memory)) {
      vkDestroyBuffer(device_, p.buffer, nullptr);
      return {};
    }
    vkBindBufferMemory(device_, p.buffer, p.memory.memory, p.memory.offset);
    f.pages.push_back(p);
  }
}

// --- uploads ---

VkCommandBuffer VulkanDevice::UploadCommands() {
  Upload& u = uploads_[upload_];
  if (!uploadOpen_) {
    // (the batch's commands and staging are free again once the card is done with its last use)
    vkWaitForFences(device_, 1, &u.done, VK_TRUE, UINT64_MAX);
    for (auto& s : u.staging) {
      vkDestroyBuffer(device_, s.first, nullptr);
      memory_.Free(s.second);
    }
    u.staging.clear();
    vkResetCommandPool(device_, u.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(u.cmd, &bi);
    uploadOpen_ = true;
    uploadBytes_ = 0;
  }
  return u.cmd;
}

bool VulkanDevice::StagingBuffer(VkDeviceSize size, VkBuffer* buffer, Allocation* memory) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, *buffer, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, memory)) {
    vkDestroyBuffer(device_, *buffer, nullptr);
    return false;
  }
  vkBindBufferMemory(device_, *buffer, memory->memory, memory->offset);
  return true;
}

void VulkanDevice::KeepStaging(VkBuffer buffer, const Allocation& memory, VkDeviceSize bytes) {
  uploads_[upload_].staging.push_back({buffer, memory});
  uploadBytes_ += bytes;
  if (uploadBytes_ > (256ull << 20)) FlushUploads();  // (a map loading: sent on, so copies do not pile up)
}

void VulkanDevice::FlushUploads() {
  if (!uploadOpen_) return;
  Upload& u = uploads_[upload_];
  vkEndCommandBuffer(u.cmd);
  vkResetFences(device_, 1, &u.done);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &u.cmd;
  vkQueueSubmit(queue_, 1, &si, u.done);
  upload_ = (upload_ + 1) % kUploadBatches;
  uploadOpen_ = false;
  uploadBytes_ = 0;
}

// --- resources ---

Buffer VulkanDevice::CreateBuffer(const BufferDesc& desc) {
  BufferRec rec;
  rec.used = true;
  rec.kind = desc.kind;
  rec.dynamic = desc.dynamic;
  rec.stride = desc.stride;
  if (!desc.dynamic) {
    if (!desc.data || !desc.size) return {};
    rec.size = desc.size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | (desc.kind == BufferKind::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                     : desc.kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                                                      : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (rayTracing_ && desc.kind != BufferKind::Shader)  // (what a geometry structure is built from)
      bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (vkCreateBuffer(device_, &bci, nullptr, &rec.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, rec.buffer, &req);
    if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &rec.memory)) {
      vkDestroyBuffer(device_, rec.buffer, nullptr);
      return {};
    }
    vkBindBufferMemory(device_, rec.buffer, rec.memory.memory, rec.memory.offset);
    VkBuffer staging;
    Allocation stagingMemory;
    if (!StagingBuffer(desc.size, &staging, &stagingMemory)) return {};
    std::memcpy(stagingMemory.mapped, desc.data, desc.size);
    VkBufferCopy copy{0, 0, desc.size};
    vkCmdCopyBuffer(UploadCommands(), staging, rec.buffer, 1, &copy);
    KeepStaging(staging, stagingMemory, desc.size);
  }
  uint32_t index;
  if (!freeBuffers_.empty()) {
    index = freeBuffers_.back();
    freeBuffers_.pop_back();
    buffers_[index] = rec;
  } else {
    index = static_cast<uint32_t>(buffers_.size());
    buffers_.push_back(rec);
  }
  return {index + 1};
}

void VulkanDevice::UpdateBuffer(Buffer buffer, const void* data, uint32_t size) {
  if (void* to = WriteBuffer(buffer, size)) std::memcpy(to, data, size);
}

void* VulkanDevice::WriteBuffer(Buffer buffer, uint32_t size) {
  BufferRec* rec = Find(buffer);
  if (!rec || !rec->dynamic || !inFrame_ || !size) return nullptr;
  Span s = Allocate(size, 256);
  if (!s.cpu) return nullptr;
  rec->buffer = s.buffer;
  rec->offset = s.offset;
  rec->size = size;
  rec->frame = frameNumber_;
  return s.cpu;
}

Texture VulkanDevice::CreateTexture(const TextureDesc& desc) {
  const VkFormat format = ToVk(desc.format);
  if (format == VK_FORMAT_UNDEFINED || desc.width <= 0 || desc.height <= 0 || desc.mips <= 0 || desc.layers <= 0 || !desc.data) return {};
  TextureRec rec;
  rec.used = true;
  rec.width = desc.width;
  rec.height = desc.height;
  rec.layers = desc.layers;
  rec.mips = desc.mips;
  rec.format = format;
  rec.pixelBytes = IsBlockCompressed(desc.format) ? 0 : BytesPerPixelOrBlock(desc.format);
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.flags = desc.cube && desc.layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
  ii.mipLevels = static_cast<uint32_t>(desc.mips);
  ii.arrayLayers = static_cast<uint32_t>(desc.layers);
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return {};
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, rec.image, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) {
    vkDestroyImage(device_, rec.image, nullptr);
    return {};
  }
  vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rec.image;
  vi.viewType = desc.cube && desc.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  if (desc.format == Format::A8)  // (reads as (0, 0, 0, a))
    vi.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  if (desc.format == Format::BC5)  // (red also in alpha: a normal map's x where the game's DXT5 ones keep it)
    vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(desc.mips), 0, static_cast<uint32_t>(desc.layers)};
  if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return {};

  // the caller's images, tightly packed (layer by layer, each with its mips), copied in one go
  const bool blocks = IsBlockCompressed(desc.format);
  const size_t unit = static_cast<size_t>(BytesPerPixelOrBlock(desc.format));
  std::vector<VkBufferImageCopy> copies;
  size_t at = 0, read = 0;  // (in the staging buffer, each piece aligned; in the caller's data, packed)
  for (int layer = 0; layer < desc.layers; ++layer)
    for (int mip = 0; mip < desc.mips; ++mip) {
      const size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
      const size_t bytes = (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
      read += bytes;
      if (read > desc.dataSize) {
        fprintf(stderr, "render: a %dx%d texture with %d mips has too little data (%zu bytes)\n", desc.width, desc.height, desc.mips, desc.dataSize);
        vkDestroyImageView(device_, rec.view, nullptr);
        vkDestroyImage(device_, rec.image, nullptr);
        memory_.Free(rec.memory);
        return {};
      }
      VkBufferImageCopy c{};
      c.bufferOffset = at;
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(mip), static_cast<uint32_t>(layer), 1};
      c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
      copies.push_back(c);
      at += bytes;
      at = (at + 15) & ~size_t{15};  // (each piece starts aligned)
    }
  VkBuffer staging;
  Allocation stagingMemory;
  if (!StagingBuffer(std::max<size_t>(at, 16), &staging, &stagingMemory)) return {};
  {  // (the same walk again, copying, at the aligned places)
    const uint8_t* src = static_cast<const uint8_t*>(desc.data);
    size_t k = 0;
    for (int layer = 0; layer < desc.layers; ++layer)
      for (int mip = 0; mip < desc.mips; ++mip, ++k) {
        const size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
        const size_t bytes = (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
        std::memcpy(stagingMemory.mapped + copies[k].bufferOffset, src, bytes);
        src += bytes;
      }
  }
  VkCommandBuffer cmd = UploadCommands();
  Barrier(cmd, rec.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  vkCmdCopyBufferToImage(cmd, staging, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
  Barrier(cmd, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  KeepStaging(staging, stagingMemory, at);

  uint32_t index;
  if (!freeTextures_.empty()) {
    index = freeTextures_.back();
    freeTextures_.pop_back();
  } else {
    index = static_cast<uint32_t>(textures_.size());
    textures_.emplace_back();
  }
  textures_[index] = rec;
  return {index};
}

void VulkanDevice::UpdateTexture(Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) {
  TextureRec* rec = Find(texture);
  if (!rec || rec->target || rec->storage || !inFrame_ || width <= 0 || height <= 0 || !data || rec->mips != 1 || rec->layers != 1 || x < 0 || y < 0 ||
      x + width > rec->width || y + height > rec->height || rec->pixelBytes <= 0)
    return;
  const size_t row = static_cast<size_t>(width) * static_cast<size_t>(rec->pixelBytes);
  Span s = Allocate(row * static_cast<size_t>(height), 16);
  if (!s.cpu) return;
  for (int r = 0; r < height; ++r) std::memcpy(s.cpu + static_cast<size_t>(r) * row, static_cast<const uint8_t*>(data) + static_cast<size_t>(r) * pitch, row);
  // (a copy cannot lie inside a pass: the pass is left and begun again after it, keeping what it drew)
  const bool wasRendering = rendering_;
  const bool hadDepth = renderingDepth_;
  EndRendering();
  Transition(rec, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferOffset = s.offset;
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageOffset = {x, y, 0};
  c.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  vkCmdCopyBufferToImage(frames_[cur_].cmd, s.buffer, rec->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  Transition(rec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if (wasRendering) StartRendering(hadDepth);
}

Texture VulkanDevice::CreateTarget(const TargetDesc& desc) {
  if (desc.storage) {  // written by compute shaders: never an attachment; a storage view for each level
    const VkFormat format = ToVk(desc.format);
    const bool volume = desc.depth > 1;
    int most = 1;  // (the levels it can have)
    for (int s = std::max({desc.width, desc.height, volume ? desc.depth : 1}); s > 1; s >>= 1) ++most;
    const bool writable = desc.format == Format::RGBA8 || desc.format == Format::R8 || desc.format == Format::R16F || desc.format == Format::RG16F ||
                          desc.format == Format::RGBA16F || desc.format == Format::R32F || desc.format == Format::R32UI || desc.format == Format::RGBA32F;
    VkFormatProperties fp{};
    if (writable) vkGetPhysicalDeviceFormatProperties(physical_, format, &fp);
    if (!writable || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || desc.width <= 0 || desc.height <= 0 || desc.depth <= 0 ||
        desc.layers != 1 || desc.samples > 1 || desc.mips <= 0 || desc.mips > most) {
      fprintf(stderr, "render: Vulkan: a storage target needs RGBA8, R8, R16F, RG16F, RGBA16F, R32F, R32UI or RGBA32F (as the card can write it), one layer, one sample, and levels it can have\n");
      return {};
    }
    TextureRec rec;
    rec.used = rec.storage = true;
    rec.width = desc.width;
    rec.height = desc.height;
    rec.slices = desc.depth;
    rec.mips = desc.mips;
    rec.format = format;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), static_cast<uint32_t>(volume ? desc.depth : 1)};
    ii.mipLevels = static_cast<uint32_t>(desc.mips);
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, rec.image, &req);
    if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) {
      vkDestroyImage(device_, rec.image, nullptr);
      return {};
    }
    vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = rec.image;
    vi.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(desc.mips), 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return {};
    for (int mip = 0; mip < desc.mips; ++mip) {
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(mip), 1, 0, 1};
      VkImageView v;
      if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) return {};
      rec.storageViews.push_back(v);
    }
    // readable from the start, as every target
    Barrier(UploadCommands(), rec.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    uint32_t index;
    if (!freeTextures_.empty()) {
      index = freeTextures_.back();
      freeTextures_.pop_back();
    } else {
      index = static_cast<uint32_t>(textures_.size());
      textures_.emplace_back();
    }
    textures_[index] = rec;
    return {index};
  }
  const bool depth = desc.format == Format::D32;
  const VkFormat format = ToVk(desc.format);
  const int samples = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? desc.samples : 1;
  if (format == VK_FORMAT_UNDEFINED || IsBlockCompressed(desc.format) || desc.width <= 0 || desc.height <= 0 || desc.layers <= 0 ||
      (!depth && desc.layers != 1) || (samples > 1 && desc.layers != 1))
    return {};
  TextureRec rec;
  rec.used = rec.target = true;
  rec.depth = depth;
  rec.width = desc.width;
  rec.height = desc.height;
  rec.layers = desc.layers;
  rec.samples = samples;
  rec.format = format;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = static_cast<uint32_t>(desc.layers);
  ii.samples = static_cast<VkSampleCountFlagBits>(samples);
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return {};
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, rec.image, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) {
    vkDestroyImage(device_, rec.image, nullptr);
    return {};
  }
  vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
  const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rec.image;
  vi.viewType = desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, static_cast<uint32_t>(desc.layers)};
  if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return {};
  for (int layer = 0; layer < desc.layers; ++layer) {  // (as an attachment: one layer at a time)
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = {aspect, 0, 1, static_cast<uint32_t>(layer), 1};
    VkImageView v;
    if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) return {};
    if (desc.layers == 1) rec.attachView = v;
    else rec.layerViews.push_back(v);
  }
  // readable from the start, as Direct3D 12's targets are
  VkCommandBuffer cmd = UploadCommands();
  Barrier(cmd, rec.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect);
  rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  uint32_t index;
  if (!freeTextures_.empty()) {
    index = freeTextures_.back();
    freeTextures_.pop_back();
  } else {
    index = static_cast<uint32_t>(textures_.size());
    textures_.emplace_back();
  }
  textures_[index] = rec;
  return {index};
}

void VulkanDevice::Collect(Garbage& g) {
  for (VkPipeline p : g.pipelines) vkDestroyPipeline(device_, p, nullptr);
  for (VkImageView v : g.views) vkDestroyImageView(device_, v, nullptr);
  for (VkImage i : g.images) vkDestroyImage(device_, i, nullptr);
  for (VkAccelerationStructureKHR s : g.structures) vkDestroyAccelerationStructureKHR(device_, s, nullptr);
  for (VkBuffer b : g.buffers) vkDestroyBuffer(device_, b, nullptr);
  for (const Allocation& a : g.memory) memory_.Free(a);
  g = Garbage{};
}

void VulkanDevice::Destroy(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec) return;
  if (!rec->dynamic) {
    Bin().buffers.push_back(rec->buffer);
    Bin().memory.push_back(rec->memory);
  }
  *rec = BufferRec{};
  freeBuffers_.push_back(buffer.id - 1);
}

void VulkanDevice::Destroy(Texture texture) {
  TextureRec* rec = Find(texture);
  if (!rec) return;
  if (passColor_ == texture.id) passColor_ = 0;
  if (passDepth_ == texture.id) passDepth_ = 0;
  Garbage& g = Bin();
  g.views.push_back(rec->view);
  if (rec->attachView) g.views.push_back(rec->attachView);
  for (VkImageView v : rec->layerViews) g.views.push_back(v);
  for (VkImageView v : rec->storageViews) g.views.push_back(v);
  general_.erase(std::remove(general_.begin(), general_.end(), texture.id), general_.end());
  for (auto& b : boundStorage_)
    if (b.first == texture.id) b = {};
  g.images.push_back(rec->image);
  g.memory.push_back(rec->memory);
  for (uint32_t& t : boundTextures_)
    if (t == texture.id) t = 0;
  setDirty_ = true;
  *rec = TextureRec{};
  freeTextures_.push_back(texture.id);
}

void VulkanDevice::Destroy(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (!rec) return;
  Bin().pipelines.push_back(rec->pipeline);
  *rec = PipelineRec{};
  freePipelines_.push_back(pipeline.id - 1);
}

// --- pipelines ---

const VulkanDevice::Layout* VulkanDevice::LayoutFor(const std::vector<SpirvImage>& images) {
  for (const auto& l : layouts_)
    if (l->images == images) return l.get();
  std::vector<VkDescriptorSetLayoutBinding> b;
  for (const auto& image : images) {
    const bool storage = image.kind == SpirvImage::Storage || image.kind == SpirvImage::StorageVolume;
    const VkDescriptorType type = image.kind == SpirvImage::Scene ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                  : storage                       ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b.push_back({image.binding, type, image.count, kStages, nullptr});
  }
  for (uint32_t i = 0; i < 2; ++i) b.push_back({kBufferBinding + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages, nullptr});
  for (uint32_t i = 0; i < 6; ++i) b.push_back({kSamplerBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kStages, &samplers_[i]});
  auto l = std::make_unique<Layout>();
  l->images = images;
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = static_cast<uint32_t>(b.size());
  li.pBindings = b.data();
  if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &l->set) != VK_SUCCESS) return nullptr;
  const VkDescriptorSetLayout sets[2] = {l->set, constantsLayout_};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 2;
  pli.pSetLayouts = sets;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &l->pipeline) != VK_SUCCESS) {
    vkDestroyDescriptorSetLayout(device_, l->set, nullptr);
    return nullptr;
  }
  layouts_.push_back(std::move(l));
  return layouts_.back().get();
}

Pipeline VulkanDevice::CreatePipeline(const PipelineDesc& desc, std::string* error) {
  SpirvShader vs, ps;
  std::string why;
  if (!CompileSpirv(std::string(desc.vertexSource), desc.vertexEntry, "vs", shaderCache_, &vs, &why)) {
    if (error) *error = std::string(desc.name) + " (" + desc.vertexEntry + "): " + why;
    return {};
  }
  const bool hasPixelShader = !desc.pixelSource.empty();
  if (hasPixelShader && !CompileSpirv(std::string(desc.pixelSource), desc.pixelEntry, "ps", shaderCache_, &ps, &why)) {
    if (error) *error = std::string(desc.name) + " (" + desc.pixelEntry + "): " + why;
    return {};
  }
  VkShaderModule modules[2] = {};
  auto module = [&](const SpirvShader& s, VkShaderModule* out) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = s.words.size() * 4;
    mi.pCode = s.words.data();
    return vkCreateShaderModule(device_, &mi, nullptr, out) == VK_SUCCESS;
  };
  if (!module(vs, &modules[0]) || (hasPixelShader && !module(ps, &modules[1]))) {
    if (error) *error = std::string(desc.name) + ": Vulkan took no shader module";
    for (VkShaderModule m : modules)
      if (m) vkDestroyShaderModule(device_, m, nullptr);
    return {};
  }
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = modules[0];
  stages[0].pName = desc.vertexEntry;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = modules[1];
  stages[1].pName = desc.pixelEntry;

  // the vertex's parts, by the semantic each has in the vertex shader
  std::vector<VkVertexInputAttributeDescription> attributes;
  for (const VertexAttr& a : desc.attrs) {
    std::string semantic = a.semantic ? a.semantic : "";
    for (char& c : semantic) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    semantic += std::to_string(a.index);
    for (const auto& in : vs.inputs)
      if (in.first == semantic) attributes.push_back({in.second, 0, ToVk(a.type), a.offset});
  }
  VkVertexInputBindingDescription binding{0, desc.stride, VK_VERTEX_INPUT_RATE_VERTEX};
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = desc.attrs.empty() ? 0 : 1;
  vin.pVertexBindingDescriptions = &binding;
  vin.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
  vin.pVertexAttributeDescriptions = attributes.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = desc.topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = desc.cull == Cull::None ? VK_CULL_MODE_NONE : desc.cull == Cull::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
  rs.frontFace = desc.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  rs.depthBiasEnable = desc.depthBias != 0;
  rs.depthBiasConstantFactor = desc.depthBias;
  rs.lineWidth = 1;
  const int samples = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? desc.samples : 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples);
  ms.alphaToCoverageEnable = desc.alphaToCoverage && samples > 1;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = desc.depthTest && desc.depthTarget;
  ds.depthWriteEnable = desc.depthWrite && ds.depthTestEnable;  // (as Direct3D 12: no test, no writes)
  ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // (reversed depth)
  VkPipelineColorBlendAttachmentState bt{};
  bt.colorWriteMask = !desc.colorWrite ? 0 : desc.alphaWrite ? 0xf : 0x7;
  bt.colorBlendOp = bt.alphaBlendOp = VK_BLEND_OP_ADD;
  bt.srcColorBlendFactor = bt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  bt.dstColorBlendFactor = bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  switch (desc.blend) {  // (as gfx_d3d12.cpp)
    case Blend::Opaque: break;
    case Blend::Alpha:
      bt.blendEnable = VK_TRUE;
      bt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      bt.dstColorBlendFactor = bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case Blend::Premultiplied:
      bt.blendEnable = VK_TRUE;
      bt.dstColorBlendFactor = bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case Blend::Additive:
      bt.blendEnable = VK_TRUE;
      bt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      bt.dstColorBlendFactor = bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
    case Blend::Multiply:
      bt.blendEnable = VK_TRUE;
      bt.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      bt.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
      bt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
    case Blend::InverseModulate:
      bt.blendEnable = VK_TRUE;
      bt.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      bt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      bt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      bt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = desc.depthOnly ? 0 : 1;
  cb.pAttachments = &bt;
  VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dynamic;
  const VkFormat colorFormat = ToVk(desc.colorFormat);
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = desc.depthOnly ? 0 : 1;
  ri.pColorAttachmentFormats = &colorFormat;
  ri.depthAttachmentFormat = desc.depthTarget ? kDepthFormat : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.pNext = &ri;
  pi.stageCount = hasPixelShader ? 2 : 1;
  pi.pStages = stages;
  pi.pVertexInputState = &vin;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &ds;
  pi.pColorBlendState = &cb;
  pi.pDynamicState = &dy;
  // the textures both shaders read, each binding with the most either declares
  std::vector<SpirvImage> images = vs.images;
  for (const auto& image : ps.images) {
    auto same = std::find_if(images.begin(), images.end(), [&](const auto& i) { return i.binding == image.binding; });
    if (same != images.end()) same->count = std::max(same->count, image.count);
    else images.push_back(image);
  }
  std::sort(images.begin(), images.end());
  for (const auto& image : images) {
    const char* wrong = image.binding < kTexBinding || image.binding + image.count > kTexBinding + kTexSlots || image.kind >= SpirvImage::Storage
                            ? "a texture outside t0-t39"
                        : image.kind == SpirvImage::Scene && !rayTracing_ ? "it traces rays, which this card or its driver cannot"
                                                                          : nullptr;
    if (wrong) {
      if (error) *error = std::string(desc.name) + ": " + wrong;
      for (VkShaderModule m : modules)
        if (m) vkDestroyShaderModule(device_, m, nullptr);
      return {};
    }
  }
  const Layout* layout = LayoutFor(images);
  if (!layout) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline layout";
    for (VkShaderModule m : modules)
      if (m) vkDestroyShaderModule(device_, m, nullptr);
    return {};
  }
  pi.layout = layout->pipeline;
  PipelineRec rec;
  rec.used = true;
  rec.layout = layout;
  rec.depth = desc.depthTarget;
  const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &rec.pipeline);
  for (VkShaderModule m : modules)
    if (m) vkDestroyShaderModule(device_, m, nullptr);
  if (r != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline: " + Result(r);
    return {};
  }
  uint32_t index;
  if (!freePipelines_.empty()) {
    index = freePipelines_.back();
    freePipelines_.pop_back();
    pipelines_[index] = rec;
  } else {
    index = static_cast<uint32_t>(pipelines_.size());
    pipelines_.push_back(rec);
  }
  return {index + 1};
}

Pipeline VulkanDevice::CreateCompute(const ComputeDesc& desc, std::string* error) {
  SpirvShader cs;
  std::string why;
  if (!CompileSpirv(std::string(desc.source), desc.entry, "cs", shaderCache_, &cs, &why)) {
    if (error) *error = std::string(desc.name) + " (" + desc.entry + "): " + why;
    return {};
  }
  std::vector<SpirvImage> images = cs.images;
  std::sort(images.begin(), images.end());
  for (const auto& image : images) {
    const bool storage = image.kind == SpirvImage::Storage || image.kind == SpirvImage::StorageVolume;
    const uint32_t from = storage ? kStorageBinding : kTexBinding, slots = storage ? kStorageSlots : kTexSlots;
    if (image.binding < from || image.binding + image.count > from + slots) {
      if (error) *error = std::string(desc.name) + ": a texture outside t0-t39 or u0-u15";
      return {};
    }
    if (image.kind == SpirvImage::Scene && !rayTracing_) {
      if (error) *error = std::string(desc.name) + ": it traces rays, which this card or its driver cannot";
      return {};
    }
  }
  const Layout* layout = LayoutFor(images);
  if (!layout) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline layout";
    return {};
  }
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.codeSize = cs.words.size() * 4;
  mi.pCode = cs.words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &mi, nullptr, &module) != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan took no shader module";
    return {};
  }
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = module;
  ci.stage.pName = desc.entry;
  ci.layout = layout->pipeline;
  PipelineRec rec;
  rec.used = rec.compute = true;
  rec.layout = layout;
  const VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &rec.pipeline);
  vkDestroyShaderModule(device_, module, nullptr);
  if (r != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no compute pipeline: " + Result(r);
    return {};
  }
  uint32_t index;
  if (!freePipelines_.empty()) {
    index = freePipelines_.back();
    freePipelines_.pop_back();
    pipelines_[index] = rec;
  } else {
    index = static_cast<uint32_t>(pipelines_.size());
    pipelines_.push_back(rec);
  }
  return {index + 1};
}

// --- the frame ---

void VulkanDevice::EndRendering() {
  if (!rendering_) return;
  vkCmdEndRendering(frames_[cur_].cmd);
  rendering_ = false;
}

void VulkanDevice::ReleasePassTargets() {
  for (uint32_t id : {passColor_, passDepth_})
    if (id && id < textures_.size() && textures_[id].used) Transition(&textures_[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  passColor_ = passDepth_ = 0;
}

void VulkanDevice::ReleaseGeneral() {
  dispatched_ = false;
  for (uint32_t id : general_)
    if (id < textures_.size() && textures_[id].used) Transition(&textures_[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  general_.clear();
}

void VulkanDevice::BeginFrame(const float clearColor[4]) {
  Frame& f = frames_[cur_];
  vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
  Collect(f.garbage);
  f.garbage = std::move(f.later);  // (what was let go of since: once this frame is done)
  f.later = Garbage{};
  // the stages' times of this slot's last frame, done now
  if (queries_ && f.marks.size() > 1) {
    std::vector<uint64_t> t(f.marks.size());
    if (vkGetQueryPoolResults(device_, queries_, static_cast<uint32_t>(cur_) * kMarks, static_cast<uint32_t>(f.marks.size()), t.size() * 8, t.data(), 8,
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      lastTimes_.clear();
      for (size_t i = 0; i + 1 < f.marks.size(); ++i) {
        const double ms = t[i + 1] >= t[i] ? static_cast<double>(t[i + 1] - t[i]) * msPerTick_ : 0;
        auto same = std::find_if(lastTimes_.begin(), lastTimes_.end(), [&](const auto& s) { return std::strcmp(s.first, f.marks[i]) == 0; });
        if (same != lastTimes_.end()) same->second += ms;
        else lastTimes_.emplace_back(f.marks[i], ms);
      }
    }
  }
  f.marks.clear();
  waitAcquired_ = false;
  if (!imageHeld_) {  // (an image held by a frame not shown is drawn into again)
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (chainStale_ || !swapchain_) {  // (the window changed, or had no size: a chain anew)
        vkDeviceWaitIdle(device_);
        DestroySwapchain();
        std::string error;
        if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
      }
      if (!swapchain_) break;
      const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &swapIndex_);
      if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        imageHeld_ = waitAcquired_ = true;
        chainStale_ = r == VK_SUBOPTIMAL_KHR;
        break;
      }
      chainStale_ = true;
    }
  }
  vkResetFences(device_, 1, &f.done);
  vkResetCommandPool(device_, f.pool, 0);
  // the frame's memory: its pages and constants from the start, its descriptor sets anew
  f.page = 0;
  f.offset = 0;
  vkResetDescriptorPool(device_, f.descriptors, 0);
  f.sets.clear();
  f.constantSets.clear();
  f.constantPage = SIZE_MAX;
  std::fill(std::begin(constantOffsets_), std::end(constantOffsets_), 0u);
  for (std::vector<uint8_t>& c : constants_) c.clear();
  NextConstantPage();
  ++frameNumber_;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cmd, &bi);
  {  // what uploads sent ahead of this frame wrote is there before the frame reads it
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(f.cmd, &di);
  }
  if (queries_) vkCmdResetQueryPool(f.cmd, queries_, static_cast<uint32_t>(cur_) * kMarks, kMarks);
  inFrame_ = true;
  rendering_ = false;
  pipeline_ = VK_NULL_HANDLE;
  layout_ = nullptr;
  computePipeline_ = VK_NULL_HANDLE;
  computeLayout_ = nullptr;
  for (auto& b : boundStorage_) b = {};
  dispatched_ = false;
  setDirty_ = true;
  std::fill(std::begin(boundTextures_), std::end(boundTextures_), 0u);
  std::fill(std::begin(boundScenes_), std::end(boundScenes_), 0u);
  boundBuffers_[0] = boundBuffers_[1] = BoundBuffer{};
  if (rayTracing_ && !emptyScene_) BuildScene(&emptyScene_, nullptr, 0);  // (the first frame, before anything reads it)
  PassDesc window;
  window.window = true;
  std::memcpy(window.clearValue, clearColor, sizeof window.clearValue);
  BeginPass(window);
}

void VulkanDevice::BeginPass(const PassDesc& pass) {
  if (!inFrame_) return;
  EndRendering();
  ReleaseGeneral();  // (what dispatches wrote, readable by the pass's draws)
  VkCommandBuffer cmd = frames_[cur_].cmd;
  VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
  int width = width_, height = height_;
  if (pass.window) {
    ReleasePassTargets();
    if (back_.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
      Barrier(cmd, back_.image, back_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      back_.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (depth_.layout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
      Barrier(cmd, depth_.image, depth_.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
      depth_.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    colorView = back_.view;
    depthView = depth_.view;
  } else {
    TextureRec* c = Find(pass.color);
    TextureRec* d = Find(pass.depth);
    if ((!c && !d) || (pass.color.id && (!c || !c->target || c->depth)) ||
        (pass.depth.id && (!d || !d->target || !d->depth || pass.depthLayer < 0 || pass.depthLayer >= d->layers))) {
      fprintf(stderr, "render: Vulkan: BeginPass needs a colour target, a depth target, or both\n");
      return;
    }
    // a target that stays in use (the next layer of a shadow map) keeps its layout
    if (passColor_ != pass.color.id || passDepth_ != pass.depth.id) ReleasePassTargets();
    // a target cannot be read while it is drawn into
    for (uint32_t& bound : boundTextures_)
      if (bound && (bound == pass.color.id || bound == pass.depth.id)) {
        bound = 0;
        setDirty_ = true;
      }
    if (c) {
      Transition(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      colorView = c->attachView;
      width = c->width;
      height = c->height;
    }
    if (d) {
      Transition(d, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
      depthView = d->layers > 1 ? d->layerViews[static_cast<size_t>(pass.depthLayer)] : d->attachView;
      width = d->width;
      height = d->height;
    }
    passColor_ = pass.color.id;
    passDepth_ = pass.depth.id;
  }
  passWidth_ = width;
  passHeight_ = height;
  passColorView_ = colorView;
  passDepthView_ = depthView;
  passClearColor_ = pass.clearColor;
  passClearDepth_ = pass.clearDepth;
  std::memcpy(passClearValue_, pass.clearValue, sizeof passClearValue_);
  passStarted_ = false;
  StartRendering(depthView != VK_NULL_HANDLE);
}

// Begins drawing into the pass's attachments: the first time with the pass's clears; again (after a copy
// in the pass's middle, or with or without its depth for a pipeline that wants the other) keeping what is
// there.
void VulkanDevice::StartRendering(bool withDepth) {
  EndRendering();
  VkCommandBuffer cmd = frames_[cur_].cmd;
  withDepth = withDepth && passDepthView_ != VK_NULL_HANDLE;
  if (!withDepth && passDepthView_ && passClearDepth_ && !passStarted_) withDepth = true;  // (its clear first)
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = passColorView_;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = passClearColor_ && !passStarted_ ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  std::memcpy(color.clearValue.color.float32, passClearValue_, sizeof passClearValue_);
  depth.imageView = passDepthView_;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = passClearDepth_ && !passStarted_ ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil.depth = 0;  // (reversed depth: far is 0)
  if (color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || (withDepth && depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)) {
    // (dynamic rendering orders nothing between two drawings into the same attachments)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = mb.srcStageMask;
    mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {static_cast<uint32_t>(passWidth_), static_cast<uint32_t>(passHeight_)}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = passColorView_ ? 1 : 0;
  ri.pColorAttachments = passColorView_ ? &color : nullptr;
  ri.pDepthAttachment = withDepth ? &depth : nullptr;
  vkCmdBeginRendering(cmd, &ri);
  rendering_ = true;
  renderingDepth_ = withDepth;
  passStarted_ = true;
  VkViewport vp{0, 0, static_cast<float>(passWidth_), static_cast<float>(passHeight_), 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {static_cast<uint32_t>(passWidth_), static_cast<uint32_t>(passHeight_)}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
}

void VulkanDevice::Resolve(Texture from, Texture to) {
  TextureRec* src = Find(from);
  TextureRec* dst = Find(to);
  if (!inFrame_ || !src || !dst || !src->target || !dst->target || src->depth || dst->depth || src->samples <= 1 || dst->samples != 1 ||
      src->format != dst->format || src->width != dst->width || src->height != dst->height) {
    fprintf(stderr, "render: Vulkan: Resolve needs a multi-sample colour target and an ordinary one of the same size and format\n");
    return;
  }
  EndRendering();
  ReleasePassTargets();
  Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageResolve region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {static_cast<uint32_t>(src->width), static_cast<uint32_t>(src->height), 1};
  vkCmdResolveImage(frames_[cur_].cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanDevice::SetPipeline(Pipeline pipeline) {
  PipelineRec* rec = Find(pipeline);
  if (!rec || !inFrame_) return;
  if (rec->compute) {  // (its own binding point: the drawing pipeline and its set stay bound)
    computePipeline_ = rec->pipeline;
    computeLayout_ = static_cast<const Layout*>(rec->layout);
    vkCmdBindPipeline(frames_[cur_].cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rec->pipeline);
    return;
  }
  pipeline_ = rec->pipeline;
  pipelineDepth_ = rec->depth;
  if (rec->layout != layout_) {
    layout_ = static_cast<const Layout*>(rec->layout);
    setDirty_ = true;
  }
  vkCmdBindPipeline(frames_[cur_].cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rec->pipeline);
}

// The frame's next page of constants (made the first time a frame needs it, kept after), with a set
// pointing into it; what the slots were last given is copied onto it, so that a draw finds all four there.
bool VulkanDevice::NextConstantPage() {
  Frame& f = frames_[cur_];
  const size_t next = f.constantPage == SIZE_MAX ? 0 : f.constantPage + 1;
  const VkDeviceSize range = std::min<VkDeviceSize>(kUniformRange, props_.limits.maxUniformBufferRange);
  if (next >= f.constantPages.size()) {
    Page p;
    p.size = kConstantRing;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = p.size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &p.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, p.buffer, &req);
    if (!memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, &p.memory)) {
      vkDestroyBuffer(device_, p.buffer, nullptr);
      return false;
    }
    vkBindBufferMemory(device_, p.buffer, p.memory.memory, p.memory.offset);
    f.constantPages.push_back(p);
  }
  while (f.constantSets.size() <= next) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &constantsLayout_;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) return false;
    const Page& p = f.constantPages[f.constantSets.size()];
    VkDescriptorBufferInfo infos[kCbSlots];
    VkWriteDescriptorSet writes[kCbSlots];
    for (int i = 0; i < kCbSlots; ++i) {
      infos[i] = {p.buffer, 0, range};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = set;
      writes[i].dstBinding = static_cast<uint32_t>(i);
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, kCbSlots, writes, 0, nullptr);
    f.constantSets.push_back(set);
  }
  f.constantPage = next;
  f.constantsUsed = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(256, props_.limits.minUniformBufferOffsetAlignment);
  const Page& p = f.constantPages[next];
  for (int slot = 0; slot < kCbSlots; ++slot) {
    constantOffsets_[slot] = 0;
    if (constants_[slot].empty()) continue;
    const VkDeviceSize at = (f.constantsUsed + align - 1) / align * align;
    std::memcpy(p.memory.mapped + at, constants_[slot].data(), constants_[slot].size());
    f.constantsUsed = at + constants_[slot].size();
    constantOffsets_[slot] = static_cast<uint32_t>(at);
  }
  return true;
}

void VulkanDevice::SetConstants(int slot, const void* data, uint32_t size) {
  if (slot < 0 || slot >= kCbSlots || !size || !inFrame_) return;
  Frame& f = frames_[cur_];
  const VkDeviceSize range = std::min<VkDeviceSize>(kUniformRange, props_.limits.maxUniformBufferRange);
  if (size > range) return;
  const VkDeviceSize align = std::max<VkDeviceSize>(256, props_.limits.minUniformBufferOffsetAlignment);
  VkDeviceSize at = (f.constantsUsed + align - 1) / align * align;
  if (at + range > kConstantRing) {  // (this page is full: the next, the other slots' constants copied onto it)
    constants_[slot].clear();
    if (!NextConstantPage()) return;
    at = (f.constantsUsed + align - 1) / align * align;
  }
  std::memcpy(f.constantPages[f.constantPage].memory.mapped + at, data, size);
  f.constantsUsed = at + size;
  constantOffsets_[slot] = static_cast<uint32_t>(at);
  constants_[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
}

void VulkanDevice::SetTexture(int slot, Texture texture) {
  if (slot < 0 || slot >= kTexSlots) return;
  const uint32_t id = Find(texture) ? texture.id : 0;
  if (boundTextures_[slot] == id) return;
  boundTextures_[slot] = id;
  setDirty_ = true;
}

void VulkanDevice::SetVertexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Vertex || (rec->dynamic && rec->frame != frameNumber_) || !inFrame_) return;
  vkCmdBindVertexBuffers(frames_[cur_].cmd, 0, 1, &rec->buffer, &rec->offset);
}

void VulkanDevice::SetIndexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != frameNumber_) || !inFrame_) return;
  vkCmdBindIndexBuffer(frames_[cur_].cmd, rec->buffer, rec->offset, rec->stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanDevice::SetShaderBuffer(Buffer buffer, int slot) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Shader || !rec->dynamic || rec->frame != frameNumber_ || slot < 0 || slot > 1) return;
  boundBuffers_[slot] = {rec->buffer, rec->offset, rec->size};
  setDirty_ = true;
}

void VulkanDevice::SetStorage(int slot, Texture texture, int mip) {
  if (slot < 0 || slot >= kStorageSlots) return;
  const TextureRec* rec = Find(texture);
  boundStorage_[slot] = rec && rec->storage && mip >= 0 && mip < rec->mips ? std::pair<uint32_t, int>{texture.id, mip} : std::pair<uint32_t, int>{0, 0};
}

void VulkanDevice::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (!inFrame_ || !computePipeline_ || !computeLayout_ || !x || !y || !z) return;
  Frame& f = frames_[cur_];
  EndRendering();
  ReleasePassTargets();  // (the pass under way ends: its targets readable)
  auto written = [&](uint32_t id) {
    for (const auto& b : boundStorage_)
      if (b.first == id) return true;
    return false;
  };
  // the storage images it writes into the general layout; what it reads (that a dispatch wrote) out of it
  for (const auto& b : boundStorage_) {
    if (!b.first || !textures_[b.first].used || textures_[b.first].layout == VK_IMAGE_LAYOUT_GENERAL) continue;
    Transition(&textures_[b.first], VK_IMAGE_LAYOUT_GENERAL);
    general_.push_back(b.first);
  }
  for (const auto& image : computeLayout_->images) {
    if (image.kind >= SpirvImage::Storage || image.kind == SpirvImage::Scene) continue;
    for (uint32_t k = 0; k < image.count; ++k) {
      const uint32_t id = boundTextures_[image.binding - kTexBinding + k];
      if (!id || textures_[id].layout != VK_IMAGE_LAYOUT_GENERAL || written(id)) continue;
      Transition(&textures_[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      general_.erase(std::remove(general_.begin(), general_.end(), id), general_.end());
    }
  }
  if (dispatched_) {  // (the dispatch before wrote: finished before this one reads or writes)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(f.cmd, &di);
  }
  // its set: made for each dispatch (they are few)
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = f.descriptors;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &computeLayout_->set;
  if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
    static bool told = false;
    if (!told) fprintf(stderr, "render: Vulkan: out of descriptor sets this frame\n");
    told = true;
    return;
  }
  VkDescriptorImageInfo images[kTexSlots + kStorageSlots];
  VkDescriptorBufferInfo infos[2];
  VkWriteDescriptorSet writes[kTexSlots + kStorageSlots + 2];
  VkAccelerationStructureKHR scenes[kTexSlots];
  VkWriteDescriptorSetAccelerationStructureKHR sceneWrites[kTexSlots];
  uint32_t n = 0, used = 0, scenesUsed = 0, sceneWritesUsed = 0;
  for (const auto& image : computeLayout_->images) {
    if (image.kind == SpirvImage::Scene) {  // (the scenes bound in its slots, each written as an acceleration structure)
      VkWriteDescriptorSetAccelerationStructureKHR& w = sceneWrites[sceneWritesUsed++];
      w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
      w.accelerationStructureCount = image.count;
      w.pAccelerationStructures = &scenes[scenesUsed];
      for (uint32_t k = 0; k < image.count; ++k) scenes[scenesUsed++] = SceneIn(static_cast<int>(image.binding - kTexBinding + k));
      writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[n].pNext = &w;
      writes[n].dstSet = set;
      writes[n].dstBinding = image.binding;
      writes[n].descriptorCount = image.count;
      writes[n].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      ++n;
      continue;
    }
    const bool storage = image.kind >= SpirvImage::Storage;
    const uint32_t first = used;
    bool empty = false;
    for (uint32_t k = 0; k < image.count; ++k) {
      VkImageView view = VK_NULL_HANDLE;
      if (storage) {
        const auto& b = boundStorage_[image.binding - kStorageBinding + k];
        if (b.first && textures_[b.first].used) view = textures_[b.first].storageViews[static_cast<size_t>(b.second)];
      } else {
        const uint32_t id = boundTextures_[image.binding - kTexBinding + k];
        view = id ? textures_[id].view : VK_NULL_HANDLE;
        if (!view && !nullDescriptors_ && image.kind <= SpirvImage::Volume) view = dummy_[image.kind].view;
      }
      empty = empty || (!view && !nullDescriptors_);
      images[used++] = {VK_NULL_HANDLE, view, storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    if (empty) {  // (a storage slot the shader declares left empty, on a card without null descriptors)
      static bool told = false;
      if (!told) fprintf(stderr, "render: Vulkan: a dispatch with an empty storage slot its shader declares\n");
      told = true;
      used = first;
      continue;
    }
    writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[n].dstSet = set;
    writes[n].dstBinding = image.binding;
    writes[n].descriptorCount = image.count;
    writes[n].descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[n].pImageInfo = &images[first];
    ++n;
  }
  for (int i = 0; i < 2; ++i) {
    BoundBuffer b = boundBuffers_[i];
    if (!b.buffer && !nullDescriptors_) b = {dummyBuffer_, 0, 256};
    infos[i] = {b.buffer, b.buffer ? b.offset : 0, b.buffer ? b.size : VK_WHOLE_SIZE};
    writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[n].dstSet = set;
    writes[n].dstBinding = kBufferBinding + static_cast<uint32_t>(i);
    writes[n].descriptorCount = 1;
    writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[n].pBufferInfo = &infos[i];
    ++n;
  }
  vkUpdateDescriptorSets(device_, n, writes, 0, nullptr);
  vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_->pipeline, 0, 1, &set, 0, nullptr);
  if (f.constantPage >= f.constantSets.size()) return;
  vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_->pipeline, 1, 1, &f.constantSets[f.constantPage], kCbSlots, constantOffsets_);
  vkCmdDispatch(f.cmd, x, y, z);
  dispatched_ = true;
}

// --- ray tracing ---

VkDeviceSize Aligned(VkDeviceSize n, VkDeviceSize a) { return (n + a - 1) / a * a; }

VkDeviceAddress VulkanDevice::Address(VkBuffer buffer) {
  VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  ai.buffer = buffer;
  return vkGetBufferDeviceAddress(device_, &ai);
}

// A buffer of the card's own memory for ray tracing (a structure's, or the scratch room of builds), with its
// device address.
bool VulkanDevice::RayBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, Allocation* memory) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, buffer) != VK_SUCCESS) {
    *buffer = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, *buffer, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, memory)) {
    fprintf(stderr, "render: Vulkan: no memory for a ray tracing structure's buffer (%llu bytes)\n", static_cast<unsigned long long>(size));
    vkDestroyBuffer(device_, *buffer, nullptr);
    *buffer = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(device_, *buffer, memory->memory, memory->offset);
  return true;
}

// A structure of that type and size in a buffer of its own, with its address (the record's; false: none).
bool VulkanDevice::MakeStructure(StructureRec* rec, VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
  if (!RayBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &rec->buffer, &rec->memory)) return false;
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = rec->buffer;
  ci.size = size;
  ci.type = type;
  if (vkCreateAccelerationStructureKHR(device_, &ci, nullptr, &rec->structure) != VK_SUCCESS) {
    rec->structure = VK_NULL_HANDLE;
    RetireStructure(rec);
    return false;
  }
  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  ai.accelerationStructure = rec->structure;
  rec->address = vkGetAccelerationStructureDeviceAddressKHR(device_, &ai);
  rec->size = size;
  return true;
}

// A record's structure and buffer, let go of once the frames that might use them are done.
void VulkanDevice::RetireStructure(StructureRec* rec) {
  Garbage& g = Bin();
  if (rec->structure) g.structures.push_back(rec->structure);
  if (rec->buffer) {
    g.buffers.push_back(rec->buffer);
    g.memory.push_back(rec->memory);
  }
  rec->structure = VK_NULL_HANDLE;
  rec->buffer = VK_NULL_HANDLE;
  rec->memory = Allocation{};
  rec->size = 0;
  rec->address = 0;
}

// The scratch room builds share, at least `size` bytes from its (aligned) address.
bool VulkanDevice::GrowScratch(VkDeviceSize size) {
  if (size <= scratchSize_ && scratch_) return true;
  if (scratch_) {  // (a build of a frame still on the card may use it)
    Bin().buffers.push_back(scratch_);
    Bin().memory.push_back(scratchMemory_);
    scratch_ = VK_NULL_HANDLE;
  }
  const VkDeviceSize room = (std::max(size, scratchSize_ + scratchSize_ / 2) + 65535) / 65536 * 65536;
  scratchSize_ = 0;
  // (with room to move its start up to where a build's scratch may begin)
  if (!RayBuffer(room + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch_, &scratchMemory_)) return false;
  scratchAddress_ = Aligned(Address(scratch_), scratchAlign_);
  scratchSize_ = room;
  return true;
}

// What `from` did with `fromAccess` (to ray tracing's structures, or the scratch room of their builds),
// finished and seen before `to` does `toAccess`.
void VulkanDevice::RayBarrier(VkPipelineStageFlags2 from, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 to, VkAccessFlags2 toAccess) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = from;
  mb.srcAccessMask = fromAccess;
  mb.dstStageMask = to;
  mb.dstAccessMask = toAccess;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(frames_[cur_].cmd, &di);
}

// The scene a descriptor of that slot holds: the one bound there, or else the scene of nothing (none at all
// only where that could not be made).
VkAccelerationStructureKHR VulkanDevice::SceneIn(int slot) const {
  for (uint32_t id : {boundScenes_[slot], emptyScene_.id})
    if (id && id <= structures_.size() && structures_[id - 1].used && structures_[id - 1].scene && structures_[id - 1].structure)
      return structures_[id - 1].structure;
  return VK_NULL_HANDLE;
}

Structure VulkanDevice::CreateGeometry(const GeometryPart* parts, int count) {
  if (!rayTracing_ || !parts || count <= 0) return {};
  StructureRec rec;
  for (int i = 0; i < count; ++i) {
    const GeometryPart& p = parts[i];
    const BufferRec* v = Find(p.vertices);
    if (!v || v->dynamic || v->kind != BufferKind::Vertex || !p.vertexCount) return {};
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.flags = p.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    VkAccelerationStructureGeometryTrianglesDataKHR& t = g.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = Address(v->buffer) + static_cast<VkDeviceAddress>(p.firstVertex) * v->stride + p.positionOffset;
    t.vertexStride = v->stride;
    t.maxVertex = p.vertexCount - 1;
    t.indexType = VK_INDEX_TYPE_NONE_KHR;
    uint32_t triangles = p.vertexCount / 3;
    if (p.indices) {  // (counted from firstVertex, as the vertices' address starts there)
      const BufferRec* ix = Find(p.indices);
      if (!ix || ix->dynamic || ix->kind != BufferKind::Index || !p.indexCount) return {};
      t.indexType = ix->stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      t.indexData.deviceAddress = Address(ix->buffer) + static_cast<VkDeviceAddress>(p.firstIndex) * ix->stride;
      triangles = p.indexCount / 3;
    }
    rec.parts.push_back(g);
    rec.triangles.push_back(triangles);
  }
  VkAccelerationStructureBuildGeometryInfoKHR in{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  in.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  in.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  in.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  in.geometryCount = static_cast<uint32_t>(rec.parts.size());
  in.pGeometries = rec.parts.data();
  VkAccelerationStructureBuildSizesInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &in, rec.triangles.data(), &info);
  if (!info.accelerationStructureSize) return {};
  rec.scratch = Aligned(std::max<VkDeviceSize>(info.buildScratchSize, 1), scratchAlign_);
  if (!MakeStructure(&rec, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, Aligned(info.accelerationStructureSize, 256))) return {};
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

void VulkanDevice::BuildGeometry() {
  if (pendingGeometry_.empty()) return;
  // (built side by side in the scratch room, in batches of at most 64 MB of it, each batch waited for)
  constexpr VkDeviceSize kBatch = 64ull << 20;
  constexpr VkPipelineStageFlags2 build = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  constexpr VkAccessFlags2 written = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                           both = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  VkDeviceSize largest = 0;
  for (uint32_t i : pendingGeometry_)
    if (structures_[i].used && !structures_[i].parts.empty()) largest = std::max(largest, structures_[i].scratch);
  if (!largest || !GrowScratch(std::max(largest, std::min(kBatch, scratchSize_)))) {
    pendingGeometry_.clear();
    return;
  }
  VkCommandBuffer cmd = frames_[cur_].cmd;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  VkDeviceSize at = 0;
  for (uint32_t i : pendingGeometry_) {
    StructureRec& s = structures_[i];
    if (!s.used || s.parts.empty()) continue;
    if (at + s.scratch > scratchSize_) {  // (the batch before done with the scratch room first)
      RayBarrier(build, written, build, both);
      at = 0;
    }
    VkAccelerationStructureBuildGeometryInfoKHR b{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    b.dstAccelerationStructure = s.structure;
    b.geometryCount = static_cast<uint32_t>(s.parts.size());
    b.pGeometries = s.parts.data();
    b.scratchData.deviceAddress = scratchAddress_ + at;
    ranges.assign(s.parts.size(), VkAccelerationStructureBuildRangeInfoKHR{});
    for (size_t k = 0; k < s.parts.size(); ++k) ranges[k].primitiveCount = s.triangles[k];
    const VkAccelerationStructureBuildRangeInfoKHR* r = ranges.data();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &b, &r);
    at += s.scratch;
    s.parts.clear();
    s.parts.shrink_to_fit();
    s.triangles.clear();
    s.triangles.shrink_to_fit();
  }
  pendingGeometry_.clear();
  RayBarrier(build, written, build | kRayShaders, both);  // (for the scene's build, and the shaders after)
}

void VulkanDevice::BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) {
  if (!rayTracing_ || !inFrame_ || !scene) return;
  EndRendering();
  ReleasePassTargets();  // (the pass under way ends)
  BuildGeometry();
  // the instances, in this frame's memory
  const Span s = Allocate(sizeof(VkAccelerationStructureInstanceKHR) * std::max<uint32_t>(count, 1), 16);
  if (!s.cpu) return;
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const StructureRec* g = instances ? Find(instances[i].geometry) : nullptr;
    if (!g || g->scene || !g->parts.empty()) continue;  // (and a geometry whose build could not be made)
    VkAccelerationStructureInstanceKHR d{};
    std::memcpy(d.transform.matrix, instances[i].transform, sizeof d.transform.matrix);
    d.instanceCustomIndex = instances[i].id & 0xffffffu;
    d.mask = instances[i].mask;
    d.accelerationStructureReference = g->address;
    std::memcpy(s.cpu + sizeof d * n++, &d, sizeof d);
  }
  VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress = Address(s.buffer) + s.offset;
  VkAccelerationStructureBuildGeometryInfoKHR in{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  in.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  in.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  in.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  in.geometryCount = 1;
  in.pGeometries = &geometry;
  VkAccelerationStructureBuildSizesInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &in, &n, &info);
  StructureRec* rec = Find(*scene);
  if (!rec) {  // (the first time: the scene's record)
    StructureRec r;
    r.used = r.scene = true;
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
  if (!rec->scene) return;
  const VkDeviceSize need = Aligned(std::max<VkDeviceSize>(info.accelerationStructureSize, 256), 256);
  if (rec->size < need) {  // (made, or grown by a half more than it needs: a structure anew, which sets name anew)
    RetireStructure(rec);
    if (!MakeStructure(rec, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, Aligned(need + need / 2, 256))) return;
    setDirty_ = true;
  }
  if (!GrowScratch(Aligned(std::max<VkDeviceSize>(info.buildScratchSize, 1), scratchAlign_))) {
    RetireStructure(rec);  // (not built: none, rather than one a shader must not read)
    setDirty_ = true;
    return;
  }
  constexpr VkPipelineStageFlags2 build = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  // (what shaders read of it before, finished first; what the build writes, for the builds and shaders after)
  RayBarrier(kRayShaders, 0, build, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
  in.dstAccelerationStructure = rec->structure;
  in.scratchData.deviceAddress = scratchAddress_;
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = n;
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  vkCmdBuildAccelerationStructuresKHR(frames_[cur_].cmd, 1, &in, &ranges);
  RayBarrier(build, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, build | kRayShaders,
             VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
}

void VulkanDevice::SetScene(int slot, Structure scene) {
  if (slot < 0 || slot >= kTexSlots) return;
  const StructureRec* rec = Find(scene);
  const uint32_t id = rec && rec->scene && rec->structure ? scene.id : 0;
  if (boundScenes_[slot] == id) return;
  boundScenes_[slot] = id;
  setDirty_ = true;
}

void VulkanDevice::Destroy(Structure structure) {
  StructureRec* rec = Find(structure);
  if (!rec) return;
  RetireStructure(rec);
  for (uint32_t& s : boundScenes_)
    if (s == structure.id) {
      s = 0;
      setDirty_ = true;
    }
  const uint32_t index = structure.id - 1;
  pendingGeometry_.erase(std::remove(pendingGeometry_.begin(), pendingGeometry_.end(), index), pendingGeometry_.end());
  *rec = StructureRec{};
  freeStructures_.push_back(index);
}

// The textures' and buffers' set for what is bound now (made once a frame for each combination), and the
// constants' offsets.
bool VulkanDevice::PrepareDraw() {
  if (!inFrame_ || !rendering_ || !pipeline_ || !layout_) return false;
  Frame& f = frames_[cur_];
  // a pipeline drawn without depth into a pass that has it (or the other way round): Vulkan wants the
  // pass to match, so it is begun again (keeping what is drawn) as the pipeline wants
  if (pipelineDepth_ != renderingDepth_ && passDepthView_) StartRendering(pipelineDepth_);
  VkCommandBuffer cmd = f.cmd;
  if (setDirty_) {
    // what the set holds, by the layout, the views and the buffers themselves (a handle's number may be
    // used again)
    VkImageView views[kTexSlots];
    for (int i = 0; i < kTexSlots; ++i) views[i] = boundTextures_[i] ? textures_[boundTextures_[i]].view : VK_NULL_HANDLE;
    if (!nullDescriptors_)
      for (const auto& image : layout_->images)
        for (uint32_t k = 0; k < image.count; ++k)
          if (!views[image.binding - kTexBinding + k] && image.kind <= SpirvImage::Volume) views[image.binding - kTexBinding + k] = dummy_[image.kind].view;
    // (and the scenes, in the slots where the shaders declare one: the structure itself, which a scene grown
    // has anew)
    VkAccelerationStructureKHR scenes[kTexSlots] = {};
    for (const auto& image : layout_->images)
      if (image.kind == SpirvImage::Scene)
        for (uint32_t k = 0; k < image.count; ++k) scenes[image.binding - kTexBinding + k] = SceneIn(static_cast<int>(image.binding - kTexBinding + k));
    BoundBuffer buffers[2] = {boundBuffers_[0], boundBuffers_[1]};
    for (BoundBuffer& b : buffers)
      if (!b.buffer && !nullDescriptors_) b = {dummyBuffer_, 0, 256};
    std::string key(reinterpret_cast<const char*>(&layout_), sizeof layout_);
    for (const auto& image : layout_->images)
      if (image.kind == SpirvImage::Scene) key.append(reinterpret_cast<const char*>(&scenes[image.binding - kTexBinding]), image.count * sizeof(VkAccelerationStructureKHR));
      else key.append(reinterpret_cast<const char*>(&views[image.binding - kTexBinding]), image.count * sizeof(VkImageView));
    for (const BoundBuffer& b : buffers) {
      key.append(reinterpret_cast<const char*>(&b.buffer), sizeof b.buffer);
      key.append(reinterpret_cast<const char*>(&b.offset), sizeof b.offset);
      key.append(reinterpret_cast<const char*>(&b.size), sizeof b.size);
    }
    VkDescriptorSet set;
    auto known = f.sets.find(key);
    if (known != f.sets.end()) {
      set = known->second;
    } else {
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = f.descriptors;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &layout_->set;
      if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        static bool told = false;
        if (!told) fprintf(stderr, "render: Vulkan: out of descriptor sets this frame\n");
        told = true;
        return false;
      }
      VkDescriptorImageInfo images[kTexSlots];
      VkDescriptorBufferInfo infos[2];
      VkWriteDescriptorSet writes[kTexSlots + 2];
      VkWriteDescriptorSetAccelerationStructureKHR sceneWrites[kTexSlots];
      uint32_t n = 0;
      for (const auto& image : layout_->images) {  // (an array's elements written together)
        const uint32_t first = image.binding - kTexBinding;
        writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet = set;
        writes[n].dstBinding = image.binding;
        writes[n].descriptorCount = image.count;
        if (image.kind == SpirvImage::Scene) {
          sceneWrites[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
          sceneWrites[n].accelerationStructureCount = image.count;
          sceneWrites[n].pAccelerationStructures = &scenes[first];
          writes[n].pNext = &sceneWrites[n];
          writes[n].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        } else {
          for (uint32_t k = 0; k < image.count; ++k) images[first + k] = {VK_NULL_HANDLE, views[first + k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          writes[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[n].pImageInfo = &images[first];
        }
        ++n;
      }
      for (int i = 0; i < 2; ++i) {
        const BoundBuffer& b = buffers[i];
        infos[i] = {b.buffer, b.buffer ? b.offset : 0, b.buffer ? b.size : VK_WHOLE_SIZE};
        writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet = set;
        writes[n].dstBinding = kBufferBinding + static_cast<uint32_t>(i);
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[n].pBufferInfo = &infos[i];
        ++n;
      }
      vkUpdateDescriptorSets(device_, n, writes, 0, nullptr);
      f.sets.emplace(std::move(key), set);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline, 0, 1, &set, 0, nullptr);
    setDirty_ = false;
  }
  if (f.constantPage >= f.constantSets.size()) return false;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline, 1, 1, &f.constantSets[f.constantPage], kCbSlots, constantOffsets_);
  return true;
}

void VulkanDevice::Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) {
  if (!PrepareDraw()) return;
  ++draws_;
  vkCmdDraw(frames_[cur_].cmd, vertexCount, instances, firstVertex, 0);
}

void VulkanDevice::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) {
  if (!PrepareDraw()) return;
  ++draws_;
  vkCmdDrawIndexed(frames_[cur_].cmd, indexCount, instances, firstIndex, baseVertex, 0);
}

void VulkanDevice::Mark(const char* name) {
  Frame& f = frames_[cur_];
  if (!queries_ || !inFrame_ || f.marks.size() >= kMarks - 1) return;
  vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, static_cast<uint32_t>(cur_) * kMarks + static_cast<uint32_t>(f.marks.size()));
  f.marks.push_back(name);
}

void VulkanDevice::EndFrame(std::vector<uint8_t>* capture, bool present) {
  if (!inFrame_) return;
  drawsLast_ = draws_;
  draws_ = 0;
  Frame& f = frames_[cur_];
  EndRendering();
  ReleasePassTargets();
  ReleaseGeneral();
  VkCommandBuffer cmd = f.cmd;
  if (queries_ && !f.marks.empty()) {  // the frame's end
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, static_cast<uint32_t>(cur_) * kMarks + static_cast<uint32_t>(f.marks.size()));
    f.marks.push_back("(end)");
  }
  // our picture onto the swap chain's image (a copy that also turns RGBA into the chain's order)
  Barrier(cmd, back_.image, back_.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  back_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (capture) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(width_) * static_cast<VkDeviceSize>(height_) * 4;
    if (readbackSize_ < need) {
      if (readback_) {
        vkDeviceWaitIdle(device_);
        vkDestroyBuffer(device_, readback_, nullptr);
        memory_.Free(readbackMemory_);
      }
      VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bci.size = need;
      bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      vkCreateBuffer(device_, &bci, nullptr, &readback_);
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device_, readback_, &req);
      // (memory the processor reads through its cache: from the uncached kind, 15 MB take a tenth of a second)
      if (!memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true,
                       &readbackMemory_))
        memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, &readbackMemory_);
      vkBindBufferMemory(device_, readback_, readbackMemory_.memory, readbackMemory_.offset);
      readbackSize_ = need;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyImageToBuffer(cmd, back_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};  // (what the copy wrote, for the processor to read)
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
  }
  const bool show = present && imageHeld_;
  if (show) {
    VkImage swap = swapImages_[swapIndex_];
    Barrier(cmd, swap, swapLayouts_[swapIndex_], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {width_, height_, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(swapExtent_.width), static_cast<int32_t>(swapExtent_.height), 1};
    vkCmdBlitImage(cmd, back_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Barrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    swapLayouts_[swapIndex_] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  }
  vkEndCommandBuffer(cmd);
  FlushUploads();  // (what this frame draws with goes to the card ahead of it)
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = waitAcquired_ ? 1 : 0;
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = show ? 1 : 0;
  si.pSignalSemaphores = show ? &rendered_[swapIndex_] : nullptr;
  vkQueueSubmit(queue_, 1, &si, f.done);
  if (show) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[swapIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &swapIndex_;
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r != VK_SUCCESS) chainStale_ = true;  // (out of date or suboptimal: a new chain before the next frame)
    imageHeld_ = false;
  }
  inFrame_ = false;
  if (capture) {
    vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
    capture->resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
    std::memcpy(capture->data(), readbackMemory_.mapped, capture->size());
  }
  cur_ = (cur_ + 1) % kFrames;
}

}  // namespace

// (Device::Create, gfx.cpp, calls it: on Windows when asked for, elsewhere always)
std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string* error) {
  auto d = std::make_unique<VulkanDevice>();
  if (!d->Init(desc, error)) return nullptr;
  return d;
}

}  // namespace render::gfx
