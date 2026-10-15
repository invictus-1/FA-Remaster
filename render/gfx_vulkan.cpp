// The Vulkan backend of gfx.h: the same calls as gfx_d3d12.cpp, for systems other than Windows (Linux,
// macOS through MoltenVK) and tried on Windows against Direct3D 12, picture for picture. Chris, 2026-10-15:
// after the optimisation, Vulkan next; "I don't have linux so we just have to try and make it as good as
// possible same with mac os on m1".
//
// Built in steps (docs/render-vulkan.md): this file is the first - the instance, the device, the window's
// swap chain, and a frame that is cleared and shown. Nothing else is drawn by it yet.
//
// The window's picture is drawn into an image of our own (RGBA8, with its depth buffer, as Direct3D 12's
// back buffer is) and copied onto the swap chain's image at the frame's end: pipelines then need not know
// the swap chain's format, and a capture reads our own image.
//
// Vulkan's functions are fetched from the loader (vulkan-1.dll, libvulkan.so.1, libMoltenVK.dylib) at
// run time: nothing to link, and a machine without Vulkan only fails to make this backend.

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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace render::gfx {
namespace {

// --- the functions, fetched at run time ---
#define VK_GLOBAL_FUNCTIONS(X) \
  X(vkCreateInstance)          \
  X(vkEnumerateInstanceExtensionProperties) \
  X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE_FUNCTIONS(X)                     \
  X(vkDestroyInstance)                               \
  X(vkEnumeratePhysicalDevices)                      \
  X(vkGetPhysicalDeviceProperties)                   \
  X(vkGetPhysicalDeviceQueueFamilyProperties)        \
  X(vkGetPhysicalDeviceMemoryProperties)             \
  X(vkGetPhysicalDeviceFeatures2)                    \
  X(vkGetPhysicalDeviceFormatProperties)             \
  X(vkEnumerateDeviceExtensionProperties)            \
  X(vkCreateDevice)                                  \
  X(vkGetDeviceProcAddr)                             \
  X(vkDestroySurfaceKHR)                             \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)            \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)       \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)            \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#if defined(_WIN32)
#define VK_SURFACE_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#else
#define VK_SURFACE_FUNCTIONS(X)
#endif
#define VK_DEVICE_FUNCTIONS(X)        \
  X(vkDestroyDevice)                  \
  X(vkGetDeviceQueue)                 \
  X(vkDeviceWaitIdle)                 \
  X(vkCreateSwapchainKHR)             \
  X(vkDestroySwapchainKHR)            \
  X(vkGetSwapchainImagesKHR)          \
  X(vkAcquireNextImageKHR)            \
  X(vkQueuePresentKHR)                \
  X(vkQueueSubmit)                    \
  X(vkCreateCommandPool)              \
  X(vkDestroyCommandPool)             \
  X(vkAllocateCommandBuffers)         \
  X(vkResetCommandPool)               \
  X(vkBeginCommandBuffer)             \
  X(vkEndCommandBuffer)               \
  X(vkCreateFence)                    \
  X(vkDestroyFence)                   \
  X(vkWaitForFences)                  \
  X(vkResetFences)                    \
  X(vkCreateSemaphore)                \
  X(vkDestroySemaphore)               \
  X(vkCreateImage)                    \
  X(vkDestroyImage)                   \
  X(vkCreateImageView)                \
  X(vkDestroyImageView)               \
  X(vkCreateBuffer)                   \
  X(vkDestroyBuffer)                  \
  X(vkGetImageMemoryRequirements)     \
  X(vkGetBufferMemoryRequirements)    \
  X(vkAllocateMemory)                 \
  X(vkFreeMemory)                     \
  X(vkBindImageMemory)                \
  X(vkBindBufferMemory)               \
  X(vkMapMemory)                      \
  X(vkUnmapMemory)                    \
  X(vkCmdPipelineBarrier2)            \
  X(vkCmdBeginRendering)              \
  X(vkCmdEndRendering)                \
  X(vkCmdSetViewport)                 \
  X(vkCmdSetScissor)                  \
  X(vkCmdBlitImage)                   \
  X(vkCmdCopyImageToBuffer)
