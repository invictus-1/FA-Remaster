# Renderer optimisation: what it costs today, and the options

Written 2026-10-15 for the talk with Chris before the optimisation phase starts. Nothing here has been
done yet; this is the starting point to decide from.

## 1. What a frame costs today

Measured with the viewer's `--timings` (render 195bf77) on this PC: AMD Ryzen 9 9950X3D (16 cores),
NVIDIA RTX 4090, vsync off, 300 frames each, an eight-army M28 game run to tick 6000 (ten minutes in)
unless said otherwise. "CPU" is the processor's time for one frame, "GPU" the graphics card's.

| Scene | Size | CPU ms | GPU ms | Biggest parts |
|---|---|---|---|---|
| Stock Seton's, tick 900, close (90 up), night | 1920x1080 | 1.3 | 1.2 | ground 0.34, units and scenery 0.24, sun shadows 0.21 |
| Stock Seton's, late, middle zoom (150) | 1920x1080 | 7.2 | 1.9 | CPU: listing 5.1, recording 1.7; GPU: ground 0.65, sun shadows 0.30 |
| Stock Seton's, late, whole map | 1920x1080 | 11.8 | 1.2 | CPU: listing the frame 6.3, recording the drawing 5.0 |
| Rebuilt Seton's, late, whole map | 2560x1440 | 10.3 | 3.4 | CPU: listing 4.6, recording 5.3; GPU: units and scenery 1.4, ground 1.1 |
| Rebuilt Seton's, late, middle zoom, night | 2560x1440 | 7.1 | 5.3 | GPU: units and scenery 3.2, ground 1.0, sun shadows 0.35 |

Inside "listing the frame" at the whole-map view: effects 1.6 ms, particles 0.2, bones 0.3; the rest
(about 4 ms) is going through every unit and every tree and rock of the map one by one. "Recording the
drawing" is handing tens of thousands of models (up to 31,600 at once) to the graphics card, once for the
picture and again for each shadow map.

The simulation already runs on its own core beside the renderer, so it is not in these numbers.

**What this says:**
- **The processor is the limit, not the graphics card.** On one of the fastest gaming processors there is,
  a frame takes 7 to 12 ms of processor time in a late game, and all of it on ONE core: 85 to 140 frames
  a second here. On an ordinary 6-core processor (estimate, not measured: about 1.5 to 2x slower per core)
  that is 11 to 24 ms, so 40 to 90 frames a second.
- **The graphics card has room.** 1.2 to 5.3 ms on the 4090. An RTX 3060 is roughly three to four times
  slower (estimate from published comparisons, not measured): about 4 to 20 ms. The worst part is units and
  scenery at middle zoom on the rebuilt map (its nine times as many trees and rocks), then the ground.

## 2. The options

### A. Processor side (the biggest gain; my recommendation to start here)

1. **The frame's preparation on several cores.** Going through units, trees and rocks, effects, particles
   and bones is split over the cores instead of one. Today's 4.5 to 6.3 ms of listing could become about
   1 to 2 ms. Risk: moderate (work split carefully so the picture is the same). Roadmap s06, s11.
2. **Trees and rocks kept on the graphics card.** They do not move, so instead of listing all 25,000 to
   31,000 of them again every frame, they are handed over once, and the graphics card picks the ones in
   view itself. Removes most of the per-frame cost of scenery on the whole-map view, on both sides.
   Risk: moderate; felled trees and props the game changes must still update.
3. **Recording the drawing on several cores**, and fewer, bigger batches (Direct3D 12 allows several
   lists recorded at once). Today's 3 to 5 ms could become about 1 to 2 ms.
4. **Shadow maps that are not redrawn every frame.** The far cascades of the sun's shadow change little
   from one frame to the next; redrawing them every second or fourth frame saves processor and card time.

### B. Graphics card side (for the RTX 3060)

1. **Far trees as simpler shapes** (fewer triangles, or flat pictures for the farthest) and a lower count
   far from the camera. Units and scenery is the biggest card cost.
2. **The ground's extra texture reads** (the rich ground's layers) only near the camera; far ground
   reads fewer.
3. **Quality settings for players** (roadmap s04): shadow map size, ambient occlusion, texture size, tree
   density, the world beyond the edge, lamp shadows. Each one measured for what it saves.
4. **Upscaling:** drawing at a lower resolution and sharpening it up to the screen's. FSR (AMD) is open
   source under MIT and works on any card. DLSS (NVIDIA) looks better on NVIDIA cards but needs NVIDIA's
   SDK under its own license and a closed binary in the release. Which, if any, is a decision for you.

### C. Loading and memory (not frames a second)

1. **Faster loading** (roadmap s07): today every texture upload waits for the graphics card; batching
   them and loading files on other cores cuts load time.
2. **Memory** (roadmap s05): textures not used for a while are let go; on cards with little memory the
   largest size of each texture is left out.

### D. The engine's part (not the renderer's)

The simulation at about 29 ms a tick with eight armies at ten minutes (roadmap s10): fine at normal
speed (100 ms a tick), but it limits fast-forward to about 3x. The engine chat's work, alongside ours.

## 3. How we would measure

- A fixed set of test scenes (the five above, plus a big late battle up close) run by one command that
  writes a table like the one above. Every change is measured against it before and after, and the
  picture is compared too, so nothing looks different by accident.
- On this PC, and on the lower hardware we aim at, if there is a machine to run it on.

## 4. Questions for Chris

1. **The target:** what frames a second, at what resolution, on what hardware? For example "60 at 1080p on an
   RTX 3060 with a 6-core processor, in a late 8-army game".
