# The Vulkan backend

Chris, 2026-10-15: after the optimisation, Vulkan, for Linux and macOS ("I don't have linux so we just have
to try and make it as good as possible same with mac os on m1"). `render/gfx_vulkan.cpp` implements the
same `gfx::Device` as `render/gfx_d3d12.cpp`; `gfx::DeviceDesc::backend` chooses (viewer: `--vulkan`;
`WorldPicture::SetVulkan` before `Open` for a host). It is written and tried on Windows, where Vulkan runs
too, and compared with Direct3D 12 picture for picture. Linux and macOS cannot be run here: their parts are
written with care and said to be untried.

## Steps

1. **Done**: the instance and device (Vulkan 1.3: dynamic rendering, synchronization2), the window's swap
   chain, a frame cleared, shown and read back (`--vulkan-clear`). The window's picture is drawn into an
   RGBA8 image of our own with its depth (as Direct3D 12's back buffer) and blitted onto the swap chain's
   image at the frame's end.
2. **Done**: buffers (static, uploaded in batches sent ahead of the frame; dynamic, in per-frame pages),
   textures (all formats, cube maps, arrays, mips; A8 read as (0, 0, 0, a)), targets (colour, depth, layers,
   samples), passes into them, Resolve. Memory from 256 MB blocks shared out. Destroyed resources let go of
   once the frames that might use them are done.
3. **Done**: pipelines. HLSL compiled to SPIR-V by DXC (`dxcompiler.dll` loaded at run time,
   `render/gfx_vulkan_dxc.cpp`), kept in the shader cache folder as `.spv`. Registers mapped: set 0 holds
   t0-t31 (bindings 16-47), t0-t1 of space1 (storage buffers, 48-49) and the six fixed samplers (64-69);
   set 1 holds b0-b3 (dynamic uniform buffers, 0-3) pointing into the frame's pages of constants. Set 0's
   layout is made per arrangement of textures the shaders declare (read from the SPIR-V: an array of
   textures, `Texture2D maps[10] : register(t6)`, is one binding of ten in Vulkan); its sets are cached per
   frame by what is bound. Vertex inputs found by their HLSL semantics (DXC's `-fspv-reflect`
   decorations, stripped again before the driver sees them).
4. **Done**: timestamps (`Mark`/`TakeTimes`), `UpdateTexture`, the whole scene against Direct3D 12, the
   direct window path (`--window-cycle`: menu, game, menu on one window).
5. Linux (a window and a surface: X11 / Wayland) and macOS (MoltenVK, Apple Silicon). Not started.

## How it compares (2026-10-15, RTX 4090)

Seven scenes drawn by both (day, close, night with lamps, an effect shot, FSR at
1440p, a replay battle, the strategic zoom): mean difference 0.02 to 0.28 levels of 255, at most 0.02 %
of pixels off by more than 32 (what Direct3D 12 differs from itself between two runs of the battle).
Window cycle: menus identical, game 0.11.

Speed (replay battle, 2560x1440, 400 frames): graphics card 2.18 ms a frame (Direct3D 12 2.29),
drawing recorded 0.78 ms (0.72); a still scene 3.2 ms against 2.2 on the card (noisy between runs; not
chased, see the optimisation rule). Slowest frame 23 ms against 15 (the first ones).

Three things had to be matched to Direct3D 12:
- A pixel shader's `SV_Position.w` is w in Direct3D and 1 / w in Vulkan (particles' soft edges read
  it): DXC's `-fvk-use-dx-position-w`.
- The shaders are written for Direct3D 12's compiler: DXC is asked for HLSL 2018's rules (`-HV 2018`).
- NVIDIA's Direct3D 12 driver takes anisotropic readings a quarter of a mip level sharper than its Vulkan
  driver: with a -0.25 bias on the anisotropic sampler (NVIDIA only; other makers not measured) the ground
  matches to a mean of 0.03. Without anisotropy the two match with no bias.

A review of the backend against the Vulkan specification (2026-10-15) found no error that changes the
picture; what it found was put right: a minimised window (no swap chain while it has no size), one
"rendered" semaphore per swap chain image, a barrier between two drawings into the same attachments,
device features enabled only where the card has them, the fixed samplers counted in the descriptor pools,
placeholder textures of each kind when the card has no null descriptors, constants on further pages
when a frame fills one, the read-back in cached memory (reading 15 MB from uncached memory took 100 ms).

## Third-party parts

- `render/third_party/vulkan`: Khronos Vulkan-Headers 1.3.296, C headers only (Apache-2.0 or MIT).
- `render/third_party/dxc`: `dxcapi.h`, `dxcerrors.h` from Microsoft's DirectX Shader Compiler (University
  of Illinois/NCSA licence, LICENSE-LLVM.txt), kept for reference: `gfx_vulkan_dxc.cpp` declares the few
  calls it uses itself, so that no Windows headers are needed elsewhere. The compiler itself
  (`dxcompiler.dll`) is not in the repository: the Windows build of it comes under Microsoft's licence
  "solely for use on Windows", fine for trying Vulkan on Windows; the Linux and macOS builds would ship
  DXC's LLVM-licensed library (`libdxcompiler.so` / `.dylib`). The viewer finds it beside itself.