#define VK_DECLARE(name) PFN_##name name = nullptr;
VK_GLOBAL_FUNCTIONS(VK_DECLARE)
VK_INSTANCE_FUNCTIONS(VK_DECLARE)
VK_SURFACE_FUNCTIONS(VK_DECLARE)
VK_DEVICE_FUNCTIONS(VK_DECLARE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#undef VK_DECLARE

bool LoadLoader(std::string* error) {
  if (vkGetInstanceProcAddr) return true;
#if defined(_WIN32)
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (lib) vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#elif defined(__APPLE__)
  void* lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
  if (!lib) lib = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
  if (lib) vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#else
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
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
constexpr VkFormat kBackFormat = VK_FORMAT_R8G8B8A8_UNORM;  // our own picture of the window (as Direct3D 12's)
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
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

  // (later steps: buffers, textures, targets, pipelines, drawing)
  Buffer CreateBuffer(const BufferDesc&) override { return {}; }
  void UpdateBuffer(Buffer, const void*, uint32_t) override {}
  Texture CreateTexture(const TextureDesc&) override { return {}; }
  void UpdateTexture(Texture, int, int, int, int, const void*, uint32_t) override {}
  Texture CreateTarget(const TargetDesc&) override { return {}; }
  Pipeline CreatePipeline(const PipelineDesc&, std::string* error) override {
    if (error) *error = "Vulkan: pipelines are not made yet (the backend's first step)";
    return {};
  }
  void Destroy(Buffer) override {}
  void Destroy(Texture) override {}
  void Destroy(Pipeline) override {}
  void BeginFrame(const float clearColor[4]) override;
  void BeginPass(const PassDesc& pass) override;
  void Resolve(Texture, Texture) override {}
  void SetPipeline(Pipeline) override {}
  void SetConstants(int, const void*, uint32_t) override {}
  void SetTexture(int, Texture) override {}
  void SetVertexBuffer(Buffer) override {}
  void SetIndexBuffer(Buffer) override {}
  void SetShaderBuffer(Buffer, int) override {}
  void Draw(uint32_t, uint32_t, uint32_t) override {}
  void DrawIndexed(uint32_t, uint32_t, int32_t, uint32_t) override {}
  void EndFrame(std::vector<uint8_t>* capture, bool present) override;
  void Mark(const char*) override {}
  void TakeTimes(std::vector<std::pair<const char*, double>>* out) override { out->clear(); }

 private:
  bool CreateSwapchain(std::string* error);
  void DestroySwapchain();
  bool MakeImage(Image* image, VkFormat format, int width, int height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
  void FreeImage(Image* image);
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
  void Transition(Image* image, VkImageLayout to, VkImageAspectFlags aspect);
  void EndRendering();

  void* window_ = nullptr;
  int width_ = 0, height_ = 0;
  bool vsync_ = true;
  std::string adapterName_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> swapImages_;
  std::vector<VkImageLayout> swapLayouts_;
  Image back_, depth_;  // the window's picture and its depth (ours, copied onto the swap chain's image)

  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    bool submitted = false;
  };
  Frame frames_[kFrames];
  int cur_ = 0;
  uint32_t swapIndex_ = 0;
  bool holding_ = false;  // the swap chain's image of a frame not shown is kept for the next
  bool waitAcquired_ = false;  // this frame's commands wait for the image it acquired
  bool inFrame_ = false, rendering_ = false;
  // read-back (EndFrame with a capture)
  VkBuffer readback_ = VK_NULL_HANDLE;
  VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
  VkDeviceSize readbackSize_ = 0;
};

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
    if (readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
    DestroySwapchain();
    for (Frame& f : frames_) {
      if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
      if (f.done) vkDestroyFence(device_, f.done, nullptr);
      if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
      if (f.rendered) vkDestroySemaphore(device_, f.rendered, nullptr);
    }
    vkDestroyDevice(device_, nullptr);
  }
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

  // the instance: the window's surface, and (with debug) the validation layer when installed
  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(_WIN32)
  extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
  std::vector<const char*> layers;
  if (desc.debug) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> have(n);
    vkEnumerateInstanceLayerProperties(&n, have.data());
    for (const VkLayerProperties& l : have)
      if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
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
  VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
  if (r != VK_SUCCESS) return fail("creating the instance", r);
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance_, #name));
  VK_INSTANCE_FUNCTIONS(VK_LOAD)
  VK_SURFACE_FUNCTIONS(VK_LOAD)
#undef VK_LOAD

  // the window's surface
#if defined(_WIN32)
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = GetModuleHandleW(nullptr);
  sci.hwnd = static_cast<HWND>(window_);
  r = vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_);
  if (r != VK_SUCCESS) return fail("the window's surface", r);
