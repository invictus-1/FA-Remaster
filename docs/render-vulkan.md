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
   t0-t39 (bindings 16-55; t0-t31 at 16-47 until 2026-10-17), t0-t1 of space1 (storage buffers, 56-57) and the six fixed samplers (64-69);
   set 1 holds b0-b3 (dynamic uniform buffers, 0-3) pointing into the frame's pages of constants. Set 0's
   layout is made per arrangement of textures the shaders declare (read from the SPIR-V: an array of
   textures, `Texture2D maps[10] : register(t6)`, is one binding of ten in Vulkan); its sets are cached per
   frame by what is bound. Vertex inputs found by their HLSL semantics (DXC's `-fspv-reflect`
   decorations, stripped again before the driver sees them).
4. **Done**: timestamps (`Mark`/`TakeTimes`), `UpdateTexture`, the whole scene against Direct3D 12, the
   direct window path (`--window-cycle`: menu, game, menu on one window).
5. **Written, untried** (2026-10-15; there is no Linux or Mac here): Linux and macOS.
   - The device takes the window as its system has it (`gfx::DeviceDesc::windowSystem`, `window`,
     `display`): an X11 window (Xlib), a Wayland surface, or a macOS view's CAMetalLayer (through MoltenVK).
     A host gives it with `WorldPicture::UseNativeWindow(system, window, display, width, height)`.
   - Vulkan 1.3, or 1.2 with dynamic rendering and the second synchronization as extensions (MoltenVK
     and older drivers); MoltenVK's "portability" enumeration and `VK_KHR_portability_subset` are asked
     for when offered. Device features (anisotropy, BC textures, null descriptors) only where the card
     has them; without null descriptors, empty texture slots get placeholders of each kind.
   - The loader: `libvulkan.so.1` on Linux; `libvulkan.1.dylib` or `libMoltenVK.dylib` on macOS. DXC:
     `libdxcompiler.so` / `.dylib` beside the program. Shader cache: `$XDG_CACHE_HOME` or
     `~/.cache/fa-remastered-shaders`.
   - Direct3D 12's file builds to nothing outside Windows; `render/gfx.cpp` chooses the backend
     (Direct3D 12 on Windows unless Vulkan is asked for, Vulkan elsewhere).
   - Checked here: every file in `render/` compiles for x86_64 Linux and for Apple Silicon macOS (zig's
     clang cross-compiling), except `window_win32.cpp`, the viewer's own window.
   - Still to do, by someone with the machine: a window for the viewer there (X11 / Wayland / Cocoa; the
     game's own window is the interface's), building and linking there (the build script makes the
     Windows program), shipping DXC's library, and the first run. MoltenVK may refuse something the
     shaders do (it turns SPIR-V into Metal); that would show at the first run.

## Ray tracing (2026-10-17)

`gfx.h`'s ray tracing (structures over triangles, traced inline with `RayQuery`), as Direct3D 12's backend
has it. Only where the card offers all of `VK_KHR_acceleration_structure` (with
`VK_KHR_deferred_host_operations`), `VK_KHR_ray_query` and buffers' device addresses; elsewhere
(MoltenVK, older cards) `RayTracing()` is false and nothing changes, not even a buffer's flags. With it, the
buffers' memory blocks allow device addresses, static vertex and index buffers (and the frame's pages,
where a scene's instances are written) may be read by builds. A geometry's structure is made at once and
built at the next `BuildScene`, side by side in a scratch buffer they share (batches of 64 MB, a barrier
between); the scene's instances go into the frame's memory, its structure (fast build) is built again and
made anew, a half larger, when too small. Barriers: what shaders read of a scene, finished before it is
built again; what builds wrote, seen by the next builds and by vertex, pixel and compute shaders. A scene
is bound in a texture's binding (t0-t39 at 16-55): the SPIR-V reflection knows an acceleration structure
there (`OpTypeAccelerationStructureKHR`), the set layout has an acceleration structure descriptor in its
place, and the set cache's key holds the structure itself (a grown scene is a new one). A slot whose shader
declares a scene that nothing is bound in reads a scene of nothing, built at the first frame. A shader
that traces rays on a card without them is refused when its pipeline is made. `--gfx-check --vulkan`
traces the same rays as Direct3D 12 (RTX 4090: every hit right); the validation layer was not installed
here, so it has not looked at this part.

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
  (`dxcompiler.dll`) is not in the repository; the viewer finds it beside itself. Its licence: DXC is
  under the University of Illinois/NCSA (LLVM) licence (the repository's README). The Windows release
  zip (v1.9.2609) carries three licence files, LICENCE-MIT, LICENSE-LLVM and LICENSE-MS (Microsoft's
  terms: "solely for use on Windows", distributable code with conditions), without saying which file
  each covers; NuGet's Microsoft.Direct3D.DXC gives MIT for `d3d12shader.h` and LLVM for all the
  others, so `dxcompiler.dll` reads as LLVM-licensed, and LICENSE-MS most likely as `dxil.dll`'s,
  which is not needed. The Linux and macOS builds are DXC's LLVM-licensed library
  (`libdxcompiler.so` / `.dylib`), v1.7.2212 or newer: older ones gave IUnknown a virtual destructor
  outside Windows, so their interface tables do not match the one declared. Chris, 2026-10-15: a release may ship `dxcompiler.dll` beside the
  program (decided on the stricter reading); its licence files go with it.
- Direct3D 12 compiles with DXC too (DXIL, shader model 6.0) where `dxcompiler.dll` is beside the
  program, and with Direct3D's own compiler (FXC, part of Windows) where it is not, or where DXC fails
  on a shader (the viewer's `--fxc`: FXC always). DXC of 2026-09-29 signs its shaders itself;
  `dxil.dll` is not needed (checked: the same bytes with and without it).
