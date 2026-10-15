The Khronos Vulkan headers 1.3.296 (https://github.com/KhronosGroup/Vulkan-Headers, tag v1.3.296), the C headers only.
Apache-2.0 OR MIT: LICENSE.md, LICENSES/. One change: include/vk_video/ moved to include/vulkan/vk_video/, so that
vulkan_core.h finds it beside itself without an include path (the renderer's builds add none).