#else
  if (error) *error = "Vulkan: no window surface for this system yet";
  return false;
#endif

  // the card: a discrete one first, with a queue that draws and shows on the surface
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> cards(count);
  vkEnumeratePhysicalDevices(instance_, &count, cards.data());
  int bestScore = -1;
  for (VkPhysicalDevice card : cards) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(card, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) continue;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(card, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(card, &families, props.data());
    for (uint32_t i = 0; i < families; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(card, i, surface_, &present);
      if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
      if (score > bestScore) {
        bestScore = score;
        physical_ = card;
        queueFamily_ = i;
        adapterName_ = p.deviceName;
      }
      break;
    }
  }
  if (!physical_) {
    if (error) *error = "Vulkan: no card with Vulkan 1.3 that can draw into this window";
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  // the device: Vulkan 1.3's dynamic rendering and second synchronization, push descriptors, and
  // anisotropic filtering
  float priority = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &f13;
  features.features.samplerAnisotropy = VK_TRUE;
  features.features.depthBiasClamp = VK_TRUE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &features;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 2;
  dci.ppEnabledExtensionNames = deviceExtensions;
  r = vkCreateDevice(physical_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) return fail("creating the device", r);
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
  VK_DEVICE_FUNCTIONS(VK_LOAD)
#undef VK_LOAD
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

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
    if ((r = vkCreateSemaphore(device_, &smi, nullptr, &f.rendered)) != VK_SUCCESS) return fail("a semaphore", r);
  }
  return CreateSwapchain(error);
}

uint32_t VulkanDevice::MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

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
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ii, nullptr, &image->image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, image->image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &image->memory) != VK_SUCCESS) return false;
  vkBindImageMemory(device_, image->image, image->memory, 0);
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
  if (image->memory) vkFreeMemory(device_, image->memory, nullptr);
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
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)};
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent;
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
  // our picture of the window and its depth, at the size asked for (blitted onto the swap chain's)
  if (!MakeImage(&back_, kBackFormat, width_, height_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT) ||
      !MakeImage(&depth_, kDepthFormat, width_, height_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
    if (error) *error = "Vulkan: the window's picture";
    return false;
  }
  return true;
}

void VulkanDevice::DestroySwapchain() {
  holding_ = false;
  FreeImage(&back_);
  FreeImage(&depth_);
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  swapImages_.clear();
  swapLayouts_.clear();
}

void VulkanDevice::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  width_ = width;
  height_ = height;
  std::string error;
  if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
}

void VulkanDevice::SetVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  vkDeviceWaitIdle(device_);
  DestroySwapchain();
  std::string error;
  if (!CreateSwapchain(&error)) fprintf(stderr, "render: %s\n", error.c_str());
}

