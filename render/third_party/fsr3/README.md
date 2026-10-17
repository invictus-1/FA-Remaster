# AMD FidelityFX Super Resolution 3 upscaler: its compute passes

From AMD's FidelityFX SDK v2.3.0 (the FSR 3.1.5 upscaler, `ffx_fsr3upscaler`), downloaded with Chris's approval
on 2026-10-16. MIT: `LICENSE.txt` here (the SDK's `Kits/FidelityFX/docs/license.md` lists every file below under
its MIT part; its other licence, for binaries, does not apply to them). Only what the passes the renderer runs
need is here: no runtime, no DLL, no backend, no API. `render/fsr3.cpp` does on the processor what AMD's runtime
does (`ffx_fsr3upscaler.cpp`), over the renderer's own graphics layer.

## The files and where they come from (the SDK's paths, under `Kits/FidelityFX/`)

- `api/internal/gpu/`: `ffx_common_types.h`, `ffx_core.h`, `ffx_core_hlsl.h`, `ffx_core_gpu_common.h`,
  `ffx_core_portability.h`
- `upscalers/fsr3/include/gpu/fsr3upscaler/`: `ffx_fsr3upscaler_accumulate.h`, `_callbacks_hlsl.h`, `_common.h`,
  `_luma_instability.h`, `_luma_pyramid.h`, `_prepare_inputs.h`, `_prepare_reactivity.h`, `_reproject.h`,
  `_resources.h`, `_sample.h`, `_shading_change.h`, `_shading_change_pyramid.h`, `_upsample.h`
- `upscalers/fsr3/include/gpu/spd/ffx_spd.h`
- `upscalers/fsr3/internal/shaders/`: `ffx_fsr3upscaler_prepare_inputs_pass.hlsl`, `_luma_pyramid_pass.hlsl`,
  `_shading_change_pyramid_pass.hlsl`, `_shading_change_pass.hlsl`, `_prepare_reactivity_pass.hlsl`,
  `_luma_instability_pass.hlsl`, `_accumulate_pass.hlsl`

They lie flat here; their `#include`s keep AMD's paths and are found by file name (`embed_fsr3.py`).

## What was changed in them

One thing, marked "FA Remastered" where it is: in `ffx_fsr3upscaler_callbacks_hlsl.h` the point-clamp sampler is
declared at register `s2` instead of `s0`, as the renderer's samplers are fixed (`render/gfx.h`: s1 linear clamp,
s2 point clamp). The linear-clamp sampler stays at `s1`. (The passes here only read through the linear one.)

## What was left out

- The SDK's RCAS pass (`ffx_fsr3upscaler_rcas.h`, `_rcas_pass.hlsl`): it includes the SDK's own FSR 1, which was
  not taken. `render/fsr3.cpp` runs the same sharpening with the same layout (64 threads, four pixels each of a
  16 x 16 tile, the colour times the exposure around the filter) on the repository's FSR 1
  (`render/third_party/fsr1`, the same `FsrRcasF` and `FsrRcasCon`).
- The core's headers for the processor (`ffx_core_cpu.h`), GLSL (`ffx_core_glsl.h`) and 16-bit floats
  (`ffx_core_gpu_common_half.h`): these passes are compiled for the graphics card in HLSL without 16-bit floats.
  The embedded copy puts an `#error` where each is included, under the condition that would need it.
- The debug view, the reactive mask's generation and the lookup table of Lanczos weights (the history is
  reprojected with the exact weights, as AMD's runtime does on cards that are not RDNA).

## Embedding

The renderer compiles its shaders at run time from one string each, without an include handler.
`py -I render/third_party/fsr3/embed_fsr3.py` writes `render/fsr3_source.cpp`: each pass's includes followed as
the preprocessor would (a guarded file once a pass), each file cut at its `#include` lines into parts written once
and shared by the passes (each part opens with a `#line` naming its file, for the compiler's messages), and for
each pass the list of its parts. Run it again after changing anything here.
