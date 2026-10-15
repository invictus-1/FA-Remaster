# The Vulkan backend

Chris, 2026-10-15: after the optimisation, Vulkan, for Linux and macOS ("I don't have linux so we just have
to try and make it as good as possible same with mac os on m1"). `render/gfx_vulkan.cpp` implements the
same `gfx::Device` as `render/gfx_d3d12.cpp`; `gfx::DeviceDesc::backend` chooses (viewer: `--vulkan`). It is
written and tried on Windows, where Vulkan runs too, and compared with Direct3D 12 picture for picture.
Linux and macOS cannot be run here: their parts are written with care and said to be untried.

## Steps

1. **Done**: the instance and device (Vulkan 1.3: dynamic rendering, synchronization2; push descriptors),
   the window's swap chain, a frame cleared, shown and read back (`--vulkan-clear`). The window's picture is
   drawn into an RGBA8 image of our own with its depth (as Direct3D 12's back buffer) and blitted onto the
   swap chain's image at the frame's end.
2. Buffers (static, uploaded in batches; dynamic, in a per-frame ring), textures (all formats, cube maps,
   arrays, mips), targets (colour, depth, layers, samples), passes into them, Resolve.
3. Pipelines: HLSL compiled to SPIR-V by DXC (`dxcompiler.dll` loaded at run time), the registers mapped to
   one descriptor set (b0-b3 uniform buffers, t0-t31 images, t0-t1 in space1 storage buffers, s0-s5 fixed
   samplers), pushed per draw; vertex inputs by their HLSL semantics; constants, textures, draws.
4. Timestamps (`Mark`/`TakeTimes`), `UpdateTexture`, the whole scene against Direct3D 12.
5. Linux (a window and a surface: X11 / Wayland) and macOS (MoltenVK, Apple Silicon).

## Third-party parts

- `render/third_party/vulkan`: Khronos Vulkan-Headers 1.3.296, C headers only (Apache-2.0 or MIT).
- `render/third_party/dxc`: `dxcapi.h`, `dxcerrors.h` from Microsoft's DirectX Shader Compiler (University
  of Illinois/NCSA licence, LICENSE-LLVM.txt). The compiler itself (`dxcompiler.dll`) is not in the
  repository: the Windows build of it comes under Microsoft's licence "solely for use on Windows", fine for
  trying Vulkan on Windows; the Linux and macOS builds would ship DXC's LLVM-licensed library. The viewer
  finds it beside itself.
