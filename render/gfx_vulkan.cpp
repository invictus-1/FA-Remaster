// The Vulkan backend of gfx.h: the same calls as gfx_d3d12.cpp, for systems other than Windows (Linux,
// macOS through MoltenVK) and tried on Windows against Direct3D 12, picture for picture. Chris, 2026-10-15:
// after the optimisation, Vulkan next; "I don't have linux so we just have to try and make it as good as
// possible same with mac os on m1". docs/render-vulkan.md has the plan and what is done.
//
// - The window's picture is drawn into an image of our own (RGBA8, with its depth, as Direct3D 12's back
//   buffer) and blitted onto the swap chain's image at the frame's end: pipelines need not know the swap
//   chain's format, and a capture reads our own image.
// - Bindings as Direct3D 12's root signature has them: set 0 holds t0-t43 (images, at 16-59), t0-t1 of
//   space1 (storage buffers, 60-61) and the six fixed samplers (64-69), one set for each combination used
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

#include "card_memory.h"  // (photofix2)
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
#include <unistd.h>  // [hitches] (getpid: the kept pipelines' file written under a name of its own first)
#if defined(__APPLE__)
#include <sys/sysctl.h>  // [vkpresent] (the computer's memory: hw.memsize)
#endif
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "gfx_latency.h"
#include "gfx_pacing.h"
#include "gfx_vulkan_dxc.h"
#include "hitch_log.h"  // [hitches]

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
  X(vkQueueWaitIdle)                    \
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
  X(vkCreatePipelineCache)              \
  X(vkDestroyPipelineCache)             \
  X(vkGetPipelineCacheData)             \
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
  X(vkCmdDrawIndexedIndirect)           \
  X(vkCmdExecuteCommands)               \
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
constexpr int kTexSlots = 44;
constexpr int kCbSlots = 4;
constexpr uint32_t kTexBinding = 16, kBufferBinding = 60, kSamplerBinding = 64, kStorageBinding = 80;
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);  // [spikes]
    for (Freeing& f : freeing_) f.thread.join();  // [warmtarget] (the large blocks given back on threads of their own: done)
    freeing_.clear();
    for (Block& b : blocks_)
      if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    blocks_.clear();
  }
  // the blocks taken of the card's own memory, in bytes
  uint64_t DeviceLocalBytes() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);  // [spikes]
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
  // [spikes] Get and Free under a lock of its own: a worker's PrepareTexture / PrepareBuffer (gfx.h) takes memory beside the
  // frames' thread
  // [vkpresent] `mayWait` (a thread other than the frames', its staging): where the blocks there are have no room, it waits
  // a while for what the upload batches let go of (Free) before it takes a new block. A new block of the processor's memory
  // (host-visible) had the system commit and clear it: 256 MB took 130-260 ms, and the frames' thread's present waited for
  // all of it (measured: a block taken on another thread, then written, held vkQueuePresentKHR as long; some allocations
  // alone did). The staging a map opening behind wants then comes out of blocks taken before the first frame (Reserve).
  bool Get(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool linear, Allocation* out, bool mayWait = false) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    const uint32_t type = Type(req.memoryTypeBits, want);
    if (type == UINT32_MAX) return false;
    const auto waitUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRoomWaitMs);
    for (;;) {
      if (Take(req, type, linear, out)) return true;
      if (!mayWait || req.size > kBlock || std::chrono::steady_clock::now() >= waitUntil) break;
      roomFreed_.wait_until(lock, waitUntil);
    }
    if (mayWait && req.size <= kBlock && !toldFull_) {  // (said once: the reserve too small for what was in flight)
      toldFull_ = true;
      fprintf(stderr, "render: Vulkan: no room in the staging's memory for %d ms; a block more taken (a frame may wait for it)\n", kRoomWaitMs);
    }
    return MakeBlock(req, type, linear, out, lock);
  }
  // [vkpresent] Before the first frame: `count` blocks of the memory `want` names for buffers of `memoryTypeBits` (the
  // staging's), written once so that the system commits them now and not while frames are shown. Kept for the program's life.
  void Reserve(uint32_t memoryTypeBits, VkMemoryPropertyFlags want, bool linear, int count) {
    const uint32_t type = Type(memoryTypeBits, want);
    if (type == UINT32_MAX) return;
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    for (int i = 0; i < count; ++i) {
      VkMemoryRequirements req{};
      req.size = kBlock;
      req.alignment = 256;
      req.memoryTypeBits = 1u << type;
      Allocation a;
      if (!MakeBlock(req, type, linear, &a, lock)) return;
      if (a.mapped) std::memset(a.mapped, 0, static_cast<size_t>(a.size));
      GiveBack(a, lock);
    }
  }

 private:
  static constexpr VkDeviceSize kBlock = 256ull << 20;  // (a block's usual size)
  static constexpr int kRoomWaitMs = 2000;  // [vkpresent] (Get's longest wait for room, then a new block)
  // A piece of a block of `type` for `req` (with the lock held).
  bool Take(const VkMemoryRequirements& req, uint32_t type, bool linear, Allocation* out) {
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
    return false;
  }
  // A new block (256 MB, or the size asked for when larger), and `req`'s piece of it (`lock` held on entry and return).
  bool MakeBlock(const VkMemoryRequirements& req, uint32_t type, bool linear, Allocation* out, std::unique_lock<std::recursive_mutex>& lock) {
    Block b;
    b.type = type;
    b.linear = linear;
    b.size = std::max<VkDeviceSize>(kBlock, (req.size + 0xffff) & ~VkDeviceSize{0xffff});
    // [warmtarget] (the driver's allocation outside the lock: a block takes it tens of milliseconds, which the frames' thread's
    // Free and Get waited for while a map opening behind took memory on its own thread)
    lock.unlock();
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = b.size;
    mai.memoryTypeIndex = type;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (linear && deviceAddresses_) mai.pNext = &flags;
    if (vkAllocateMemory(device_, &mai, nullptr, &b.memory) != VK_SUCCESS) {
      lock.lock();
      return false;
    }
    if (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      void* p = nullptr;
      vkMapMemory(device_, b.memory, 0, b.size, 0, &p);
      b.mapped = static_cast<uint8_t*>(p);
    }
    b.free.push_back({0, b.size});
    lock.lock();
    // (photofix2: in the place of one given back, if there is one)
    const auto empty = std::find_if(blocks_.begin(), blocks_.end(), [](const Block& e) { return !e.memory; });
    if (empty != blocks_.end()) *empty = std::move(b);
    else blocks_.push_back(std::move(b));
    return Take(req, type, linear, out);
  }

 public:
  void Free(const Allocation& a) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);  // [spikes]
    GiveBack(a, lock);
  }

 private:
  // (Free's, with the lock held)
  void GiveBack(const Allocation& a, std::unique_lock<std::recursive_mutex>&) {
    if (a.block < 0 || static_cast<size_t>(a.block) >= blocks_.size()) return;
    roomFreed_.notify_all();  // [vkpresent] (a Get waiting for room looks again)
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
    // (photofix2) a block larger than the usual, made for one large picture (a photo's, the lens's at a photo's size), is
    // given back to the driver once all of it is free again: else a photo's pictures stayed taken of the card for as long
    // as the program ran. (Freed only once the card is done with what was in it: Collect, FreeImage.)
    if (b.size > (256ull << 20) && b.free.size() == 1 && b.free[0].first == 0 && b.free[0].second == b.size) {
      // [warmtarget] (given back on a thread of its own: the driver took 25-30 ms for a large one, on the frames' thread when
      // it was a map opening behind's staging, let go of with its upload batch)
      for (size_t i = 0; i < freeing_.size();) {  // (those done, joined)
        if (freeing_[i].done->load(std::memory_order_acquire)) {
          freeing_[i].thread.join();
          freeing_.erase(freeing_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
          ++i;
        }
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      freeing_.push_back({std::thread([device = device_, memory = b.memory, done] {
                            vkFreeMemory(device, memory, nullptr);
                            done->store(true, std::memory_order_release);
                          }),
                          done});
      b = Block{};
    }
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
  mutable std::recursive_mutex mutex_;  // [spikes]
  std::condition_variable_any roomFreed_;  // [vkpresent] (Free: a Get waiting for room looks again)
  bool toldFull_ = false;
  struct Freeing {  // [warmtarget] (a large block being given back; under mutex_)
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Freeing> freeing_;
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

struct BackgroundPipeline;
struct PipelineRec {
  bool used = false;
  bool compute = false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const void* layout = nullptr;  // VulkanDevice::Layout
  bool depth = true;  // drawn with a depth attachment (Vulkan wants the pass to match)
  std::shared_ptr<PendingPipeline> pending;  // (asked for in a batch, not made yet)
  std::shared_ptr<BackgroundPipeline> background;  // [snowhitch] (being made on the device's own thread, not taken yet)
};

// [snowhitch] A pipeline asked for between BeginBackgroundPipelines and EndBackgroundPipelines (as gfx_d3d12.cpp's): what it is
// made from, how far its making is (under the device's backgroundMutex_), and what was made.
struct BackgroundPipeline {
  enum Stage { Queued, Making, Made, NotMade };
  std::shared_ptr<PendingPipeline> what;
  Stage stage = Queued;
  PipelineRec made;
  std::string why;
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
  // [cardcost] a scene built to be refitted (RefitScene): over how many instances (refitted only over as many); -1 not
  uint64_t refitCount = ~uint64_t{0};
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
  std::unique_ptr<Prepared> PrepareTarget(const TargetDesc& desc) override;  // [warmtarget]
  Texture FinishTarget(std::unique_ptr<Prepared> prepared) override;
  bool MakeTarget(const TargetDesc& desc, TextureRec* out);  // (any thread)
  Texture AddTarget(TextureRec rec);                         // (the frames' thread)
  void DropTarget(TextureRec& rec);                          // (one made and never added)
  Pipeline CreatePipeline(const PipelineDesc& desc, std::string* error) override;
  Pipeline CreateCompute(const ComputeDesc& desc, std::string* error) override;
  void BeginPipelineBatch() override;
  bool EndPipelineBatch(std::string* error) override;
  void BeginBackgroundPipelines() override { backgroundAsked_ = !shaderCache_.empty(); }  // [snowhitch]
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
  void DropPrepared(TextureRec* texture, BufferRec* buffer, VkBuffer staging, const Allocation& stagingMemory);  // (one never finished)
  void Destroy(Buffer buffer) override;
  void Destroy(Texture texture) override;
  void Destroy(Pipeline pipeline) override;
  bool RayTracing() const override { return rayTracing_; }
  Structure CreateGeometry(const GeometryPart* parts, int count) override;
  void BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) override;
  bool RefitScene(Structure* scene, const SceneInstance* instances, uint32_t count, bool rebuild) override;  // [cardcost]
  bool MakeScene(Structure* scene, const SceneInstance* instances, uint32_t count, int how);                 // [cardcost]
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
  // [gpuscenery] card buffers and indirect draws (gfx.h)
  bool IndirectDraws() const override { return true; }
  void UpdateBufferRange(Buffer buffer, uint32_t offset, const void* data, uint32_t size) override;
  void DrawIndexedIndirect(Buffer arguments, uint32_t offset) override;
  void EndFrame(std::vector<uint8_t>* capture, bool present) override;
  void Mark(const char* name) override;
  uint64_t MemoryInUse() override { return memory_.DeviceLocalBytes(); }
  bool MemoryBudget(uint64_t* budget, uint64_t* used) override;  // (photofix2)
  void TakeTimes(std::vector<std::pair<const char*, double>>* out) override { *out = lastTimes_; }
  bool SetFrameGeneration(std::function<bool()> make) override;
  bool FrameGenerating() const override { return generating_; }
  Texture WindowPicture() override { return generating_ ? windowPictures_[cur_] : Texture{}; }
  Texture BetweenPicture() override { return generating_ ? betweenPictures_[cur_] : Texture{}; }
  bool SetPhotoWindow(int width, int height) override;
  bool CapturesLater() const override { return true; }  // [weather] (a photo window's frame: the minimap's ground)
  void CaptureLater(bool later) override { captureLater_ = later; }
  int TakeCapture(std::vector<uint8_t>* capture) override;
  bool SetLatencyLimiter(bool on) override;
  bool LatencyLimiter() const override { return latency_; }
  void WaitForNextFrame() override;
  // [mcrec] recording on several threads (gfx.h)
  int ParallelRecorders() const override { return kMostRecorders; }
  bool BeginParallel(int count) override;
  Recorder* Parallel(int index) override;
  void EndParallel() override;

 private:
  // (photomode) SetPhotoWindow: the picture and the depth a frame draws its window into while it is on, their size; a
  // photo frame's end (read back, never shown)
  Texture photoColor_, photoDepth_;
  int photoWidth_ = 0, photoHeight_ = 0;
  bool photoOn_ = false;
  bool PhotoNow() { return photoOn_ && Find(photoColor_) && Find(photoDepth_); }
  void EndFramePhoto(std::vector<uint8_t>* capture, bool later = false);
  // the latency limiter (gfx.h, SetLatencyLimiter; gfx_latency.h; as Direct3D 12's, gfx_d3d12.cpp, with the driver's
  // present wait in place of the swap chain's frame latency object): the switch (to wantLatency_; the chain stays as
  // it is); the frames' work taken from the slots the card has finished; the wait for a frame to begin (with the
  // just-in-time wait or without), and with frame generation the wait for the presenter; the screen's refresh; the
  // chain's image for the coming frame acquired (before the input read, with the limiter on); whether
  // presents carry ids (the present wait's, for the limiter or the measure); the measure's watcher thread, which waits
  // for each picture's showing and notes when it came
  void SwitchLatency();
  void TakeWork();
  double CardFree() const;
  void LimitFrames(bool justInTime);
  void WaitForPresenter();
  double Refresh();
  struct Frame;
  void AcquireImage(Frame& f);
  bool PresentIds() const { return presentWait_ && (latency_ || measureLatency); }
  void Watch(uint64_t id, double readAt);
  void Watcher();
  struct Image {  // the window's picture and its depth (not texture records)
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  bool CreateSwapchain(std::string* error);
  void DestroySwapchain();
  // frame generation (gfx.h; as Direct3D 12's, gfx_d3d12.cpp, but the frame's own commands copy the two pictures
  // into images of the swap chain, and the presenter only shows them, from its queue): the switch (to
  // wantGenerating_), each slot's window picture and the one made between, the presenter thread, its start, its
  // end (after what was handed to it), waiting until it has shown all it was handed, or a slot's two; an image of
  // the chain acquired without waiting long; one shown (the time just before in `at`); a frame's end with it on;
  // the read-back buffer made large enough for the window
  bool SwitchFrameGeneration();
  bool MakeFramePictures();
  void DropFramePictures();
  void StartPresenter();
  void StopPresenter();
  void DrainPresenter();
  void WaitPresented(int slot);
  void Presenter();
  bool Acquire(VkSemaphore semaphore, uint32_t* image);
  void Show(uint32_t image, double* at, double readAt = 0);
  void EndFrameGenerating(std::vector<uint8_t>* capture, bool present);
  bool EnsureReadback(VkDeviceSize need = 0);  // (0: the window's size)
  bool CreateWindowImages(std::string* error);
  bool NextConstantPage();
  bool MakeImage(Image* image, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
  void FreeImage(Image* image);
  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect);
  void Transition(TextureRec* t, VkImageLayout to);
  void EndRendering();
  void StartRendering(bool withDepth, bool secondary = false);  // ([mcrec] secondary: for recorders' command buffers alone)
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
  // [hitches] The driver's pipelines kept from run to run (pipelines-vulkan.bin beside the compiled shaders): measured on an
  // RTX 4090 (2026-10-20), a large pipeline took 2.5 to 3.5 s the first time a new build of the program made it (the
  // driver's own cache is kept by program) and 3 ms after; kept here, a new build finds them made. The file is the
  // driver's (its header names the card and the driver's cache: another's is not given to it). Pictures are not changed.
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::atomic<bool> pipelineCacheGrew_{false};  // ([snowhitch] also set on the device's own thread)
  void OpenPipelineCache();
  void SavePipelineCache();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props_{};
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  Memory memory_;
  // [vkpresent] the thread the device was made on (the frames'), and the staging's blocks taken before the first frame (4 of 256
  // MB, taken and written in about 85 ms: a SETONS_R opened behind had at most 521 MB of staging in flight, its 4K ground layers
  // 363 MB at once, with RecycleUploads; before, a first load took three or four such blocks while frames were shown and kept them)
  std::thread::id framesThread_;
  // [vkpresent] (the engine, 2026-10-24: a sixteenth of the computer's memory at most, so that 8 and 16 GB machines - Linux,
  // a Mac's shared memory - keep theirs: 8 GB 512 MB, 16 GB and more 1 GB; a load with less waits for room a little longer)
  static int StagingReserveBlocks();
  void RecycleUploads();
  bool nullDescriptors_ = false;
  // (photofix2) VK_EXT_memory_budget, where the driver has it: the card's budget for this program (MemoryBudget)
  bool memoryBudget_ = false;
  PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2_ = nullptr;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> swapImages_;
  std::vector<VkImageLayout> swapLayouts_;
  std::vector<VkSemaphore> rendered_;  // per swap chain image: the frame drawn, for its showing to wait on
  bool imageHeld_ = false;             // an image of the chain acquired and not yet shown
  std::atomic<bool> chainStale_{false};  // the chain no longer fits the window: made again before the next frame
  VkExtent2D swapExtent_{};
  Image back_, depth_;
  // frame generation (SetFrameGeneration): the presenter's queue (a second one of the drawing family, or one of
  // another family that can show in the window; none: frame generation is not offered), whether it is on (and
  // asked for), the chain made for it (more images; shared with the presenter's family), the function at a
  // frame's end, each slot's two pictures, and the presenter's thread, jobs and pacing
  uint32_t presentFamily_ = UINT32_MAX;
  VkQueue presentQueue_ = VK_NULL_HANDLE;
  bool generating_ = false, wantGenerating_ = false, chainForFrames_ = false;
  std::function<bool()> makeBetween_;
  Texture windowPictures_[kFrames], betweenPictures_[kFrames];
  struct PresentJob {
    int slot = 0;
    VkFence fence = VK_NULL_HANDLE;  // the frame's work (its copies into the chain's images included) done
    int64_t between = -1, frame = -1;  // the chain's images to show (-1: none)
    double readAt = 0;                 // (the measure) when the frame's input was read
  };
  std::thread presenter_;
  std::mutex presentMutex_;  // (over the jobs, presentStop_ and slotShowing_)
  std::mutex swapMutex_;     // (the chain's acquiring and showing, from two threads: Vulkan wants them apart)
  std::condition_variable presentWake_, presentDone_;
  std::deque<PresentJob> presentJobs_;
  bool presentStop_ = false;
  bool slotShowing_[kFrames] = {};  // handed to the presenter and not shown yet
  Pacing pacing_;                   // (the presenter's)
  double lastShown_ = 0;            // (the presenter's)

  // the latency limiter (SetLatencyLimiter): asked for, and on; whether the device has the present wait
  // (VK_KHR_present_id and VK_KHR_present_wait, enabled where offered: nothing changes until a present names an id)
  // and its function; the ids presents are given (rising, over every chain), the last one given (the presenter's
  // while frame generation is on) and the first of the chain made last; the last input read (WaitForNextFrame; 0:
  // none since the last frame began); whether WaitForNextFrame acquired the coming frame's image; each slot's frame:
  // its input read, its hand-over, whether the card's time was taken (two timestamps a slot, written only while the
  // limiter is on); the limiter, its wait, and the showings the main thread waited for
  bool wantLatency_ = false, latency_ = false, presentWait_ = false;
  PFN_vkWaitForPresentKHR vkWaitForPresent_ = nullptr;
  std::atomic<uint64_t> presentIdNext_{1}, lastPresentId_{0};
  uint64_t chainFirstId_ = 1;
  double readAt_ = 0, nextDeadline_ = 0;
  bool earlyAcquired_ = false, latencyStalled_ = false, nextPaced_ = false;
  struct LatencySlot {
    double read = 0, handed = 0, deadline = 0;
    bool paced = false, timed = false;
    uint64_t order = 0;  // (the frames' count at its hand-over: the order they were handed over in)
  };
  uint64_t handedCount_ = 0;
  // the card's clock in the processor's (the limiter's: when a frame's work began and ended on the card, by Now()'s
  // clock): calibrated timestamps, where the device offers them with the clock Now() reads (0: none)
  PFN_vkGetCalibratedTimestampsKHR vkGetCalibratedTimestamps_ = nullptr;
  VkTimeDomainKHR hostDomain_ = VK_TIME_DOMAIN_DEVICE_KHR;
  LatencySlot latencySlots_[kFrames];
  VkQueryPool latencyQueries_ = VK_NULL_HANDLE;
  gfx::LatencyLimiter limiter_;
  Waiter latencyWaiter_;
  ShowingTimes shown_;
  double osRefresh_ = 0, osRefreshAt_ = -1;  // (Refresh: the screen's, from the system, and when it was asked)
  // the measure (measureLatency): the watcher thread, the ids it is to wait for (with their input reads), and the
  // record; watchChain_ is held while it waits on the chain, and by the chain's destruction
  std::thread watcher_;
  std::mutex watchMutex_, watchChain_;
  std::condition_variable watchWake_;
  std::deque<std::pair<uint64_t, double>> watchIds_;
  bool watchStop_ = false;
  LatencyRecord latencyRecord_;
  LatencyCost cost_;  // (the measure's: what the limiter's own work and waits took)

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

  // [mcrec] A recorder's room in one frame slot (Device::BeginParallel; made the first time it is wanted, kept, and reset
  // with the slot): its secondary command buffers (one each parallel section), its descriptor pools (another made when
  // one is full), its sets by what they hold, and its pages of constants, each with its set, as the frame's own.
  struct RecorderRoom {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    size_t cmdsUsed = 0;
    std::vector<VkDescriptorPool> descriptors;
    size_t descriptorsUsed = 0;
    std::unordered_map<std::string, VkDescriptorSet> sets;
    std::vector<Page> constantPages;
    std::vector<VkDescriptorSet> constantSets;
    size_t constantPage = SIZE_MAX;
    VkDeviceSize constantsUsed = 0;
  };
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore acquired2 = VK_NULL_HANDLE;  // (frame generation: the image the picture made between goes into)
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
    std::vector<RecorderRoom> recorders;  // [mcrec] (by the recorder's number)
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
  // [mcrec] Recording on several threads (gfx.h, BeginParallel): each recorder records into a secondary command buffer of its
  // own, which EndParallel runs from the frame's own in their order, in a drawing into the pass's attachments of its own
  // (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT), between the frame's own drawings before and after it. What the
  // frame's own command buffer has bound of the vertex and index buffers (bound again after a section, and on each
  // recorder's).
  static constexpr int kMostRecorders = 16;
  static constexpr VkDeviceSize kRecorderConstantPage = 2u << 20;
  class ParallelRecorder;
  std::vector<std::unique_ptr<ParallelRecorder>> recorders_;
  int parallel_ = 0;            // recorders handed out by BeginParallel (0: none)
  bool parallelDepth_ = false;  // (the section's drawing has the pass's depth)
  std::mutex parallelMutex_;    // (a pipeline not made yet that a recorder sets: made under it)
  std::atomic<bool> warnedParallel_{false};
  struct VertexBound {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
  } vertexBound_;
  struct IndexBound {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkIndexType type = VK_INDEX_TYPE_UINT16;
  } indexBound_;
  // a draws' set of what is bound (by `textures`, `buffers` and the layout), made from `allocate` the first time (`sets`
  // keeps them by what they hold): the frame's own, or a recorder's
  VkDescriptorSet MakeDrawSet(const Layout* layout, const uint32_t* textures, const BoundBuffer* buffers,
                              const std::function<VkDescriptorSet(VkDescriptorSetLayout)>& allocate,
                              std::unordered_map<std::string, VkDescriptorSet>& sets);
  VkDescriptorSet RecorderSet(RecorderRoom& room, VkDescriptorSetLayout layout);
  bool NextRecorderPage(RecorderRoom& room, ParallelRecorder& r);
  void RecorderConstants(RecorderRoom& room, ParallelRecorder& r, int slot, const void* data, uint32_t size);
  bool RecorderPrepare(ParallelRecorder& r);
  // compute: the pipeline set last and its layout, the storage targets bound (texture, level), those in
  // the general layout, and whether the last thing recorded was a dispatch (the next waits for its writes)
  VkPipeline computePipeline_ = VK_NULL_HANDLE;
  const Layout* computeLayout_ = nullptr;
  std::pair<uint32_t, int> boundStorage_[kStorageSlots] = {};
  std::vector<uint32_t> general_;
  bool dispatched_ = false;
  // [gpuscenery] the card buffers made (ids), and whether a dispatch since the last pass began may have written one (the
  // next pass's draws then wait for it); a barrier over all memory, from `from` to `to` (stages and accesses)
  std::vector<uint32_t> cardBuffers_;
  bool cardWritten_ = false;
  bool CardBound(int slot) const;
  void CardBarrier(VkPipelineStageFlags2 from, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 to, VkAccessFlags2 toAccess);

  std::vector<BufferRec> buffers_;     // id = index + 1
  std::vector<TextureRec> textures_;   // id = index; 0 is none
  std::vector<PipelineRec> pipelines_; // id = index + 1
  bool batching_ = false;   // pipelines asked for are made at EndPipelineBatch
  std::string batchError_;  // (the first that could not be made, in a batch)
  bool MakePipeline(const PipelineDesc& desc, PipelineRec* out, std::string* error, bool background = false);
  bool MakeCompute(const ComputeDesc& desc, PipelineRec* out, std::string* error, bool background = false);
  Pipeline Keep(const PipelineRec& rec);
  bool Realize(uint32_t index, std::string* error);
  // [snowhitch] pipelines made on a thread of the device's own (BeginBackgroundPipelines; as gfx_d3d12.cpp's): MakePipeline and
  // MakeCompute there with `background` (their shaders from the cache alone, compiled into it first with a compiler of that
  // thread's own); the pipeline layouts are shared under layoutsMutex_
  bool backgroundAsked_ = false;
  std::thread backgroundThread_;
  std::mutex backgroundMutex_, layoutsMutex_;
  std::condition_variable backgroundWake_, backgroundDone_;
  std::deque<std::shared_ptr<BackgroundPipeline>> backgroundQueue_;
  int backgroundMaking_ = 0;  // [bgload] (the one being made now: 1)
  bool backgroundStop_ = false;
  std::shared_ptr<BackgroundPipeline> AskBackground(std::shared_ptr<PendingPipeline> what);
  void BackgroundWork();
  void TakeBackground(uint32_t index);
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
  // [weather] (CaptureLater) the next capture of a photo window's frame left on the card; the one copied out there and not
  // yet taken: its frame's slot and number, its size
  bool captureLater_ = false, laterWaiting_ = false;
  int laterSlot_ = 0;
  uint64_t laterNumber_ = 0;
  VkDeviceSize laterBytes_ = 0;
  Allocation readbackMemory_;
  VkDeviceSize readbackSize_ = 0;
};

// [mcrec] Recording on several threads (gfx.h, BeginParallel). A recorder: the device's draw calls into a secondary command
// buffer of its own, with what they bind kept as the device keeps its own (boundTextures_, boundBuffers_, pipeline_,
// layout_, constantOffsets_, constants_), its sets and constants in its own room of the frame slot (RecorderRoom).
class VulkanDevice::ParallelRecorder final : public Recorder {
 public:
  ParallelRecorder(VulkanDevice* device, int index) : d_(device), index_(index) {}
  void SetPipeline(Pipeline pipeline) override {
    VkPipeline made = VK_NULL_HANDLE;
    const void* layout = nullptr;
    bool depth = true;
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
      if (!rec || !rec->pipeline || rec->compute) return;
      made = rec->pipeline;
      layout = rec->layout;
      depth = rec->depth;
    }
    pipeline_ = made;
    pipelineDepth_ = depth;
    if (layout != layout_) {
      layout_ = static_cast<const Layout*>(layout);
      setDirty_ = true;
    }
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, made);
  }
  void SetConstants(int slot, const void* data, uint32_t size) override {
    d_->RecorderConstants(d_->frames_[d_->cur_].recorders[static_cast<size_t>(index_)], *this, slot, data, size);
  }
  void SetTexture(int slot, Texture texture) override {
    if (slot < 0 || slot >= kTexSlots) return;
    const uint32_t id = d_->Find(texture) ? texture.id : 0;
    if (boundTextures_[slot] == id) return;
    boundTextures_[slot] = id;
    setDirty_ = true;
  }
  void SetVertexBuffer(Buffer buffer) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || rec->kind != BufferKind::Vertex || (rec->dynamic && rec->frame != d_->frameNumber_)) return;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &rec->buffer, &rec->offset);
    vertexBound_ = {rec->buffer, rec->offset};
  }
  void SetIndexBuffer(Buffer buffer) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != d_->frameNumber_)) return;
    const VkIndexType type = rec->stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(cmd_, rec->buffer, rec->offset, type);
    indexBound_ = {rec->buffer, rec->offset, type};
  }
  void SetShaderBuffer(Buffer buffer, int slot) override {
    const BufferRec* rec = d_->Find(buffer);
    if (!rec || slot < 0 || slot > 1) return;
    if (rec->dynamic) {
      if (rec->kind != BufferKind::Shader || rec->frame != d_->frameNumber_) return;
      boundBuffers_[slot] = {rec->buffer, rec->offset, rec->size};
    } else {
      if (!rec->buffer) return;
      boundBuffers_[slot] = {rec->buffer, 0, (static_cast<VkDeviceSize>(rec->size) + 3) & ~VkDeviceSize{3}};
    }
    setDirty_ = true;
  }
  void Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) override {
    if (!d_->RecorderPrepare(*this)) return;
    ++draws_;
    vkCmdDraw(cmd_, vertexCount, instances, firstVertex, 0);
  }
  void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override {
    if (!d_->RecorderPrepare(*this)) return;
    ++draws_;
    vkCmdDrawIndexed(cmd_, indexCount, instances, firstIndex, baseVertex, 0);
  }
  void DrawIndexedIndirect(Buffer arguments, uint32_t offset) override {  // [gpuscenery]
    const BufferRec* rec = d_->Find(arguments);
    if (!rec || rec->kind != BufferKind::Card || (offset & 3u) || static_cast<uint64_t>(offset) + 20 > rec->size) return;
    if (!d_->RecorderPrepare(*this)) return;
    ++draws_;
    vkCmdDrawIndexedIndirect(cmd_, rec->buffer, offset, 1, 20);
  }

  VulkanDevice* d_;
  int index_;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  uint32_t boundTextures_[kTexSlots] = {};
  BoundBuffer boundBuffers_[2];
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  const Layout* layout_ = nullptr;
  bool pipelineDepth_ = true;
  uint32_t constantOffsets_[kCbSlots] = {};
  std::vector<uint8_t> constants_[kCbSlots];
  bool setDirty_ = true;
  VertexBound vertexBound_;
  IndexBound indexBound_;
  int draws_ = 0;
};