2. **Hardware:** is there a second machine (or a friend's) with lower parts to test on, or only this PC?
3. **Upscaling:** FSR only (open), DLSS too (NVIDIA's license), or neither for now?
4. **Order:** processor side first (my recommendation, it is the limit today) or quality settings first?
5. **Vulkan** (other systems than Windows, roadmap f06): before the optimisation, after it, or later?
6. **Frame cap:** vsync on by default (smooth, no tearing, caps at the screen's rate), or uncapped?

## 5. Chris's answers (2026-10-15)

1. Target: 60 frames a second, without giving up quality ("we'll see how feasible it is").
2. Test hardware: only this PC. Lower hardware is estimated: fewer cores by limiting the viewer to some
   of them, a slower card by drawing at a higher resolution than the screen's.
3. Upscaling: FSR only. No DLSS for now.
4. Order: the processor side first.
5. Vulkan: the renderer's recommendation, given above all for Linux and Mac (people have asked him for
   both; he has neither): see the next section.
6. Frame rate: vsync on, vsync off and frame caps all offered as options; vsync on by default.

## 6. Vulkan: the plan

- Linux players can already run the Windows build through Steam's Proton or Wine, which turn Direct3D 12
  into Vulkan (vkd3d-proton). A native Vulkan build makes that unnecessary but is not needed for them to play.
- The Mac has no Vulkan of its own: Vulkan runs there through MoltenVK (Vulkan on Apple's Metal), and the
  Mac build would also need its own window code and an Apple Silicon build.
- So: the optimisation first, on Direct3D 12, with every new part of the drawing interface (several cores
  recording at once, scenery kept on the card) shaped so that Vulkan can do the same. Then the Vulkan
  backend, written and tested on Windows on this PC (Vulkan runs there too), which catches nearly all of
  its mistakes without a Linux machine. Then a Linux build, which can be tried on this PC from a Linux
  USB stick (free, no install); the Mac last, through MoltenVK, with players who own a Mac testing it.

## 7. Later: mods (Chris, 2026-10-15)

When every faction is done and the remaining work is mods: the mods' units we remaster are held to
the same costs as our own units (levels, triangles, textures, lights, effects), measured with the
same tools, and checked so the AI can use them without special cases. On FAF, badly built mod units
were the main cause of slow games; ours must not be.

## 8. Done so far (2026-10-15)

Measured on this PC (Ryzen 9 9950X3D, RTX 4090), eight-army M28 game ten minutes in.

- **Processor, the renderer's frame:** 11.8 -> 3.5 ms at the whole-map view (stock Seton's), 10.3 -> 4.2 ms
  on the rebuilt map. Model lookups cached; trees and rocks kept in per-square groups; which models a pass
  sees tested on up to eight threads; a radix sort; effects' lookups by map. Same picture.
- **The game's window** (with the interface chat): the world drawn straight into the game's window and the
  interface laid over it on the card, instead of a read-back and a Windows copy every frame. World picture:
  1600x900 6.7 -> 2.7 ms, 3840x2160 24.5 -> 7.9 ms (plus 0.5 / 3.3 ms of Windows copy gone). The interface's
  own drawing on the processor (9 to 14 ms) is the interface chat's next piece.
- **FSR 1** (AMD, MIT): the card's frame at 3840x2160 on the rebuilt map 12.6 ms off, 6.5 Ultra Quality,
  3.9 Quality, 2.5 Performance. A player's option.
- **The models' depth laid first**, for those large on screen only, and their full shading then writing no
  depth: units and scenery on the rebuilt map at middle zoom 2.49 -> 1.84 ms on the card (1440p). A first
  try that kept writing depth in the full shading saved nothing, and small models drawn twice cost more than
  they spared. On by default; viewer `--no-prepass` turns it off.
- **The ground reads a layer only where it is there** (a map's ten layers each cover a part of it,
  7da8dd3): the card's whole frame, by the lowest of each run's frames (see below), stock map at middle
  zoom 1.14 -> 1.12 ms, from far 1.50 -> 1.41; rebuilt map 2.41 -> 2.24 and 2.60 -> 2.37 (Direct3D 12,
  1440p). (The first numbers given for it, ground 0.95 -> 0.55, were averages taken while other programs
  used the card, and too high.)
- **Direct3D 12's shaders compiled by DXC** where it is beside the program (docs/render-vulkan.md): a
  first start without the shader cache 20-26 s -> 9-11 s; with the cache the same. The card's frame
  0-3 % lower (lowest frames, five scenes). Viewer `--fxc`: the old compiler.
- **At scale (2026-10-16, main b4ab4c4; the viewer's `--crowd UEL0201=n`: a block of tanks at rest, the
  whole map in view from 700-900 up, 1440p):**

  | Tanks | Processor, whole frame | of it: drawing recorded | Card, lowest frame |
  |---|---|---|---|
  | 4,000 | 3.5 ms | 0.7 ms | 1.7 ms |
  | 16,000 | 4.0 ms | 1.4 ms | 2.6 ms |
  | 64,000 | 9.1 ms | 4.0 ms | 6.4 ms |

  - **The biggest parts at 64,000:** strategic icons and bars 1.4 ms, the models' tests 1.1, sorting 0.5, handing
    over 0.7. The rest is the viewer's own way of listing a crowd, not a game's frame.
  - **On the card:** 68,000 models and 40 million triangles at that height; units and scenery take 4.8 of the
    6.4 ms.
  - **Verdict:** inside 60 fps for the renderer at the engine's B4 scale. The simulation will be the limit there
    long before. Tanks that move and pose cost more (bones, effects), which shows in B3-B5 once the engine has
    them.
- **How the card is measured now:** other programs on the card (other chats' replays and tests) raise the
  average of a run by up to 2-3 times. `--timings` also reports each stage's middle and lowest frame, and
  the A/B script waits for an idle card before each run; the lowest frame is what a change is judged by.