// An image into a layout (all of it), with what waits for what as wide as it needs to be.
void VulkanDevice::Transition(Image* image, VkImageLayout to, VkImageAspectFlags aspect) {
  if (image->layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = image->layout;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image->image;
  b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(frames_[cur_].cmd, &di);
  image->layout = to;
}

void VulkanDevice::EndRendering() {
  if (!rendering_) return;
  vkCmdEndRendering(frames_[cur_].cmd);
  rendering_ = false;
}

void VulkanDevice::BeginFrame(const float clearColor[4]) {
  Frame& f = frames_[cur_];
  vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
  waitAcquired_ = false;
  if (!holding_) {
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &swapIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {  // (the window changed under us: a new chain, then again)
      vkDeviceWaitIdle(device_);
      DestroySwapchain();
      std::string error;
      CreateSwapchain(&error);
      r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &swapIndex_);
    }
    waitAcquired_ = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
  }
  vkResetFences(device_, 1, &f.done);
  vkResetCommandPool(device_, f.pool, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cmd, &bi);
  inFrame_ = true;
  PassDesc window;
  window.window = true;
  std::memcpy(window.clearValue, clearColor, sizeof window.clearValue);
  BeginPass(window);
}

void VulkanDevice::BeginPass(const PassDesc& pass) {
  if (!inFrame_) return;
  EndRendering();
  VkCommandBuffer cmd = frames_[cur_].cmd;
  if (!pass.window) return;  // (targets: a later step)
  Transition(&back_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Transition(&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = back_.view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  std::memcpy(color.clearValue.color.float32, pass.clearValue, sizeof pass.clearValue);
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = pass.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil.depth = 0;  // (reversed depth: far is 0, as in Direct3D 12's)
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &ri);
  rendering_ = true;
  VkViewport vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
}

void VulkanDevice::EndFrame(std::vector<uint8_t>* capture, bool present) {
  if (!inFrame_) return;
  Frame& f = frames_[cur_];
  EndRendering();
  VkCommandBuffer cmd = f.cmd;
  // our picture onto the swap chain's image (a copy that also turns RGBA into the chain's order)
  Transition(&back_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  if (capture) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(width_) * static_cast<VkDeviceSize>(height_) * 4;
    if (readbackSize_ < need) {
      if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
      if (readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
      VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bci.size = need;
      bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      vkCreateBuffer(device_, &bci, nullptr, &readback_);
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device_, readback_, &req);
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      vkAllocateMemory(device_, &mai, nullptr, &readbackMemory_);
      vkBindBufferMemory(device_, readback_, readbackMemory_, 0);
      readbackSize_ = need;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyImageToBuffer(cmd, back_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
  }
  if (present) {
    Image swap{swapImages_[swapIndex_], VK_NULL_HANDLE, VK_NULL_HANDLE, swapLayouts_[swapIndex_]};
    Transition(&swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {width_, height_, 1};
    blit.dstOffsets[1] = {width_, height_, 1};
    vkCmdBlitImage(cmd, back_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Transition(&swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    swapLayouts_[swapIndex_] = swap.layout;
  }
  vkEndCommandBuffer(cmd);
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = waitAcquired_ ? 1 : 0;  // (an image kept from a frame not shown was waited for then)
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = present ? 1 : 0;
  si.pSignalSemaphores = &f.rendered;
  vkQueueSubmit(queue_, 1, &si, f.done);
  if (present) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &swapIndex_;
    vkQueuePresentKHR(queue_, &pi);
  }
  holding_ = !present;
  inFrame_ = false;
  if (capture) {
    vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
    capture->resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
    void* mapped = nullptr;
    if (vkMapMemory(device_, readbackMemory_, 0, readbackSize_, 0, &mapped) == VK_SUCCESS) {
      std::memcpy(capture->data(), mapped, capture->size());
      vkUnmapMemory(device_, readbackMemory_);
    }
  }
  cur_ = (cur_ + 1) % kFrames;
}

}  // namespace

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string* error) {
  auto d = std::make_unique<VulkanDevice>();
  if (!d->Init(desc, error)) return nullptr;
  return d;
}

}  // namespace render::gfx