VulkanDevice::~VulkanDevice() {
  {  // [snowhitch] the thread making pipelines ahead: what it is making finished, the rest left
    std::lock_guard<std::mutex> lock(backgroundMutex_);
    backgroundStop_ = true;
  }
  backgroundWake_.notify_all();
  if (backgroundThread_.joinable()) backgroundThread_.join();
  if (!device_) {
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    return;
  }
  if (generating_) StopPresenter();
  vkDeviceWaitIdle(device_);
  earlyAcquired_ = false;  // (its semaphore goes with the frames' below)
  if (watcher_.joinable()) {  // (the measure's watcher: what it still waits for shown or given up)
    {
      std::lock_guard<std::mutex> lock(watchMutex_);
      watchStop_ = true;
    }
    watchWake_.notify_all();
    watcher_.join();
  }
  if (measureLatency) {
    const double refresh = Refresh();
    latencyRecord_.Print(!presentWait_ ? "Vulkan (no present wait: not measured)" : latency_ ? "Vulkan (low latency)" : "Vulkan", vsync_ ? refresh : 0, refresh);
    if (latency_) limiter_.Print(presentWait_ ? "Vulkan (present wait)" : "Vulkan (no present wait: the frames on the card kept to two)"), cost_.Print("Vulkan");
  }
  if (latencyQueries_) vkDestroyQueryPool(device_, latencyQueries_, nullptr);
  for (Frame& f : frames_) {
    Collect(f.garbage);
    Collect(f.later);
    for (Page& p : f.pages) vkDestroyBuffer(device_, p.buffer, nullptr);
    for (Page& p : f.constantPages) vkDestroyBuffer(device_, p.buffer, nullptr);
    for (RecorderRoom& r : f.recorders) {  // [mcrec]
      for (Page& p : r.constantPages) vkDestroyBuffer(device_, p.buffer, nullptr);
      for (VkDescriptorPool p : r.descriptors) vkDestroyDescriptorPool(device_, p, nullptr);
      if (r.pool) vkDestroyCommandPool(device_, r.pool, nullptr);
    }
    if (f.descriptors) vkDestroyDescriptorPool(device_, f.descriptors, nullptr);
    if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    if (f.done) vkDestroyFence(device_, f.done, nullptr);
    if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
    if (f.acquired2) vkDestroySemaphore(device_, f.acquired2, nullptr);
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
  for (PipelineRec& p : pipelines_) {
    if (p.used && p.pipeline) vkDestroyPipeline(device_, p.pipeline, nullptr);
    if (p.used && p.background && p.background->made.pipeline) vkDestroyPipeline(device_, p.background->made.pipeline, nullptr);  // [snowhitch]
  }
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
  if (pipelineCache_) {  // [hitches] (the pipelines made since the map opened, for the next run)
    SavePipelineCache();
    vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
  }
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
  // the present wait (the latency limiter's, and its measure's): presents with ids, and waiting until one is shown
  presentWait_ = deviceOffers(physical_, VK_KHR_PRESENT_ID_EXTENSION_NAME) && deviceOffers(physical_, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  VkPhysicalDevicePresentIdFeaturesKHR havePresentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR havePresentWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  if (presentWait_) {
    havePresentWait.pNext = have.pNext;
    havePresentId.pNext = &havePresentWait;
    have.pNext = &havePresentId;
  }
  // calibrated timestamps (the latency limiter's): the card's clock read together with the processor's, in the
  // domain Now()'s clock is (Windows: the performance counter; Linux: CLOCK_MONOTONIC; none on macOS here)
  const bool calibrationKhr = deviceOffers(physical_, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
  bool calibration = calibrationKhr || deviceOffers(physical_, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#if defined(_WIN32)
  hostDomain_ = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#elif defined(__linux__)
  hostDomain_ = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#else
  calibration = false;
#endif
  if (calibration) {
    auto domainsOf = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
        vkGetInstanceProcAddr(instance_, calibrationKhr ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR" : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    uint32_t n = 0;
    std::vector<VkTimeDomainKHR> domains;
    if (domainsOf && domainsOf(physical_, &n, nullptr) == VK_SUCCESS) {
      domains.resize(n);
      if (domainsOf(physical_, &n, domains.data()) != VK_SUCCESS) domains.clear();
    }
    calibration = std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_DEVICE_KHR) != domains.end() &&
                  std::find(domains.begin(), domains.end(), hostDomain_) != domains.end();
  }
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
  presentWait_ = presentWait_ && havePresentId.presentId && havePresentWait.presentWait;
  if (!have.features.textureCompressionBC) fprintf(stderr, "render: Vulkan: this card reads no BC textures (most of the game's)\n");
  // frame generation's presenter (SetFrameGeneration) shows from a queue of its own: a second one of the drawing
  // family where it has two, else one of another family that can show in this window; with neither, frame
  // generation is not offered
  {
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, fam.data());
    if (fam[queueFamily_].queueCount >= 2) presentFamily_ = queueFamily_;
    for (uint32_t i = 0; i < families && presentFamily_ == UINT32_MAX; ++i) {
      VkBool32 can = VK_FALSE;
      if (i != queueFamily_ && fam[i].queueCount) vkGetPhysicalDeviceSurfaceSupportKHR(physical_, i, surface_, &can);
      if (can) presentFamily_ = i;
    }
  }
  const float priorities[2] = {1, 1};
  VkDeviceQueueCreateInfo qcis[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
  qcis[0].queueFamilyIndex = queueFamily_;
  qcis[0].queueCount = presentFamily_ == queueFamily_ ? 2 : 1;
  qcis[0].pQueuePriorities = priorities;
  qcis[1].queueFamilyIndex = presentFamily_;
  qcis[1].queueCount = 1;
  qcis[1].pQueuePriorities = priorities;
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
  VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  if (calibration) deviceExtensions.push_back(calibrationKhr ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
  // (photofix2) the card's memory budget (asked for a photo's plan only; it changes nothing drawn)
  memoryProperties2_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceMemoryProperties2"));
  memoryBudget_ = memoryProperties2_ && deviceOffers(physical_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (memoryBudget_) deviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (presentWait_) {  // (ahead of the rest of the chain)
    deviceExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    presentId.presentId = VK_TRUE;
    presentWaitFeature.presentWait = VK_TRUE;
    presentWaitFeature.pNext = features.pNext;
    presentId.pNext = &presentWaitFeature;
    features.pNext = &presentId;
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
  dci.queueCreateInfoCount = presentFamily_ != UINT32_MAX && presentFamily_ != queueFamily_ ? 2 : 1;
  dci.pQueueCreateInfos = qcis;
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
  OpenPipelineCache();  // [hitches]
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
  if (presentWait_) vkWaitForPresent_ = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR"));
  presentWait_ = presentWait_ && vkWaitForPresent_;
  if (calibration)
    vkGetCalibratedTimestamps_ = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
        vkGetDeviceProcAddr(device_, calibrationKhr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT"));
  if (rayTracing_ && vkGetPhysicalDeviceProperties2) {  // (where a build's scratch room may start)
    VkPhysicalDeviceAccelerationStructurePropertiesKHR sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sp;
    vkGetPhysicalDeviceProperties2(physical_, &p2);
    scratchAlign_ = std::max<VkDeviceSize>(sp.minAccelerationStructureScratchOffsetAlignment, 1);
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  if (presentFamily_ != UINT32_MAX) vkGetDeviceQueue(device_, presentFamily_, presentFamily_ == queueFamily_ ? 1 : 0, &presentQueue_);
  memory_.Init(device_, memoryProps, rayTracing_);
  framesThread_ = std::this_thread::get_id();  // [vkpresent]
  {  // [vkpresent] the staging's memory taken and committed now, before the first frame (Memory::Get, `mayWait`)
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 16;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer probe = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bci, nullptr, &probe) == VK_SUCCESS) {
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device_, probe, &req);
      vkDestroyBuffer(device_, probe, nullptr);
      memory_.Reserve(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, StagingReserveBlocks());
    }
  }

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
    if ((r = vkCreateSemaphore(device_, &smi, nullptr, &f.acquired2)) != VK_SUCCESS) return fail("a semaphore", r);
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
  sci.minImageCount = std::max(caps.minImageCount, chainForFrames_ ? 5u : 3u);  // (frame generation: two a frame)
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent;
  swapExtent_ = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  const uint32_t sharing[2] = {queueFamily_, presentFamily_};
  if (chainForFrames_ && presentFamily_ != queueFamily_) {  // (drawn into by one family, shown from another)
    sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    sci.queueFamilyIndexCount = 2;
    sci.pQueueFamilyIndices = sharing;
  }
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  chainFirstId_ = presentIdNext_.load();  // (the present wait: ids before it were another chain's)
  VkResult r;
  {
    std::unique_lock<std::mutex> chain(watchChain_, std::defer_lock);  // (the measure's watcher reads the handle)
    if (watcher_.joinable()) chain.lock();
    r = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_);
  }
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
  if (earlyAcquired_) {  // (the latency limiter acquired an image for the frame to come: its semaphore, signalled and
                         // never waited for, made anew; the card is idle here)
    Frame& f = frames_[cur_];
    vkDestroySemaphore(device_, f.acquired, nullptr);
    VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &smi, nullptr, &f.acquired);
    earlyAcquired_ = waitAcquired_ = false;
  }
  if (watcher_.joinable()) {  // (the measure's watcher: out of its wait on this chain, what it waits for given up)
    std::lock_guard<std::mutex> chain(watchChain_);
    std::lock_guard<std::mutex> lock(watchMutex_);
    watchIds_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  shown_.Reset();
  for (VkSemaphore sem : rendered_)
    if (sem) vkDestroySemaphore(device_, sem, nullptr);
  rendered_.clear();
  swapImages_.clear();
  swapLayouts_.clear();
}

void VulkanDevice::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  if (generating_) DrainPresenter();
  FlushUploads();
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  FreeImage(&back_);
  FreeImage(&depth_);
  width_ = width;
  height_ = height;
  std::string error;
  if (!CreateWindowImages(&error) || !CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
  if (generating_) {  // (the slots' pictures at the new size; none: frame generation off)
    DropFramePictures();
    if (!MakeFramePictures()) {
      fprintf(stderr, "render: Vulkan: frame generation's pictures could not be made; it is off\n");
      wantGenerating_ = false;
      SwitchFrameGeneration();
    }
  }
}

// The function at a frame's end replaced at once (a world that goes away is never called after); the slots'
// pictures and the chain switched now when between frames, else at the next BeginFrame.
bool VulkanDevice::SetFrameGeneration(std::function<bool()> make) {
  wantGenerating_ = static_cast<bool>(make);
  makeBetween_ = std::move(make);
  if (wantGenerating_ && !presentQueue_) {  // (no queue to show from: not offered)
    wantGenerating_ = false;
    makeBetween_ = {};
    return false;
  }
  if (inFrame_ || wantGenerating_ == generating_) return true;
  return SwitchFrameGeneration();
}

bool VulkanDevice::SwitchFrameGeneration() {
  const bool on = wantGenerating_;
  if (generating_) StopPresenter();
  FlushUploads();
  vkDeviceWaitIdle(device_);
  DropFramePictures();
  if (on && !MakeFramePictures()) {
    fprintf(stderr, "render: Vulkan: frame generation could not be switched on\n");
    DropFramePictures();
    wantGenerating_ = false;
    if (!generating_) return false;
  }
  generating_ = chainForFrames_ = wantGenerating_;
  DestroySwapchain();
  std::string error;
  if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
  if (generating_) {
    pacing_.Reset();
    lastShown_ = 0;
    StartPresenter();
  }
  return generating_ == on;
}

// The window's picture and the one made between, for each frame slot (the window's size, RGBA8).
bool VulkanDevice::MakeFramePictures() {
  for (int i = 0; i < kFrames; ++i) {
    TargetDesc td;
    td.width = width_;
    td.height = height_;
    td.format = Format::RGBA8;
    windowPictures_[i] = CreateTarget(td);
    td.storage = true;
    betweenPictures_[i] = CreateTarget(td);
    if (!windowPictures_[i] || !betweenPictures_[i]) return false;
  }
  return true;
}

void VulkanDevice::DropFramePictures() {
  for (int i = 0; i < kFrames; ++i) {
    Destroy(windowPictures_[i]);
    Destroy(betweenPictures_[i]);
    windowPictures_[i] = betweenPictures_[i] = {};
  }
}

void VulkanDevice::StartPresenter() {
  presentStop_ = false;
  for (bool& s : slotShowing_) s = false;
  presenter_ = std::thread([this] {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // (AMD's presenter's priority)
#endif
    Presenter();
  });
}

void VulkanDevice::StopPresenter() {
  {
    std::lock_guard<std::mutex> lock(presentMutex_);
    presentStop_ = true;
  }
  presentWake_.notify_all();
  if (presenter_.joinable()) presenter_.join();
  presentStop_ = false;
  if (presentQueue_) vkQueueWaitIdle(presentQueue_);
}

void VulkanDevice::DrainPresenter() {
  {
    std::unique_lock<std::mutex> lock(presentMutex_);
    presentDone_.wait(lock, [&] { return presentJobs_.empty() && std::none_of(std::begin(slotShowing_), std::end(slotShowing_), [](bool b) { return b; }); });
  }
  if (presentQueue_) vkQueueWaitIdle(presentQueue_);
}

void VulkanDevice::WaitPresented(int slot) {
  std::unique_lock<std::mutex> lock(presentMutex_);
  presentDone_.wait(lock, [&] { return !slotShowing_[slot]; });
}

// The presenter's thread: for each frame handed to it, once the card has finished it (its copies into the chain's
// images with it), the picture made between and then the frame, each a pacing's spacing after the one before.
void VulkanDevice::Presenter() {
  Waiter waiter;
  ShownRecord record;
  for (;;) {
    PresentJob job;
    {
      std::unique_lock<std::mutex> lock(presentMutex_);
      presentWake_.wait(lock, [&] { return presentStop_ || !presentJobs_.empty(); });
      if (presentJobs_.empty()) {  // (stopped, and all handed to it shown)
        record.Print("Vulkan");
        return;
      }
      job = presentJobs_.front();
      presentJobs_.pop_front();
    }
    vkWaitForFences(device_, 1, &job.fence, VK_TRUE, UINT64_MAX);
    const double spacing = pacing_.Ready(Now());
    if (job.between >= 0) {
      waiter.Until(lastShown_ + spacing);
      Show(static_cast<uint32_t>(job.between), &lastShown_);
      record.Shown(lastShown_, true);
    }
    waiter.Until(lastShown_ + spacing);
    Show(static_cast<uint32_t>(job.frame), &lastShown_, job.readAt);
    record.Shown(lastShown_, false);
    {
      std::lock_guard<std::mutex> lock(presentMutex_);
      slotShowing_[job.slot] = false;
    }
    presentDone_.notify_all();
  }
}

void VulkanDevice::Show(uint32_t image, double* at, double readAt) {
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &rendered_[image];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &image;
  VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};  // (the latency limiter's and the measure's)
  uint64_t id = 0;
  if (PresentIds()) {
    id = presentIdNext_++;
    pid.swapchainCount = 1;
    pid.pPresentIds = &id;
    pi.pNext = &pid;
  }
  *at = Now();  // (as AMD's: the time just before it is shown, not after the call returns, which may wait)
  VkResult r;
  {
    std::lock_guard<std::mutex> lock(swapMutex_);
    r = vkQueuePresentKHR(presentQueue_, &pi);
  }
  if (r != VK_SUCCESS) chainStale_ = true;  // (out of date or suboptimal: a new chain before the next frame)
  if (id) {
    lastPresentId_ = id;
    if (measureLatency) Watch(id, readAt);
  }
}

// An image of the chain, without holding the chain while it waits (the presenter shows from it meanwhile): tried
// again until one comes free, which the next showing frees; none after a quarter of a second (a window hidden).
bool VulkanDevice::Acquire(VkSemaphore semaphore, uint32_t* image) {
  const double until = Now() + 0.25;
  for (;;) {
    VkResult r;
    {
      std::lock_guard<std::mutex> lock(swapMutex_);
      r = vkAcquireNextImageKHR(device_, swapchain_, 0, semaphore, VK_NULL_HANDLE, image);
    }
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
      if (r == VK_SUBOPTIMAL_KHR) chainStale_ = true;
      return true;
    }
    if ((r != VK_NOT_READY && r != VK_TIMEOUT) || Now() > until) {
      if (r != VK_NOT_READY && r != VK_TIMEOUT) chainStale_ = true;
      return false;
    }
    std::this_thread::yield();  // (not a sleep: Windows' sleeps last a whole timer tick, up to 15.6 ms)
  }
}

// (photofix2) VK_EXT_memory_budget's budget and use of the card's largest heap of its own (where its pictures are made)
bool VulkanDevice::MemoryBudget(uint64_t* budget, uint64_t* used) {
  if (!memoryBudget_) return false;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  p.pNext = &b;
  memoryProperties2_(physical_, &p);
  const VkPhysicalDeviceMemoryProperties& m = p.memoryProperties;
  int heap = -1;
  for (uint32_t i = 0; i < m.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
    if ((m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && (heap < 0 || m.memoryHeaps[i].size > m.memoryHeaps[heap].size)) heap = static_cast<int>(i);
  if (heap < 0 || !b.heapBudget[heap]) return false;
  *budget = b.heapBudget[heap];
  *used = b.heapUsage[heap];
  // no more than this program could still take of what all programs together leave free on the card (card_memory.h:
  // the driver's budget barely heeds the others), where the card's id is known
  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &id;
  vkGetPhysicalDeviceProperties2(physical_, &p2);
  uint64_t all = 0;
  if (id.deviceLUIDValid) {
    uint32_t low = 0;
    int32_t high = 0;
    std::memcpy(&low, id.deviceLUID, 4);
    std::memcpy(&high, id.deviceLUID + 4, 4);
    const uint64_t card = m.memoryHeaps[heap].size;
    if (CardMemoryUsedByAll(low, high, &all)) *budget = std::min<uint64_t>(*budget, *used + (card > all ? card - all : 0));
  }
  return true;
}

// (photomode) The picture a photo's frames draw their window into, and its depth (gfx.h).
bool VulkanDevice::SetPhotoWindow(int width, int height) {
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

// (photomode) A photo frame's end: its picture read back with `capture`; nothing shown, the window's own picture and
// the chain's image held for the next frame untouched.
void VulkanDevice::EndFramePhoto(std::vector<uint8_t>* capture, bool later) {
  Frame& f = frames_[cur_];
  VkCommandBuffer cmd = f.cmd;
  if (queries_ && !f.marks.empty()) {  // the frame's end
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, static_cast<uint32_t>(cur_) * kMarks + static_cast<uint32_t>(f.marks.size()));
    f.marks.push_back("(end)");
  }
  TextureRec* picture = Find(photoColor_);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(photoWidth_) * static_cast<VkDeviceSize>(photoHeight_) * 4;
  const bool reading = capture && EnsureReadback(bytes);
  if (reading) {
    Transition(picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(photoWidth_), static_cast<uint32_t>(photoHeight_), 1};
    vkCmdCopyImageToBuffer(cmd, picture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};  // (what the copy wrote, for the processor to read)
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    Transition(picture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  vkEndCommandBuffer(cmd);
  FlushUploads();  // (what this frame draws with goes to the card ahead of it)
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = waitAcquired_ ? 1 : 0;  // (an image of the chain acquired at BeginFrame stays held for the next)
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue_, 1, &si, f.done);
  inFrame_ = false;
  if (capture) {
    capture->clear();
    if (reading && later) {  // [weather] taken by TakeCapture once the card is done with this frame
      laterWaiting_ = true;
      laterSlot_ = cur_;
      laterNumber_ = frameNumber_;
      laterBytes_ = bytes;
    } else if (reading) {
      vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
      capture->resize(static_cast<size_t>(bytes));
      std::memcpy(capture->data(), readbackMemory_.mapped, capture->size());
    }
  }
  cur_ = (cur_ + 1) % kFrames;
}

int VulkanDevice::TakeCapture(std::vector<uint8_t>* capture) {  // [weather]
  if (!laterWaiting_ || !readback_ || readbackSize_ < laterBytes_) return -1;
  // (done: its slot drawn into again since - BeginFrame waited for it then - or its fence reached)
  const bool done = frameNumber_ >= laterNumber_ + kFrames || vkGetFenceStatus(device_, frames_[laterSlot_].done) == VK_SUCCESS;
  if (!done) return 0;
  laterWaiting_ = false;
  capture->resize(static_cast<size_t>(laterBytes_));
  std::memcpy(capture->data(), readbackMemory_.mapped, capture->size());
  return 1;
}

// A frame's end with frame generation on: the picture between made (it may dispatch and draw), both pictures copied
// into images of the chain by the frame's own commands, and handed to the presenter.
void VulkanDevice::EndFrameGenerating(std::vector<uint8_t>* capture, bool present) {
  Frame& f = frames_[cur_];
  const bool made = makeBetween_ && makeBetween_();
  EndRendering();
  ReleasePassTargets();
  ReleaseGeneral();
  TextureRec* window = Find(windowPictures_[cur_]);
  TextureRec* between = Find(betweenPictures_[cur_]);
  VkCommandBuffer cmd = f.cmd;
  if (queries_ && !f.marks.empty()) {  // the frame's end
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, static_cast<uint32_t>(cur_) * kMarks + static_cast<uint32_t>(f.marks.size()));
    f.marks.push_back("(end)");
  }
  Transition(window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(between, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  if (latency_ && latencyQueries_) {  // (the latency limiter: the frame's last timestamp)
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, latencyQueries_, static_cast<uint32_t>(2 * cur_ + 1));
    latencySlots_[cur_].timed = true;
  }
  if (capture && EnsureReadback()) {
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyImageToBuffer(cmd, (captureBetween && made ? between : window)->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
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
  // the chain's images: the frame's first, then the picture made between's (where there is one)
  int64_t frameImage = -1, betweenImage = -1;
  uint32_t image = 0;
  if (present && swapchain_ && !chainStale_) {
    if (Acquire(f.acquired, &image)) frameImage = image;
    if (made && frameImage >= 0 && Acquire(f.acquired2, &image)) betweenImage = image;
  }
  auto copyInto = [&](TextureRec* from, int64_t into) {  // (a copy that also turns RGBA into the chain's order)
    const uint32_t i = static_cast<uint32_t>(into);
    Barrier(cmd, swapImages_[i], swapLayouts_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {width_, height_, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(swapExtent_.width), static_cast<int32_t>(swapExtent_.height), 1};
    vkCmdBlitImage(cmd, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Barrier(cmd, swapImages_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    swapLayouts_[i] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  };
  if (frameImage >= 0) copyInto(window, frameImage);
  if (betweenImage >= 0) copyInto(between, betweenImage);
  vkEndCommandBuffer(cmd);
  FlushUploads();  // (what this frame draws with goes to the card ahead of it)
  VkSemaphore waits[2], signals[2];
  VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  uint32_t n = 0;
  if (frameImage >= 0) {
    waits[n] = f.acquired;
    signals[n++] = rendered_[static_cast<size_t>(frameImage)];
  }
  if (betweenImage >= 0) {
    waits[n] = f.acquired2;
    signals[n++] = rendered_[static_cast<size_t>(betweenImage)];
  }
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = n;
  si.pWaitSemaphores = waits;
  si.pWaitDstStageMask = stages;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = n;
  si.pSignalSemaphores = signals;
  vkQueueSubmit(queue_, 1, &si, f.done);
  if (latency_ || measureLatency) latencySlots_[cur_].handed = Now(), latencySlots_[cur_].order = ++handedCount_;
  inFrame_ = false;
  if (frameImage >= 0) {  // handed to the presenter, which shows them once the card has finished the frame
    {
      std::lock_guard<std::mutex> lock(presentMutex_);
      slotShowing_[cur_] = true;
      presentJobs_.push_back({cur_, f.done, betweenImage, frameImage, latencySlots_[cur_].read});
    }
    presentWake_.notify_one();
  }
  if (capture) {
    vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
    capture->resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
    std::memcpy(capture->data(), readbackMemory_.mapped, capture->size());
  }
  cur_ = (cur_ + 1) % kFrames;
}

void VulkanDevice::SetVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  if (generating_) DrainPresenter();
  FlushUploads();
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  std::string error;
  if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
}

// --- the latency limiter (gfx.h, SetLatencyLimiter; gfx_latency.h) ---

// Switched now when between frames, else at the next WaitForNextFrame or BeginFrame. The chain stays as it is (the
// present wait needs nothing of it); with frame generation the presenter is stopped and started again around it.
bool VulkanDevice::SetLatencyLimiter(bool on) {
  wantLatency_ = on;
  if (!inFrame_ && wantLatency_ != latency_) SwitchLatency();
  return true;
}

void VulkanDevice::SwitchLatency() {
  if (wantLatency_ && !latencyQueries_ && queries_) {  // the slots' timestamps, the first time
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2 * kFrames;
    if (vkCreateQueryPool(device_, &qpi, nullptr, &latencyQueries_) != VK_SUCCESS) latencyQueries_ = VK_NULL_HANDLE;
  }
  if (generating_) StopPresenter();
  latency_ = wantLatency_;
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
// be shown to one (or two), the chain's image acquired, then the wait that begins the frame just in time; with it off,
// nothing (the moment is only noted for the measure).
void VulkanDevice::WaitForNextFrame() {
  if (inFrame_) return;
  if (wantLatency_ != latency_) SwitchLatency();
  if (latency_ && !PhotoNow()) {  // (a photo's frames are never shown: nothing to wait for, no image to acquire)
    if (generating_) WaitForPresenter();
    // (a switch: BeginFrame's)
    else if (wantGenerating_ == generating_ && !earlyAcquired_ && !imageHeld_) LimitFrames(true);
  }
  if (latency_ || measureLatency) readAt_ = Now();
}

// The frames the card has finished, for the limiter, in the order they were handed over: their input read, their
// hand-over, the card's time for them (their first and last timestamps) and, where the device has calibrated
// timestamps, its begin and end in the processor's time; and the time each was to be done by.
void VulkanDevice::TakeWork() {
  if (!latencyQueries_) return;
  int order[kFrames], n = 0;
  for (int i = 0; i < kFrames; ++i)
    if (latencySlots_[i].timed && vkGetFenceStatus(device_, frames_[i].done) == VK_SUCCESS) order[n++] = i;
  if (n == 0) return;
  std::sort(order, order + n, [&](int a, int b) { return latencySlots_[a].order < latencySlots_[b].order; });
  // (the card's clock and the processor's read together: a timestamp's processor time is the processor's reading and
  // the card's ticks from its reading to the timestamp)
  uint64_t together[2] = {}, deviation = 0;
  bool calibrated = false;
  if (vkGetCalibratedTimestamps_) {
    VkCalibratedTimestampInfoKHR infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}, {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].timeDomain = hostDomain_;
    calibrated = vkGetCalibratedTimestamps_(device_, 2, infos, together, &deviation) == VK_SUCCESS;
  }
#if defined(_WIN32)
  static const double hostPerSecond = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
#else
  constexpr double hostPerSecond = 1e9;  // (CLOCK_MONOTONIC: nanoseconds)
#endif
  auto cpuTime = [&](uint64_t t) {
    return calibrated ? static_cast<double>(together[1]) / hostPerSecond + (static_cast<double>(t) - static_cast<double>(together[0])) * msPerTick_ / 1000 : 0.0;
  };
  for (int k = 0; k < n; ++k) {
    LatencySlot& s = latencySlots_[order[k]];
    s.timed = false;
    uint64_t t[2] = {};
    if (vkGetQueryPoolResults(device_, latencyQueries_, static_cast<uint32_t>(2 * order[k]), 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) continue;
    LatencyFrame lf;
    lf.read = s.read, lf.handed = s.handed, lf.deadline = s.deadline, lf.paced = s.paced;
    if (t[1] > t[0]) {
      lf.card = static_cast<double>(t[1] - t[0]) * msPerTick_ / 1000;
      lf.cardBegin = cpuTime(t[0]), lf.cardEnd = cpuTime(t[1]);
    }
    limiter_.Work(lf);
  }
}

// When the card will be done with the frames it was given (0: not known): as Direct3D 12's (gfx_d3d12.cpp).
double VulkanDevice::CardFree() const {
  double free = limiter_.LastCardEnd();
  if (free <= 0) return 0;
  int order[kFrames], n = 0;
  for (int i = 0; i < kFrames; ++i)
    if (latencySlots_[i].timed) order[n++] = i;
  std::sort(order, order + n, [&](int a, int b) { return latencySlots_[a].order < latencySlots_[b].order; });
  for (int k = 0; k < n; ++k) free = std::max(free, latencySlots_[order[k]].handed) + limiter_.Card();
  return free;
}

// The chain's image for the frame to come (a chain anew where it no longer fits the window): BeginFrame's, and with
// the limiter on WaitForNextFrame's, so that any wait for an image comes before the input read.
void VulkanDevice::AcquireImage(Frame& f) {
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

// Without frame generation. With vsync and the present wait: until the picture before the last is shown (one ahead:
// as Direct3D 12's frame latency of one; two ahead, the one before that); with vsync and without it (an older driver;
// MoltenVK): the frames on the card kept to two; without vsync, or stepped back to the way without the limiter
// (gfx_latency.h, Keep), neither (as Direct3D 12's). Then the chain's image acquired, and the wait that begins the
// frame no earlier than the card is about to be free of the frames before.
// No wait for the refresh it will be shown at, as Direct3D 12 has: the present wait's times come too unevenly to
// pace by (NVIDIA's driver on Windows, a window the compositor shows: the gaps between pictures shown at 240 Hz
// spread by 6-7 ms), and a refresh's time reckoned from them fed back into the waits themselves (a wait of two such
// "refreshes" made the gaps longer still: the viewer all but stopped).
void VulkanDevice::LimitFrames(bool justInTime) {
  const double t0 = Now();
  TakeWork();
  const double t1 = Now();
  // (three ahead, stepped back to the way without the limiter: no wait for the screen beyond the slots' own)
  const bool held = vsync_ && limiter_.Ahead() < 3;
  if (presentWait_ && held) {
    const uint64_t last = lastPresentId_.load(), ahead = static_cast<uint64_t>(limiter_.Ahead());
    if (last >= chainFirstId_ + ahead && swapchain_) {
      const uint64_t id = last - ahead;
      if (id > shown_.Newest()) {
        // (a tenth of a second at most; after a wait that ran out, a window hidden or minimised, only looked at until
        // pictures are shown again, so that such a window's frames are not held up)
        const VkResult r = vkWaitForPresent_(device_, swapchain_, id, latencyStalled_ ? 0 : 100000000ull);
        latencyStalled_ = r == VK_TIMEOUT;
        if (r == VK_SUCCESS && shown_.Shown(id, Now()) && vsync_) limiter_.Shown(shown_.Late(), false);
        else if (r != VK_SUCCESS && r != VK_TIMEOUT) chainStale_ = true;
      }
    }
  } else if (held && !presentWait_) {
    // (the frame before the last)
    vkWaitForFences(device_, 1, &frames_[(cur_ + 1) % kFrames].done, VK_TRUE, 100000000ull);
  }
  Frame& f = frames_[cur_];
  vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);  // (done already: its acquire semaphore free again)
  AcquireImage(f);
  earlyAcquired_ = imageHeld_;
  const double t2 = Now();
  limiter_.Pace(vsync_, Refresh());  // (for how many frames may wait: one, or two where they do not fit a refresh)
  nextDeadline_ = 0;
  nextPaced_ = false;
  const double now = Now();
  const double at = justInTime ? limiter_.StartAt(now, nextDeadline_, CardFree()) : now;
  nextPaced_ = justInTime && limiter_.Paced();
  cost_.Add(t1 - t0, now - t2, t2 - t1, at - now);
  if (!justInTime) return;
  if (at > now) latencyWaiter_.Until(at);
}

// The time between the screen's refreshes (s; 0: not known): on Windows the rate of the screen the window is on, from
// the system (asked at most once a second: the window may be moved to another screen); elsewhere the middle of the gaps
// between the pictures the present wait saw shown (only for how many frames may wait, never for a wait's length).
double VulkanDevice::Refresh() {
#if defined(_WIN32)
  const double now = Now();
  if (now - osRefreshAt_ >= 1 || osRefreshAt_ < 0) {
    osRefreshAt_ = now;
    osRefresh_ = 0;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    const HMONITOR screen = MonitorFromWindow(static_cast<HWND>(window_), MONITOR_DEFAULTTONEAREST);
    if (screen && GetMonitorInfoW(screen, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      osRefresh_ = 1.0 / dm.dmDisplayFrequency;
  }
  if (osRefresh_ > 0) return osRefresh_;
#endif
  return shown_.Period();
}

// With frame generation: as Direct3D 12's (the real frames handed to the presenter and not yet shown kept to one, or
// none where the screen sets the pace and a frame's work fits well within a refresh; the presenter as without).
void VulkanDevice::WaitForPresenter() {
  const double period = Refresh();
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

// The measure: a picture handed over, for the watcher to wait for its showing (started the first time).
void VulkanDevice::Watch(uint64_t id, double readAt) {
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    latencyRecord_.Presented(id, readAt);
    watchIds_.push_back({id, readAt});
  }
  if (!watcher_.joinable()) watcher_ = std::thread([this] { Watcher(); });
  watchWake_.notify_one();
}

// The measure's thread: for each picture in turn, the present wait until it is shown (a fiftieth of a second at a
// time, so that a chain being made anew is not held up), and when that was.
void VulkanDevice::Watcher() {
  for (;;) {
    uint64_t id = 0;
    {
      std::unique_lock<std::mutex> lock(watchMutex_);
      watchWake_.wait(lock, [&] { return watchStop_ || !watchIds_.empty(); });
      if (watchIds_.empty()) return;
      id = watchIds_.front().first;
    }
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    {
      std::lock_guard<std::mutex> chain(watchChain_);
      if (swapchain_ && id >= chainFirstId_) r = vkWaitForPresent_(device_, swapchain_, id, 20000000ull);
    }
    const double at = Now();
    std::lock_guard<std::mutex> lock(watchMutex_);
    if (r == VK_TIMEOUT && !watchStop_) continue;
    if (!watchIds_.empty() && watchIds_.front().first == id) watchIds_.pop_front();
    if (r == VK_SUCCESS) latencyRecord_.Shown(id, at);
  }
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
  // [vkpresent] (off the frames' thread - a map opening behind, a worker making ahead - it waits for room in the blocks there
  // are rather than take a new one while frames are shown)
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, memory,
                   std::this_thread::get_id() != framesThread_)) {
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

// [vkpresent] The computer's memory in bytes (0: not known).
static uint64_t SystemMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? static_cast<uint64_t>(m.ullTotalPhys) : 0;
#elif defined(__APPLE__)
  uint64_t bytes = 0;
  size_t size = sizeof bytes;
  return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
#endif
}

// [vkpresent] The staging's reserve, in blocks of 256 MB: min(1 GB, the computer's memory / 16), at least one (two where
// the memory is not known); gStagingReserveMB (checks: the viewer's --staging-reserve) in place of that where set.
int VulkanDevice::StagingReserveBlocks() {
  constexpr uint64_t kBlockBytes = 256ull << 20;
  uint64_t bytes = gStagingReserveMB > 0 ? static_cast<uint64_t>(gStagingReserveMB) << 20 : 0;
  if (!bytes) {
    const uint64_t system = SystemMemoryBytes();
    bytes = system ? std::min<uint64_t>(4 * kBlockBytes, system / 16) : 2 * kBlockBytes;
  }
  const int blocks = static_cast<int>(std::max<uint64_t>(1, (bytes + kBlockBytes / 2) / kBlockBytes));
  printf("render: Vulkan: the staging's reserve %d MB\n", blocks * 256);
  return blocks;
}

// [vkpresent] The staging of the upload batches the card has done, let go of now (else only when the batch's turn came round
// again, four batches later): a map opening behind waits for that room (StagingBuffer) rather than take more memory.
void VulkanDevice::RecycleUploads() {
  for (int i = 0; i < kUploadBatches; ++i) {
    Upload& u = uploads_[i];
    if (u.staging.empty() || (i == upload_ && uploadOpen_) || vkGetFenceStatus(device_, u.done) != VK_SUCCESS) continue;
    for (auto& s : u.staging) {
      vkDestroyBuffer(device_, s.first, nullptr);
      memory_.Free(s.second);
    }
    u.staging.clear();
  }
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
  if (desc.kind == BufferKind::Card) {  // [gpuscenery] the card's own: written by dispatches and copies, read as indirect arguments
    if (desc.dynamic || !desc.size || (desc.size & 15u)) return {};
    rec.size = desc.size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
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
    if (!StagingBuffer(desc.size, &staging, &stagingMemory)) {
      vkDestroyBuffer(device_, rec.buffer, nullptr);
      memory_.Free(rec.memory);
      return {};
    }
    if (desc.data) std::memcpy(stagingMemory.mapped, desc.data, desc.size);
    else std::memset(stagingMemory.mapped, 0, desc.size);
    VkBufferCopy copy{0, 0, desc.size};
    vkCmdCopyBuffer(UploadCommands(), staging, rec.buffer, 1, &copy);
    KeepStaging(staging, stagingMemory, desc.size);
  } else if (!desc.dynamic) {
    if (!desc.data || !desc.size) return {};
    rec.size = desc.size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // (its size a whole number of 4-byte words: a shader reads it a word at a time, SetShaderBuffer, and the last
    // word of 16-bit indices of an odd count lies half past the data)
    bci.size = (static_cast<VkDeviceSize>(desc.size) + 3) & ~VkDeviceSize{3};
    // every static buffer can be read by shaders as well (SetShaderBuffer: a model's vertices and indices)
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                (desc.kind == BufferKind::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                 : desc.kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                   : 0);
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
  if (desc.kind == BufferKind::Card) cardBuffers_.push_back(index + 1);  // [gpuscenery]
  return {index + 1};
}

// [gpuscenery] Whether a shader buffer's slot holds a card buffer.
bool VulkanDevice::CardBound(int slot) const {
  const VkBuffer bound = boundBuffers_[slot].buffer;
  if (!bound) return false;
  for (uint32_t id : cardBuffers_)
    if (id <= buffers_.size() && buffers_[id - 1].used && buffers_[id - 1].buffer == bound) return true;
  return false;
}

void VulkanDevice::CardBarrier(VkPipelineStageFlags2 from, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 to, VkAccessFlags2 toAccess) {
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

void VulkanDevice::UpdateBufferRange(Buffer buffer, uint32_t offset, const void* data, uint32_t size) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Card || !inFrame_ || parallel_ || !data || !size || ((offset | size) & 3u) ||
      static_cast<uint64_t>(offset) + size > rec->size)
    return;
  Span s = Allocate(size, 16);
  if (!s.cpu) return;
  std::memcpy(s.cpu, data, size);
  // (a copy cannot lie inside a pass: the pass is left and begun again after it, keeping what it drew; what read or wrote
  // the buffer before is done first, and what reads it after waits for the copy)
  const bool wasRendering = rendering_;
  const bool hadDepth = renderingDepth_;
  EndRendering();
  CardBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT);
  VkBufferCopy copy{s.offset, offset, size};
  vkCmdCopyBuffer(frames_[cur_].cmd, s.buffer, rec->buffer, 1, &copy);
  CardBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  if (wasRendering) StartRendering(hadDepth);
}

void VulkanDevice::DrawIndexedIndirect(Buffer arguments, uint32_t offset) {
  const BufferRec* rec = Find(arguments);
  if (!rec || rec->kind != BufferKind::Card || (offset & 3u) || static_cast<uint64_t>(offset) + 20 > rec->size) return;
  if (!PrepareDraw()) return;
  ++draws_;
  vkCmdDrawIndexedIndirect(frames_[cur_].cmd, rec->buffer, offset, 1, 20);
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

// [spikes] The halves of CreateTexture and of a static CreateBuffer (gfx.h, PrepareTexture). Prepare, on any thread: the
// image (or buffer), its memory and its view made, and the caller's data copied into a staging buffer of its own (the device
// is free for these; the memory's blocks are shared out under their own lock, Memory). Finish, on the frames' thread: the
// copy and the layouts recorded into the open upload batch, as CreateTexture's are, and the handle. One never finished is
// let go of at once (nothing of the card's work has it).
namespace {
struct PreparedVk final : Device::Prepared {
  VulkanDevice* device = nullptr;
  bool texture = false;
  TextureRec tex;
  BufferRec buf;
  VkBuffer staging = VK_NULL_HANDLE;
  Allocation stagingMemory;
  std::vector<VkBufferImageCopy> copies;
  VkDeviceSize bytes = 0;
  // [warmtarget] a large texture's further staging buffers (kStagingChunk at most each), with their copies
  struct More {
    VkBuffer staging = VK_NULL_HANDLE;
    Allocation memory;
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize bytes = 0;
  };
  std::vector<More> more;
  bool finished = false;
  ~PreparedVk() override {
    if (!finished && device) {
      device->DropPrepared(texture ? &tex : nullptr, texture ? nullptr : &buf, staging, stagingMemory);
      for (const More& m : more) device->DropPrepared(nullptr, nullptr, m.staging, m.memory);
    }
  }
};
// [warmtarget] A texture's staging in buffers of at most this many bytes: one larger took a block of the memory of its own
// (Memory::Get), given back to the driver when its upload batch was done, which took the driver 25-30 ms (a map opening
// behind: the remastered ground's 4K layers, 363 MB in one).
constexpr size_t kStagingChunk = 128ull << 20;
}  // namespace

void VulkanDevice::DropPrepared(TextureRec* texture, BufferRec* buffer, VkBuffer staging, const Allocation& stagingMemory) {
  if (texture) {
    if (texture->view) vkDestroyImageView(device_, texture->view, nullptr);
    if (texture->image) vkDestroyImage(device_, texture->image, nullptr);
    if (texture->memory.memory) memory_.Free(texture->memory);
  }
  if (buffer) {
    if (buffer->buffer) vkDestroyBuffer(device_, buffer->buffer, nullptr);
    if (buffer->memory.memory) memory_.Free(buffer->memory);
  }
  if (staging) vkDestroyBuffer(device_, staging, nullptr);
  if (stagingMemory.memory) memory_.Free(stagingMemory);
}

std::unique_ptr<Device::Prepared> VulkanDevice::PrepareTexture(const TextureDesc& desc) {
  const VkFormat format = ToVk(desc.format);
  if (format == VK_FORMAT_UNDEFINED || desc.width <= 0 || desc.height <= 0 || desc.mips <= 0 || desc.layers <= 0 || !desc.data) return nullptr;
  auto p = std::make_unique<PreparedVk>();
  p->device = this;
  p->texture = true;
  TextureRec& rec = p->tex;  // (as CreateTexture's)
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
  if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return nullptr;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, rec.image, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) return nullptr;
  vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rec.image;
  vi.viewType = desc.cube && desc.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  if (desc.format == Format::A8) vi.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  if (desc.format == Format::BC5) vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(desc.mips), 0, static_cast<uint32_t>(desc.layers)};
  if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return nullptr;
  const bool blocks = IsBlockCompressed(desc.format);
  const size_t unit = static_cast<size_t>(BytesPerPixelOrBlock(desc.format));
  size_t at = 0, read = 0;
  std::vector<size_t> chunkOf;  // [warmtarget] (each copy's staging buffer: 0 the first, n the more[n - 1])
  std::vector<VkBufferImageCopy>* copies = &p->copies;
  VkDeviceSize* chunkBytes = &p->bytes;
  for (int layer = 0; layer < desc.layers; ++layer)
    for (int mip = 0; mip < desc.mips; ++mip) {
      const size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
      const size_t bytes = (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
      read += bytes;
      if (read > desc.dataSize) {
        fprintf(stderr, "render: a %dx%d texture with %d mips has too little data (%zu bytes)\n", desc.width, desc.height, desc.mips, desc.dataSize);
        return nullptr;
      }
      if (at > 0 && at + bytes > kStagingChunk) {  // [warmtarget] (the next staging buffer)
        *chunkBytes = at;
        p->more.emplace_back();
        copies = &p->more.back().copies;
        chunkBytes = &p->more.back().bytes;
        at = 0;
      }
      VkBufferImageCopy c{};
      c.bufferOffset = at;
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(mip), static_cast<uint32_t>(layer), 1};
      c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
      copies->push_back(c);
      chunkOf.push_back(p->more.size());
      at += bytes;
      at = (at + 15) & ~size_t{15};
    }
  *chunkBytes = at;
  if (!StagingBuffer(std::max<size_t>(p->bytes, 16), &p->staging, &p->stagingMemory)) return nullptr;
  for (PreparedVk::More& m : p->more)
    if (!StagingBuffer(std::max<size_t>(m.bytes, 16), &m.staging, &m.memory)) return nullptr;
  const uint8_t* src = static_cast<const uint8_t*>(desc.data);
  size_t k = 0;
  std::vector<size_t> inChunk(p->more.size() + 1, 0);  // (each staging buffer's copies taken so far)
  for (int layer = 0; layer < desc.layers; ++layer)
    for (int mip = 0; mip < desc.mips; ++mip, ++k) {
      const size_t w = static_cast<size_t>(std::max(1, desc.width >> mip)), h = static_cast<size_t>(std::max(1, desc.height >> mip));
      const size_t bytes = (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
      const size_t n = chunkOf[k];
      const VkBufferImageCopy& c = n ? p->more[n - 1].copies[inChunk[n]] : p->copies[inChunk[n]];
      ++inChunk[n];
      std::memcpy((n ? p->more[n - 1].memory.mapped : p->stagingMemory.mapped) + c.bufferOffset, src, bytes);
      src += bytes;
    }
  return p;
}

Texture VulkanDevice::FinishTexture(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedVk*>(prepared.get());
  if (!p) return Device::FinishTexture(std::move(prepared));  // (one kept whole: CreateTexture)
  if (!p->texture) return {};
  TextureRec& rec = p->tex;
  VkCommandBuffer cmd = UploadCommands();
  Barrier(cmd, rec.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  vkCmdCopyBufferToImage(cmd, p->staging, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(p->copies.size()), p->copies.data());
  for (const PreparedVk::More& m : p->more)  // [warmtarget]
    vkCmdCopyBufferToImage(cmd, m.staging, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(m.copies.size()), m.copies.data());
  Barrier(cmd, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  p->finished = true;
  // (all kept with the batch their copies are in, and that sent on after, where it has grown large: [vkpresent] a staging
  // buffer kept with the next batch was let go of as soon as that batch's slot was taken again, its copy maybe not done yet)
  for (const PreparedVk::More& m : p->more) {
    uploads_[upload_].staging.push_back({m.staging, m.memory});
    uploadBytes_ += m.bytes;
  }
  KeepStaging(p->staging, p->stagingMemory, p->bytes);
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

std::unique_ptr<Device::Prepared> VulkanDevice::PrepareBuffer(const BufferDesc& desc) {
  if (desc.dynamic || !desc.data || !desc.size) return nullptr;
  auto p = std::make_unique<PreparedVk>();
  p->device = this;
  BufferRec& rec = p->buf;  // (as CreateBuffer's static path)
  rec.used = true;
  rec.kind = desc.kind;
  rec.stride = desc.stride;
  rec.size = desc.size;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = (static_cast<VkDeviceSize>(desc.size) + 3) & ~VkDeviceSize{3};
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              (desc.kind == BufferKind::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : desc.kind == BufferKind::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : 0);
  if (rayTracing_ && desc.kind != BufferKind::Shader)
    bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (vkCreateBuffer(device_, &bci, nullptr, &rec.buffer) != VK_SUCCESS) return nullptr;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, rec.buffer, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &rec.memory)) return nullptr;
  vkBindBufferMemory(device_, rec.buffer, rec.memory.memory, rec.memory.offset);
  if (!StagingBuffer(desc.size, &p->staging, &p->stagingMemory)) return nullptr;
  std::memcpy(p->stagingMemory.mapped, desc.data, desc.size);
  p->bytes = desc.size;
  return p;
}

Buffer VulkanDevice::FinishBuffer(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedVk*>(prepared.get());
  if (!p) return Device::FinishBuffer(std::move(prepared));  // (one kept whole: CreateBuffer)
  if (p->texture) return {};
  VkBufferCopy copy{0, 0, p->bytes};
  vkCmdCopyBuffer(UploadCommands(), p->staging, p->buf.buffer, 1, &copy);
  p->finished = true;
  KeepStaging(p->staging, p->stagingMemory, p->bytes);
  uint32_t index;
  if (!freeBuffers_.empty()) {
    index = freeBuffers_.back();
    freeBuffers_.pop_back();
    buffers_[index] = p->buf;
  } else {
    index = static_cast<uint32_t>(buffers_.size());
    buffers_.push_back(p->buf);
  }
  return {index + 1};
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
  TextureRec rec;  // [warmtarget] (in two halves, as PrepareTarget / FinishTarget make it)
  if (!MakeTarget(desc, &rec)) return {};
  return AddTarget(std::move(rec));
}

// [warmtarget] A target's image, memory and views, as CreateTarget makes them: on any thread (the device is free for these; the
// memory's blocks are shared out under their own lock, Memory). False: it cannot be made (said where CreateTarget says it).
bool VulkanDevice::MakeTarget(const TargetDesc& desc, TextureRec* out) {
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
      return false;
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
    if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, rec.image, &req);
    if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) {
      vkDestroyImage(device_, rec.image, nullptr);
      return false;
    }
    vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = rec.image;
    vi.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(desc.mips), 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return false;
    for (int mip = 0; mip < desc.mips; ++mip) {
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(mip), 1, 0, 1};
      VkImageView v;
      if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) return false;
      rec.storageViews.push_back(v);
    }
    *out = std::move(rec);
    return true;
  }
  const bool depth = desc.format == Format::D32;
  const VkFormat format = ToVk(desc.format);
  const int samples = desc.samples == 2 || desc.samples == 4 || desc.samples == 8 ? desc.samples : 1;
  if (format == VK_FORMAT_UNDEFINED || IsBlockCompressed(desc.format) || desc.width <= 0 || desc.height <= 0 || desc.layers <= 0 ||
      (!depth && desc.layers != 1) || (samples > 1 && desc.layers != 1))
    return false;
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
  if (vkCreateImage(device_, &ii, nullptr, &rec.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, rec.image, &req);
  if (!memory_.Get(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &rec.memory)) {
    vkDestroyImage(device_, rec.image, nullptr);
    return false;
  }
  vkBindImageMemory(device_, rec.image, rec.memory.memory, rec.memory.offset);
  const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rec.image;
  vi.viewType = desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, static_cast<uint32_t>(desc.layers)};
  if (vkCreateImageView(device_, &vi, nullptr, &rec.view) != VK_SUCCESS) return false;
  for (int layer = 0; layer < desc.layers; ++layer) {  // (as an attachment: one layer at a time)
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = {aspect, 0, 1, static_cast<uint32_t>(layer), 1};
    VkImageView v;
    if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) return false;
    if (desc.layers == 1) rec.attachView = v;
    else rec.layerViews.push_back(v);
  }
  *out = std::move(rec);
  return true;
}

// [warmtarget] A target made by MakeTarget, given its handle (the frames' thread): readable from the start, as Direct3D 12's
// targets are (a storage target as every target).
Texture VulkanDevice::AddTarget(TextureRec rec) {
  Barrier(UploadCommands(), rec.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          rec.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  uint32_t index;
  if (!freeTextures_.empty()) {
    index = freeTextures_.back();
    freeTextures_.pop_back();
  } else {
    index = static_cast<uint32_t>(textures_.size());
    textures_.emplace_back();
  }
  textures_[index] = std::move(rec);
  return {index};
}

// [warmtarget] The halves of CreateTarget (gfx.h, PrepareTarget). One never finished is let go of at once (nothing of the card's
// work has it).
namespace {
struct PreparedTargetVk final : Device::Prepared {
  VulkanDevice* device = nullptr;
  TextureRec rec;
  bool finished = false;
  ~PreparedTargetVk() override {
    if (!finished && device) device->DropTarget(rec);
  }
};
}  // namespace

void VulkanDevice::DropTarget(TextureRec& rec) {
  for (VkImageView v : rec.storageViews) vkDestroyImageView(device_, v, nullptr);
  for (VkImageView v : rec.layerViews) vkDestroyImageView(device_, v, nullptr);
  if (rec.attachView) vkDestroyImageView(device_, rec.attachView, nullptr);
  if (rec.view) vkDestroyImageView(device_, rec.view, nullptr);
  if (rec.image) vkDestroyImage(device_, rec.image, nullptr);
  if (rec.memory.memory) memory_.Free(rec.memory);
  rec = TextureRec{};
}

std::unique_ptr<Device::Prepared> VulkanDevice::PrepareTarget(const TargetDesc& desc) {
  auto p = std::make_unique<PreparedTargetVk>();
  if (!MakeTarget(desc, &p->rec)) {  // (what it made of it let go of; CreateTarget then)
    DropTarget(p->rec);
    return nullptr;
  }
  p->device = this;
  return p;
}

Texture VulkanDevice::FinishTarget(std::unique_ptr<Prepared> prepared) {
  auto* p = dynamic_cast<PreparedTargetVk*>(prepared.get());
  if (!p) return Device::FinishTarget(std::move(prepared));  // (one kept whole: CreateTarget)
  p->finished = true;
  return AddTarget(std::move(p->rec));
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
  if (rec->kind == BufferKind::Card) cardBuffers_.erase(std::remove(cardBuffers_.begin(), cardBuffers_.end(), buffer.id), cardBuffers_.end());  // [gpuscenery]
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
  if (rec->background) {  // [snowhitch] (asked for in the background and not taken: not made; or made and its pipeline let go of)
    std::unique_lock<std::mutex> lock(backgroundMutex_);
    auto at = std::find(backgroundQueue_.begin(), backgroundQueue_.end(), rec->background);
    if (at != backgroundQueue_.end()) {
      backgroundQueue_.erase(at);
    } else {
      std::shared_ptr<BackgroundPipeline> job = rec->background;
      backgroundDone_.wait(lock, [&] { return job->stage == BackgroundPipeline::Made || job->stage == BackgroundPipeline::NotMade; });
      if (job->made.pipeline) Bin().pipelines.push_back(job->made.pipeline);
      job->made.pipeline = VK_NULL_HANDLE;
    }
  }
  if (rec->pipeline) Bin().pipelines.push_back(rec->pipeline);
  *rec = PipelineRec{};
  freePipelines_.push_back(pipeline.id - 1);
}

// --- pipelines ---

const VulkanDevice::Layout* VulkanDevice::LayoutFor(const std::vector<SpirvImage>& images) {
  std::lock_guard<std::mutex> lock(layoutsMutex_);  // [snowhitch] (pipelines are made on the device's own thread too)
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

bool VulkanDevice::MakePipeline(const PipelineDesc& desc, PipelineRec* out, std::string* error, bool background) {
  SpirvShader vs, ps;
  std::string why;
  const bool hasPixelShader = !desc.pixelSource.empty();
  if (background) {  // [snowhitch] (on the device's own thread: from the cache, what it lacks compiled into it first)
    const std::string vSource(desc.vertexSource), pSource(desc.pixelSource);
    auto cached = [&] {
      return CachedSpirv(vSource, desc.vertexEntry, "vs", shaderCache_, &vs) && (!hasPixelShader || CachedSpirv(pSource, desc.pixelEntry, "ps", shaderCache_, &ps));
    };
    if (!cached()) {
      std::vector<ShaderJob> jobs = {{vSource, desc.vertexEntry, "vs"}};
      if (hasPixelShader) jobs.push_back({pSource, desc.pixelEntry, "ps"});
      PrewarmSpirv(jobs, shaderCache_);
      if (!cached()) {
        if (error) *error = std::string(desc.name) + ": its shaders could not be compiled on the device's own thread";
        return false;
      }
    }
  } else {
    if (!CompileSpirv(std::string(desc.vertexSource), desc.vertexEntry, "vs", shaderCache_, &vs, &why)) {
      if (error) *error = std::string(desc.name) + " (" + desc.vertexEntry + "): " + why;
      return false;
    }
    if (hasPixelShader && !CompileSpirv(std::string(desc.pixelSource), desc.pixelEntry, "ps", shaderCache_, &ps, &why)) {
      if (error) *error = std::string(desc.name) + " (" + desc.pixelEntry + "): " + why;
      return false;
    }
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
    return false;
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
                            ? "a texture outside t0-t43"
                        : image.kind == SpirvImage::Scene && !rayTracing_ ? "it traces rays, which this card or its driver cannot"
                                                                          : nullptr;
    if (wrong) {
      if (error) *error = std::string(desc.name) + ": " + wrong;
      for (VkShaderModule m : modules)
        if (m) vkDestroyShaderModule(device_, m, nullptr);
      return false;
    }
  }
  const Layout* layout = LayoutFor(images);
  if (!layout) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline layout";
    for (VkShaderModule m : modules)
      if (m) vkDestroyShaderModule(device_, m, nullptr);
    return false;
  }
  pi.layout = layout->pipeline;
  PipelineRec rec;
  rec.used = true;
  rec.layout = layout;
  rec.depth = desc.depthTarget;
  const VkResult r = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pi, nullptr, &rec.pipeline);  // [hitches] (the kept ones)
  if (r == VK_SUCCESS) pipelineCacheGrew_ = true;
  for (VkShaderModule m : modules)
    if (m) vkDestroyShaderModule(device_, m, nullptr);
  if (r != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline: " + Result(r);
    return false;
  }
  *out = rec;
  return true;
}

bool VulkanDevice::MakeCompute(const ComputeDesc& desc, PipelineRec* out, std::string* error, bool background) {
  SpirvShader cs;
  std::string why;
  if (background) {  // [snowhitch] (as MakePipeline's)
    const std::string source(desc.source);
    if (!CachedSpirv(source, desc.entry, "cs", shaderCache_, &cs)) {
      PrewarmSpirv({{source, desc.entry, "cs"}}, shaderCache_);
      if (!CachedSpirv(source, desc.entry, "cs", shaderCache_, &cs)) {
        if (error) *error = std::string(desc.name) + ": its shader could not be compiled on the device's own thread";
        return false;
      }
    }
  } else if (!CompileSpirv(std::string(desc.source), desc.entry, "cs", shaderCache_, &cs, &why)) {
    if (error) *error = std::string(desc.name) + " (" + desc.entry + "): " + why;
    return false;
  }
  std::vector<SpirvImage> images = cs.images;
  std::sort(images.begin(), images.end());
  for (const auto& image : images) {
    const bool storage = image.kind == SpirvImage::Storage || image.kind == SpirvImage::StorageVolume;
    const uint32_t from = storage ? kStorageBinding : kTexBinding, slots = storage ? kStorageSlots : kTexSlots;
    if (image.binding < from || image.binding + image.count > from + slots) {
      if (error) *error = std::string(desc.name) + ": a texture outside t0-t43 or u0-u15";
      return false;
    }
    if (image.kind == SpirvImage::Scene && !rayTracing_) {
      if (error) *error = std::string(desc.name) + ": it traces rays, which this card or its driver cannot";
      return false;
    }
  }
  const Layout* layout = LayoutFor(images);
  if (!layout) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no pipeline layout";
    return false;
  }
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.codeSize = cs.words.size() * 4;
  mi.pCode = cs.words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &mi, nullptr, &module) != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan took no shader module";
    return false;
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
  const VkResult r = vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &rec.pipeline);  // [hitches] (the kept ones)
  if (r == VK_SUCCESS) pipelineCacheGrew_ = true;
  vkDestroyShaderModule(device_, module, nullptr);
  if (r != VK_SUCCESS) {
    if (error) *error = std::string(desc.name) + ": Vulkan made no compute pipeline: " + Result(r);
    return false;
  }
  *out = rec;
  return true;
}

Pipeline VulkanDevice::Keep(const PipelineRec& rec) {
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

Pipeline VulkanDevice::CreatePipeline(const PipelineDesc& desc, std::string* error) {
  PipelineRec rec;
  if (batching_) {  // (made at the batch's end, its shaders compiled with the others')
    rec.used = true;
    rec.pending = PendingPipeline::Of(desc);
    return Keep(rec);
  }
  if (backgroundAsked_) {  // [snowhitch] (made on the device's own thread; taken the first time it is set)
    rec.used = true;
    rec.depth = desc.depthTarget;
    rec.background = AskBackground(PendingPipeline::Of(desc));
    return Keep(rec);
  }
  HitchLog::Scope made("pipeline", desc.name);  // [hitches] (one made outside a batch: while the game runs)
  if (!MakePipeline(desc, &rec, error)) return {};
  return Keep(rec);
}

Pipeline VulkanDevice::CreateCompute(const ComputeDesc& desc, std::string* error) {
  PipelineRec rec;
  if (batching_) {
    rec.used = rec.compute = true;
    rec.pending = PendingPipeline::Of(desc);
    return Keep(rec);
  }
  if (backgroundAsked_) {  // [snowhitch]
    rec.used = rec.compute = true;
    rec.background = AskBackground(PendingPipeline::Of(desc));
    return Keep(rec);
  }
  HitchLog::Scope made("compute pipeline", desc.name);  // [hitches]
  if (!MakeCompute(desc, &rec, error)) return {};
  return Keep(rec);
}

// A pipeline asked for in a batch, made now in its slot (what could not be made leaves the slot empty: its
// handle then draws nothing).
bool VulkanDevice::Realize(uint32_t index, std::string* error) {
  std::shared_ptr<PendingPipeline> p = std::move(pipelines_[index].pending);
  if (!p) return true;
  PipelineRec rec;
  const bool made = p->compute ? MakeCompute(p->computeDesc, &rec, error) : MakePipeline(p->desc, &rec, error);
  pipelines_[index] = made ? rec : PipelineRec{};
  return made;
}

// [snowhitch] A pipeline to be made on the device's own thread (begun the first time one is asked for; as gfx_d3d12.cpp's).
std::shared_ptr<BackgroundPipeline> VulkanDevice::AskBackground(std::shared_ptr<PendingPipeline> what) {
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

// [snowhitch] The device's own thread: the pipelines asked for, made one after another in the order asked (the pipeline cache
// is the driver's, which keeps it safe from several threads; the layouts are shared under their own lock).
void VulkanDevice::BackgroundWork() {
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
    const bool ok = what.compute ? MakeCompute(what.computeDesc, &rec, &why, true) : MakePipeline(what.desc, &rec, &why, true);
    made.End();
    {
      std::lock_guard<std::mutex> lock(backgroundMutex_);
      backgroundMaking_ = 0;  // [bgload]
      job->made = rec;
      job->why = why;
      job->stage = ok ? BackgroundPipeline::Made : BackgroundPipeline::NotMade;
    }
    backgroundDone_.notify_all();
  }
}

// [snowhitch] Pipeline `index`, asked for in the background, into its slot: as made there; waited for while it is being made;
// made here, as it would have been when it was asked for, when its turn has not come yet or it could not be made there.
void VulkanDevice::TakeBackground(uint32_t index) {
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
    rec = job->made;
    return;
  }
  const PendingPipeline& what = *job->what;
  if (!job->why.empty()) fprintf(stderr, "render: Vulkan: %s; made on the frames' thread\n", job->why.c_str());
  PipelineRec made;
  std::string why;
  bool ok = false;
  {
    HitchLog::Scope note(what.compute ? "compute pipeline" : "pipeline", what.name);  // [hitches] (as CreatePipeline's)
    ok = what.compute ? MakeCompute(what.computeDesc, &made, &why) : MakePipeline(what.desc, &made, &why);
  }
  if (!ok) fprintf(stderr, "render: Vulkan: %s\n", why.c_str());
  if (ok) {
    rec = made;
  } else {  // (cannot be made: its handle draws nothing, as a batch's)
    rec = PipelineRec{};
    rec.used = true;
  }
}

void VulkanDevice::BeginPipelineBatch() {
  batching_ = !shaderCache_.empty();
  batchError_.clear();
}

bool VulkanDevice::EndPipelineBatch(std::string* error) {
  if (!batching_) return true;
  batching_ = false;
  std::vector<ShaderJob> jobs;
  for (const PipelineRec& r : pipelines_)
    if (r.pending) r.pending->Jobs(&jobs);
  PrewarmSpirv(jobs, shaderCache_);  // (the cache's missing shaders, on several cores at once)
  for (uint32_t i = 0; i < pipelines_.size(); ++i) {
    std::string why;
    if (pipelines_[i].pending && !Realize(i, &why) && batchError_.empty()) batchError_ = why;
  }
  SavePipelineCache();  // [hitches] (what the driver made of them kept for the next run, now: a game may end any way)
  if (batchError_.empty()) return true;
  if (error) *error = batchError_;
  return false;
}

// [hitches] (pipelineCache_) The kept file given to the driver where its header is this card's and this driver's cache's.
void VulkanDevice::OpenPipelineCache() {
  std::vector<uint8_t> data;
  if (!shaderCache_.empty())
    if (FILE* f = fopen((shaderCache_ + "/pipelines-vulkan.bin").c_str(), "rb")) {
      fseek(f, 0, SEEK_END);
      const long size = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (size > 0 && size < (256L << 20)) {
        data.resize(static_cast<size_t>(size));
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
      }
      fclose(f);
    }
  // (its header: its own size, version 1, the card's vendor and device, the driver's cache's UUID)
  bool fits = data.size() >= 16 + VK_UUID_SIZE;
  if (fits) {
    uint32_t h[4];
    std::memcpy(h, data.data(), sizeof h);
    fits = h[0] >= 16 + VK_UUID_SIZE && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == props_.vendorID && h[3] == props_.deviceID &&
           std::memcmp(data.data() + 16, props_.pipelineCacheUUID, VK_UUID_SIZE) == 0;
  }
  if (!fits) data.clear();
  VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  pc.initialDataSize = data.size();
  pc.pInitialData = data.empty() ? nullptr : data.data();
  if (vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_) != VK_SUCCESS) pipelineCache_ = VK_NULL_HANDLE;
}

void VulkanDevice::SavePipelineCache() {
  if (!pipelineCache_ || !pipelineCacheGrew_ || shaderCache_.empty()) return;
  pipelineCacheGrew_ = false;
  size_t size = 0;
  if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || !size || size >= (256u << 20)) return;
  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
  // (written under another name first, then put in its place: a half-written file is never read)
  const std::string path = shaderCache_ + "/pipelines-vulkan.bin";
#if defined(_WIN32)
  const std::string part = path + ".part" + std::to_string(GetCurrentProcessId());
#else
  const std::string part = path + ".part" + std::to_string(getpid());
#endif
  FILE* f = fopen(part.c_str(), "wb");
  if (!f) return;
  const bool ok = fwrite(data.data(), 1, size, f) == size;
  fclose(f);
#if defined(_WIN32)
  if (!ok || !MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) std::remove(part.c_str());
#else
  if (!ok || std::rename(part.c_str(), path.c_str()) != 0) std::remove(part.c_str());
#endif
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
  if (cardWritten_) {  // [gpuscenery] (what dispatches wrote of card buffers, there for the draws: as indirect arguments too)
    CardBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    cardWritten_ = false;
  }
  for (uint32_t id : general_)
    if (id < textures_.size() && textures_[id].used) Transition(&textures_[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  general_.clear();
}

void VulkanDevice::BeginFrame(const float clearColor[4]) {
  if (wantGenerating_ != generating_) SwitchFrameGeneration();  // (asked for during the frame before)
  if (wantLatency_ != latency_) SwitchLatency();
  // (the latency limiter, where the host did not wait before its input: its wait here)
  if (latency_ && !generating_ && !earlyAcquired_ && !imageHeld_ && !PhotoNow()) LimitFrames(false);  // (not for a photo's frames)
  Frame& f = frames_[cur_];
  vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
  RecycleUploads();  // [vkpresent]
  if (generating_) WaitPresented(cur_);  // (the presenter no longer waits on this slot's fence)
  if (latency_ && latencySlots_[cur_].timed) TakeWork();  // (as Direct3D 12's: the slot's frame before, done now)
  if (latency_ || measureLatency) {  // (this frame's input read: the host's last WaitForNextFrame, else now)
    latencySlots_[cur_] = LatencySlot{readAt_ > 0 ? readAt_ : Now(), 0, nextDeadline_, nextPaced_, false};
    readAt_ = nextDeadline_ = 0;
    nextPaced_ = false;
  }
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
  // (the latency limiter acquired this frame's image before the input read)
  if (!earlyAcquired_) waitAcquired_ = false;
  earlyAcquired_ = false;
  if (generating_) {  // (the frame's end acquires the chain's images; a chain anew when it no longer fits)
    if (chainStale_ || !swapchain_) {
      DrainPresenter();
      vkDeviceWaitIdle(device_);
      DestroySwapchain();
      std::string error;
      if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
    }
  } else if (!imageHeld_) {  // (an image held by a frame not shown is drawn into again)
    AcquireImage(f);
  }
  vkResetFences(device_, 1, &f.done);
  vkResetCommandPool(device_, f.pool, 0);
  for (RecorderRoom& r : f.recorders) {  // [mcrec] (the recorders' room in this slot anew)
    if (r.pool) vkResetCommandPool(device_, r.pool, 0);
    r.cmdsUsed = 0;
    for (VkDescriptorPool p : r.descriptors) vkResetDescriptorPool(device_, p, 0);
    r.descriptorsUsed = 0;
    r.sets.clear();
    r.constantSets.clear();
    r.constantPage = SIZE_MAX;
    r.constantsUsed = 0;
  }
  parallel_ = 0;
  vertexBound_ = {};
  indexBound_ = {};
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
  if (latency_ && latencyQueries_) {  // (the latency limiter: the frame's first timestamp)
    vkCmdResetQueryPool(f.cmd, latencyQueries_, static_cast<uint32_t>(2 * cur_), 2);
    vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, latencyQueries_, static_cast<uint32_t>(2 * cur_));
  }
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
  if (generating_)  // (the slot's two pictures, copies' sources at its last frame's end, readable again)
    for (Texture t : {windowPictures_[cur_], betweenPictures_[cur_]}) Transition(Find(t), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  PassDesc window;
  window.window = true;
  std::memcpy(window.clearValue, clearColor, sizeof window.clearValue);
  BeginPass(window);
}

void VulkanDevice::BeginPass(const PassDesc& pass) {
  if (!inFrame_) return;
  if (pass.window && viewColor_) {  // [views] a view's own picture in place of the window (SetViewWindow)
    BeginPass(ViewWindowPass(pass));
    return;
  }
  EndRendering();
  ReleaseGeneral();  // (what dispatches wrote, readable by the pass's draws)
  VkCommandBuffer cmd = frames_[cur_].cmd;
  VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
  int width = width_, height = height_;
  if (pass.window && PhotoNow()) {  // (photomode) a photo's frame: its picture and depth in place of the window's
    TextureRec* c = Find(photoColor_);
    TextureRec* d = Find(photoDepth_);
    if (passColor_ != photoColor_.id || passDepth_ != photoDepth_.id) ReleasePassTargets();
    for (uint32_t& bound : boundTextures_)  // (a target is not read while it is drawn into)
      if (bound && (bound == photoColor_.id || bound == photoDepth_.id)) {
        bound = 0;
        setDirty_ = true;
      }
    Transition(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Transition(d, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    colorView = c->attachView;
    depthView = d->attachView;
    width = photoWidth_;
    height = photoHeight_;
    passColor_ = photoColor_.id;
    passDepth_ = photoDepth_.id;
  } else if (pass.window) {
    TextureRec* picture = generating_ ? Find(windowPictures_[cur_]) : nullptr;
    if (picture) {  // frame generation: the slot's window picture, as a target (not read while drawn into)
      const uint32_t id = windowPictures_[cur_].id;
      if (passColor_ != id || passDepth_) ReleasePassTargets();
      for (uint32_t& bound : boundTextures_)
        if (bound == id) {
          bound = 0;
          setDirty_ = true;
        }
      Transition(picture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      passColor_ = id;
      passDepth_ = 0;
      colorView = picture->attachView;
    } else {
      ReleasePassTargets();
      if (back_.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        Barrier(cmd, back_.image, back_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        back_.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      }
      colorView = back_.view;
    }
    if (depth_.layout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
      Barrier(cmd, depth_.image, depth_.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
      depth_.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
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
void VulkanDevice::StartRendering(bool withDepth, bool secondary) {
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
  if (secondary) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;  // [mcrec] (the recorders' command buffers alone)
  vkCmdBeginRendering(cmd, &ri);
  rendering_ = true;
  renderingDepth_ = withDepth;
  passStarted_ = true;
  if (secondary) return;  // (each recorder sets its own viewport)
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
  if (rec && rec->pending) {  // (asked for in a batch and wanted before its end: made now)
    std::string why;
    if (!Realize(pipeline.id - 1, &why) && batchError_.empty()) batchError_ = why;
    rec = Find(pipeline);
  }
  if (rec && rec->background) {  // [snowhitch] (made ahead on the device's own thread: taken now)
    TakeBackground(pipeline.id - 1);
    rec = Find(pipeline);
  }
  if (!rec || !rec->pipeline || !inFrame_) return;
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
  vertexBound_ = {rec->buffer, rec->offset};  // [mcrec]
}

void VulkanDevice::SetIndexBuffer(Buffer buffer) {
  BufferRec* rec = Find(buffer);
  if (!rec || rec->kind != BufferKind::Index || (rec->dynamic && rec->frame != frameNumber_) || !inFrame_) return;
  vkCmdBindIndexBuffer(frames_[cur_].cmd, rec->buffer, rec->offset, rec->stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
  indexBound_ = {rec->buffer, rec->offset, rec->stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32};  // [mcrec]
}

void VulkanDevice::SetShaderBuffer(Buffer buffer, int slot) {
  BufferRec* rec = Find(buffer);
  if (!rec || slot < 0 || slot > 1) return;
  if (rec->dynamic) {  // a Shader buffer's contents of this frame
    if (rec->kind != BufferKind::Shader || rec->frame != frameNumber_) return;
    boundBuffers_[slot] = {rec->buffer, rec->offset, rec->size};
  } else {  // a static buffer, whole (its own buffer, made a whole number of words long: CreateBuffer)
    if (!rec->buffer) return;
    boundBuffers_[slot] = {rec->buffer, 0, (static_cast<VkDeviceSize>(rec->size) + 3) & ~VkDeviceSize{3}};
  }
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
  // [gpuscenery] a card buffer in a shader buffer's slot may be written: what read or wrote it before (draws of a pass, a
  // copy) done first, and what comes after waits for this dispatch (below)
  const bool cards = CardBound(0) || CardBound(1);
  if (cards && !dispatched_)
    CardBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
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
  if (cards) cardWritten_ = true;  // [gpuscenery]
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

void VulkanDevice::BuildScene(Structure* scene, const SceneInstance* instances, uint32_t count) { MakeScene(scene, instances, count, 0); }

// [cardcost] (gfx.h) Built to be refitted, and refitted in place where it was so built over as many instances as now.
bool VulkanDevice::RefitScene(Structure* scene, const SceneInstance* instances, uint32_t count, bool rebuild) {
  return MakeScene(scene, instances, count, rebuild ? 1 : 2);
}

// The scene's structure over the instances: `how` 0 built (BuildScene), 1 built so that it can be refitted, 2 refitted in
// place where it was so built over as many instances (else built so). True where it was made (built or refitted).
bool VulkanDevice::MakeScene(Structure* scene, const SceneInstance* instances, uint32_t count, int how) {
  if (!rayTracing_ || !inFrame_ || !scene) return false;
  EndRendering();
  ReleasePassTargets();  // (the pass under way ends)
  BuildGeometry();
  // the instances, in this frame's memory
  const Span s = Allocate(sizeof(VkAccelerationStructureInstanceKHR) * std::max<uint32_t>(count, 1), 16);
  if (!s.cpu) return false;
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
  if (how > 0) in.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;  // [cardcost]
  in.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  in.geometryCount = 1;
  in.pGeometries = &geometry;
  VkAccelerationStructureBuildSizesInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &in, &n, &info);
  StructureRec* rec = Find(*scene);
  constexpr VkPipelineStageFlags2 build = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  // [cardcost] refitted in place: the structure as it was built (to be refitted, over as many instances), its tree kept
  if (how == 2 && rec && rec->scene && rec->structure && rec->refitCount == n &&
      GrowScratch(Aligned(std::max<VkDeviceSize>(info.updateScratchSize, 1), scratchAlign_))) {
    RayBarrier(kRayShaders, 0, build, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    in.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    in.srcAccelerationStructure = in.dstAccelerationStructure = rec->structure;
    in.scratchData.deviceAddress = scratchAddress_;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = n;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(frames_[cur_].cmd, 1, &in, &ranges);
    RayBarrier(build, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, build | kRayShaders,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    return true;
  }
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
  if (!rec->scene) return false;
  rec->refitCount = ~uint64_t{0};  // [cardcost] (built now: refitted later only where built so, below)
  const VkDeviceSize need = Aligned(std::max<VkDeviceSize>(info.accelerationStructureSize, 256), 256);
  if (rec->size < need) {  // (made, or grown by a half more than it needs: a structure anew, which sets name anew)
    RetireStructure(rec);
    if (!MakeStructure(rec, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, Aligned(need + need / 2, 256))) return false;
    setDirty_ = true;
  }
  if (!GrowScratch(Aligned(std::max<VkDeviceSize>(info.buildScratchSize, 1), scratchAlign_))) {
    RetireStructure(rec);  // (not built: none, rather than one a shader must not read)
    setDirty_ = true;
    return false;
  }
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
  if (how > 0) rec->refitCount = n;  // [cardcost]
  return true;
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
// [mcrec] (PrepareDraw's, and a recorder's: the set the draws read of what is bound, made the first time from `allocate`)
VkDescriptorSet VulkanDevice::MakeDrawSet(const Layout* layout, const uint32_t* textures, const BoundBuffer* bound,
                                          const std::function<VkDescriptorSet(VkDescriptorSetLayout)>& allocate,
                                          std::unordered_map<std::string, VkDescriptorSet>& sets) {
  // what the set holds, by the layout, the views and the buffers themselves (a handle's number may be
  // used again)
  VkImageView views[kTexSlots];
  for (int i = 0; i < kTexSlots; ++i) views[i] = textures[i] ? textures_[textures[i]].view : VK_NULL_HANDLE;
  if (!nullDescriptors_)
    for (const auto& image : layout->images)
      for (uint32_t k = 0; k < image.count; ++k)
        if (!views[image.binding - kTexBinding + k] && image.kind <= SpirvImage::Volume) views[image.binding - kTexBinding + k] = dummy_[image.kind].view;
  // (and the scenes, in the slots where the shaders declare one: the structure itself, which a scene grown
  // has anew)
  VkAccelerationStructureKHR scenes[kTexSlots] = {};
  for (const auto& image : layout->images)
    if (image.kind == SpirvImage::Scene)
      for (uint32_t k = 0; k < image.count; ++k) scenes[image.binding - kTexBinding + k] = SceneIn(static_cast<int>(image.binding - kTexBinding + k));
  BoundBuffer buffers[2] = {bound[0], bound[1]};
  for (BoundBuffer& b : buffers)
    if (!b.buffer && !nullDescriptors_) b = {dummyBuffer_, 0, 256};
  std::string key(reinterpret_cast<const char*>(&layout), sizeof layout);
  for (const auto& image : layout->images)
    if (image.kind == SpirvImage::Scene) key.append(reinterpret_cast<const char*>(&scenes[image.binding - kTexBinding]), image.count * sizeof(VkAccelerationStructureKHR));
    else key.append(reinterpret_cast<const char*>(&views[image.binding - kTexBinding]), image.count * sizeof(VkImageView));
  for (const BoundBuffer& b : buffers) {
    key.append(reinterpret_cast<const char*>(&b.buffer), sizeof b.buffer);
    key.append(reinterpret_cast<const char*>(&b.offset), sizeof b.offset);
    key.append(reinterpret_cast<const char*>(&b.size), sizeof b.size);
  }
  VkDescriptorSet set;
  auto known = sets.find(key);
  if (known != sets.end()) {
    set = known->second;
  } else {
    set = allocate(layout->set);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo images[kTexSlots];
    VkDescriptorBufferInfo infos[2];
    VkWriteDescriptorSet writes[kTexSlots + 2];
    VkWriteDescriptorSetAccelerationStructureKHR sceneWrites[kTexSlots];
    uint32_t n = 0;
    for (const auto& image : layout->images) {  // (an array's elements written together)
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
    sets.emplace(std::move(key), set);
  }
  return set;
}

bool VulkanDevice::PrepareDraw() {
  if (!inFrame_ || !rendering_ || !pipeline_ || !layout_) return false;
  Frame& f = frames_[cur_];
  // a pipeline drawn without depth into a pass that has it (or the other way round): Vulkan wants the
  // pass to match, so it is begun again (keeping what is drawn) as the pipeline wants
  if (pipelineDepth_ != renderingDepth_ && passDepthView_) StartRendering(pipelineDepth_);
  VkCommandBuffer cmd = f.cmd;
  if (setDirty_) {
    const VkDescriptorSet set = MakeDrawSet(layout_, boundTextures_, boundBuffers_, [&](VkDescriptorSetLayout setLayout) {
      VkDescriptorSet made = VK_NULL_HANDLE;
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = f.descriptors;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &setLayout;
      return vkAllocateDescriptorSets(device_, &ai, &made) == VK_SUCCESS ? made : VK_NULL_HANDLE;
    }, f.sets);  // [mcrec] (MakeDrawSet: the set's making shared with the recorders)
    if (!set) {
      static bool told = false;
      if (!told) fprintf(stderr, "render: Vulkan: out of descriptor sets this frame\n");
      told = true;
      return false;
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline, 0, 1, &set, 0, nullptr);
    setDirty_ = false;
  }
  if (f.constantPage >= f.constantSets.size()) return false;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_->pipeline, 1, 1, &f.constantSets[f.constantPage], kCbSlots, constantOffsets_);
  return true;
}

// [mcrec] A set from a recorder's own descriptor pools: another pool made when the last is full.
VkDescriptorSet VulkanDevice::RecorderSet(RecorderRoom& room, VkDescriptorSetLayout layout) {
  for (;;) {
    if (room.descriptorsUsed < room.descriptors.size()) {
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = room.descriptors[room.descriptorsUsed];
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &layout;
      if (vkAllocateDescriptorSets(device_, &ai, &set) == VK_SUCCESS) return set;
      ++room.descriptorsUsed;  // (full: the next)
      continue;
    }
    // (as the frame's own pool, a quarter of its size; the fixed samplers count against it too)
    constexpr uint32_t kSets = 2048;
    VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * kTexSlots},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets * 2},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kSets * 6},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64 * kCbSlots},
                                     {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSets}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSets + 64;
    dpi.poolSizeCount = rayTracing_ ? 5 : 4;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    room.descriptors.push_back(pool);
  }
}

// [mcrec] A recorder's next page of constants (as NextConstantPage, in its own room, its pages smaller), what its slots were
// last given copied onto it.
bool VulkanDevice::NextRecorderPage(RecorderRoom& room, ParallelRecorder& r) {
  const size_t next = room.constantPage == SIZE_MAX ? 0 : room.constantPage + 1;
  const VkDeviceSize range = std::min<VkDeviceSize>(kUniformRange, props_.limits.maxUniformBufferRange);
  if (next >= room.constantPages.size()) {
    Page p;
    p.size = kRecorderConstantPage;
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
    room.constantPages.push_back(p);
  }
  while (room.constantSets.size() <= next) {
    const VkDescriptorSet set = RecorderSet(room, constantsLayout_);
    if (!set) return false;
    const Page& p = room.constantPages[room.constantSets.size()];
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
    room.constantSets.push_back(set);
  }
  room.constantPage = next;
  room.constantsUsed = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(256, props_.limits.minUniformBufferOffsetAlignment);
  const Page& p = room.constantPages[next];
  for (int slot = 0; slot < kCbSlots; ++slot) {
    r.constantOffsets_[slot] = 0;
    if (r.constants_[slot].empty()) continue;
    const VkDeviceSize at = (room.constantsUsed + align - 1) / align * align;
    std::memcpy(p.memory.mapped + at, r.constants_[slot].data(), r.constants_[slot].size());
    room.constantsUsed = at + r.constants_[slot].size();
    r.constantOffsets_[slot] = static_cast<uint32_t>(at);
  }
  return true;
}

// [mcrec] A recorder's SetConstants (as the device's own, in its own room).
void VulkanDevice::RecorderConstants(RecorderRoom& room, ParallelRecorder& r, int slot, const void* data, uint32_t size) {
  if (slot < 0 || slot >= kCbSlots || !size) return;
  const VkDeviceSize range = std::min<VkDeviceSize>(kUniformRange, props_.limits.maxUniformBufferRange);
  if (size > range) return;
  const VkDeviceSize align = std::max<VkDeviceSize>(256, props_.limits.minUniformBufferOffsetAlignment);
  VkDeviceSize at = (room.constantsUsed + align - 1) / align * align;
  if (room.constantPage >= room.constantPages.size() || at + range > kRecorderConstantPage) {  // (none yet, or full: the next)
    r.constants_[slot].clear();
    if (!NextRecorderPage(room, r)) return;
    at = (room.constantsUsed + align - 1) / align * align;
  }
  std::memcpy(room.constantPages[room.constantPage].memory.mapped + at, data, size);
  room.constantsUsed = at + size;
  r.constantOffsets_[slot] = static_cast<uint32_t>(at);
  r.constants_[slot].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
}

// [mcrec] A recorder's PrepareDraw: its set of what is bound, and its constants' set at their offsets. (Its drawing cannot be
// begun again without the pass's depth, as the frame's own can: a pipeline drawn without depth into a pass with it is
// passed over, and said once.)
bool VulkanDevice::RecorderPrepare(ParallelRecorder& r) {
  if (!r.pipeline_ || !r.layout_) return false;
  if (r.pipelineDepth_ != parallelDepth_ && passDepthView_) {
    if (!warnedParallel_.exchange(true))
      fprintf(stderr, "render: Vulkan: a recorder's pipeline drawn %s the pass's depth: passed over\n", r.pipelineDepth_ ? "with" : "without");
    return false;
  }
  RecorderRoom& room = frames_[cur_].recorders[static_cast<size_t>(r.index_)];
  if (r.setDirty_) {
    const VkDescriptorSet set = MakeDrawSet(r.layout_, r.boundTextures_, r.boundBuffers_,
                                            [&](VkDescriptorSetLayout setLayout) { return RecorderSet(room, setLayout); }, room.sets);
    if (!set) {
      static std::atomic<bool> told{false};
      if (!told.exchange(true)) fprintf(stderr, "render: Vulkan: a recorder is out of descriptor sets\n");
      return false;
    }
    vkCmdBindDescriptorSets(r.cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout_->pipeline, 0, 1, &set, 0, nullptr);
    r.setDirty_ = false;
  }
  if (room.constantPage >= room.constantSets.size()) return false;
  vkCmdBindDescriptorSets(r.cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout_->pipeline, 1, 1, &room.constantSets[room.constantPage], kCbSlots,
                          r.constantOffsets_);
  return true;
}

bool VulkanDevice::BeginParallel(int count) {
  if (!inFrame_ || parallel_ || count < 1 || count > kMostRecorders || (!passColorView_ && !passDepthView_)) return false;
  Frame& f = frames_[cur_];
  if (f.recorders.size() < static_cast<size_t>(count)) f.recorders.resize(static_cast<size_t>(count));
  while (recorders_.size() < static_cast<size_t>(count)) recorders_.push_back(std::make_unique<ParallelRecorder>(this, static_cast<int>(recorders_.size())));
  // each its secondary command buffer (none handed out when one cannot be made)
  VkCommandBuffer cmds[kMostRecorders];
  for (int i = 0; i < count; ++i) {
    RecorderRoom& room = f.recorders[static_cast<size_t>(i)];
    if (!room.pool) {
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pci.queueFamilyIndex = queueFamily_;
      if (vkCreateCommandPool(device_, &pci, nullptr, &room.pool) != VK_SUCCESS) return false;
    }
    if (room.cmdsUsed == room.cmds.size()) {
      VkCommandBuffer cmd = VK_NULL_HANDLE;
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = room.pool;
      cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
      cai.commandBufferCount = 1;
      if (vkAllocateCommandBuffers(device_, &cai, &cmd) != VK_SUCCESS) return false;
      room.cmds.push_back(cmd);
    }
    cmds[i] = room.cmds[room.cmdsUsed];
  }
  // what they draw into: the pass's attachments, as the drawing EndParallel begins for them has them
  parallelDepth_ = passDepthView_ != VK_NULL_HANDLE;
  const VkFormat colorFormat = !passColorView_ ? VK_FORMAT_UNDEFINED : passColor_ && passColor_ < textures_.size() ? textures_[passColor_].format : swapFormat_;
  const int samples = passColor_ && passColor_ < textures_.size() ? textures_[passColor_].samples
                      : passDepth_ && passDepth_ < textures_.size() ? textures_[passDepth_].samples : 1;
  VkCommandBufferInheritanceRenderingInfo rendering{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
  rendering.colorAttachmentCount = passColorView_ ? 1 : 0;
  rendering.pColorAttachmentFormats = &colorFormat;
  rendering.depthAttachmentFormat = parallelDepth_ ? kDepthFormat : VK_FORMAT_UNDEFINED;
  rendering.rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples);
  VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  inheritance.pNext = &rendering;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bi.pInheritanceInfo = &inheritance;
  const VkViewport vp{0, 0, static_cast<float>(passWidth_), static_cast<float>(passHeight_), 0, 1};
  const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(passWidth_), static_cast<uint32_t>(passHeight_)}};
  for (int i = 0; i < count; ++i) {
    RecorderRoom& room = f.recorders[static_cast<size_t>(i)];
    ParallelRecorder& r = *recorders_[static_cast<size_t>(i)];
    ++room.cmdsUsed;
    r.cmd_ = cmds[i];
    vkBeginCommandBuffer(r.cmd_, &bi);
    vkCmdSetViewport(r.cmd_, 0, 1, &vp);
    vkCmdSetScissor(r.cmd_, 0, 1, &sc);
    // (set up as the device is now)
    std::copy(std::begin(boundTextures_), std::end(boundTextures_), r.boundTextures_);
    r.boundBuffers_[0] = boundBuffers_[0];
    r.boundBuffers_[1] = boundBuffers_[1];
    r.pipeline_ = pipeline_;
    r.layout_ = layout_;
    r.pipelineDepth_ = pipelineDepth_;
    r.setDirty_ = true;
    r.draws_ = 0;
    if (pipeline_) vkCmdBindPipeline(r.cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    r.vertexBound_ = vertexBound_;
    r.indexBound_ = indexBound_;
    if (vertexBound_.buffer) vkCmdBindVertexBuffers(r.cmd_, 0, 1, &vertexBound_.buffer, &vertexBound_.offset);
    if (indexBound_.buffer) vkCmdBindIndexBuffer(r.cmd_, indexBound_.buffer, indexBound_.offset, indexBound_.type);
    // (the constants as the slots were last given, onto its own page)
    for (int slot = 0; slot < kCbSlots; ++slot) r.constants_[slot] = constants_[slot];
    if (room.constantPage >= room.constantPages.size()) {
      NextRecorderPage(room, r);
    } else {
      for (int slot = 0; slot < kCbSlots; ++slot) {
        if (r.constants_[slot].empty()) continue;
        const std::vector<uint8_t> given = r.constants_[slot];
        RecorderConstants(room, r, slot, given.data(), static_cast<uint32_t>(given.size()));
      }
    }
  }
  parallel_ = count;
  return true;
}

Recorder* VulkanDevice::Parallel(int index) {
  return index >= 0 && index < parallel_ ? recorders_[static_cast<size_t>(index)].get() : nullptr;
}

void VulkanDevice::EndParallel() {
  if (!parallel_) return;
  Frame& f = frames_[cur_];
  VkCommandBuffer cmds[kMostRecorders];
  for (int i = 0; i < parallel_; ++i) {
    ParallelRecorder& r = *recorders_[static_cast<size_t>(i)];
    vkEndCommandBuffer(r.cmd_);
    cmds[i] = r.cmd_;
    draws_ += r.draws_;
  }
  // a drawing into the pass's attachments of their own (its clears, if the pass had not begun yet), then the frame's own again
  EndRendering();
  StartRendering(parallelDepth_, true);
  vkCmdExecuteCommands(f.cmd, static_cast<uint32_t>(parallel_), cmds);
  EndRendering();
  // the device set as the last recorder left it, bound again on the frame's own command buffer (a secondary's leaves nothing)
  ParallelRecorder& last = *recorders_[static_cast<size_t>(parallel_ - 1)];
  std::copy(std::begin(last.boundTextures_), std::end(last.boundTextures_), boundTextures_);
  boundBuffers_[0] = last.boundBuffers_[0];
  boundBuffers_[1] = last.boundBuffers_[1];
  pipeline_ = last.pipeline_;
  layout_ = last.layout_;
  pipelineDepth_ = last.pipelineDepth_;
  vertexBound_ = last.vertexBound_;
  indexBound_ = last.indexBound_;
  setDirty_ = true;
  parallel_ = 0;
  StartRendering(parallelDepth_);
  if (pipeline_) vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  if (computePipeline_) vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
  if (vertexBound_.buffer) vkCmdBindVertexBuffers(f.cmd, 0, 1, &vertexBound_.buffer, &vertexBound_.offset);
  if (indexBound_.buffer) vkCmdBindIndexBuffer(f.cmd, indexBound_.buffer, indexBound_.offset, indexBound_.type);
  for (int slot = 0; slot < kCbSlots; ++slot) {
    if (last.constants_[slot].empty()) continue;
    const std::vector<uint8_t> given = last.constants_[slot];
    SetConstants(slot, given.data(), static_cast<uint32_t>(given.size()));
  }
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

bool VulkanDevice::EnsureReadback(VkDeviceSize need) {
  if (need == 0) need = static_cast<VkDeviceSize>(width_) * static_cast<VkDeviceSize>(height_) * 4;
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
  return readback_ != VK_NULL_HANDLE;
}

void VulkanDevice::EndFrame(std::vector<uint8_t>* capture, bool present) {
  if (!inFrame_) return;
  drawsLast_ = draws_;
  draws_ = 0;
  Frame& f = frames_[cur_];
  EndRendering();
  ReleasePassTargets();
  ReleaseGeneral();
  const bool later = captureLater_;  // [weather]
  captureLater_ = false;
  if (capture) laterWaiting_ = false;  // ([weather] the read-back is this one's now)
  if (PhotoNow()) {  // (photomode) a photo's frame: read back, never shown
    EndFramePhoto(capture, later);
    return;
  }
  if (generating_ && Find(windowPictures_[cur_]) && Find(betweenPictures_[cur_])) {
    EndFrameGenerating(capture, present);
    return;
  }
  VkCommandBuffer cmd = f.cmd;
  if (queries_ && !f.marks.empty()) {  // the frame's end
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, static_cast<uint32_t>(cur_) * kMarks + static_cast<uint32_t>(f.marks.size()));
    f.marks.push_back("(end)");
  }
  // our picture onto the swap chain's image (a copy that also turns RGBA into the chain's order)
  Barrier(cmd, back_.image, back_.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  back_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (capture) {
    EnsureReadback();
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
  if (latency_ && latencyQueries_) {  // (the latency limiter: the frame's last timestamp)
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, latencyQueries_, static_cast<uint32_t>(2 * cur_ + 1));
    latencySlots_[cur_].timed = true;
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
  if (latency_ || measureLatency) latencySlots_[cur_].handed = Now(), latencySlots_[cur_].order = ++handedCount_;
  if (show) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[swapIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &swapIndex_;
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};  // (the latency limiter's and the measure's)
    uint64_t id = 0;
    if (PresentIds()) {
      id = presentIdNext_++;
      pid.swapchainCount = 1;
      pid.pPresentIds = &id;
      pi.pNext = &pid;
    }
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r != VK_SUCCESS) chainStale_ = true;  // (out of date or suboptimal: a new chain before the next frame)
    imageHeld_ = false;
    if (id) {
      lastPresentId_ = id;
      if (measureLatency) Watch(id, latencySlots_[cur_].read);
    }
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
