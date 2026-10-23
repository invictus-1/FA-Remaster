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
- **Bones on several cores (2026-10-16):** the posed models' bone matrices are made on the worker
  threads, and which posed models some pass sees is tested there too. The matrices are written straight
  into the frame's copy of the bone buffer (`gfx::Device::WriteBuffer`, both backends), not into an array
  that was then copied. Viewer `--crowd UEL0201=10000 --turn turret`, 1080p, processor:
  - bones: 1.81 -> 0.46 ms (listing 0.24, matrices 0.22);
  - whole frame: 5.03 -> 4.40 ms.

  Pictures are pixel for pixel the same on Direct3D 12 and on Vulkan.
- **How the card is measured now:** other programs on the card (other chats' replays and tests) raise the
  average of a run by up to 2-3 times. `--timings` also reports each stage's middle and lowest frame, and
  the A/B script waits for an idle card before each run; the lowest frame is what a change is judged by.

## 9. The latency limiter ("Low Latency", 2026-10-18)

Chris approved it 2026-10-17 (the renderer's next looks, item 7): the camera, the cursor's feedback and the
interface answer the mouse and the keys sooner. Our own and vendor-neutral: no NVIDIA Reflex, AMD Anti-Lag or
LatencyFleX code (the "just in time" frame start is the common idea). `gfx::Device::SetLatencyLimiter`,
`WaitForNextFrame` (gfx.h), the shared reckoning in `render/gfx_latency.h`; viewer `--low-latency` (key K), the
game's host through `WorldPicture::SetLowLatency` and `WorldPicture::WaitForNextFrame`. Off by default; off, the
swap chains and the waits are as they always were (regression: every scene "differ 0 px"; and on against off, the
same scene with vsync, also 0 px: it changes when frames begin, never what they show).

- **Why.** Without it a frame's input is read up to three frames before it is shown (Direct3D 12's frame latency of
  three, a swap chain of three buffers), and the compositor adds a refresh: 70 ms from the input read to the screen
  at 60 pictures a second, about 20 ms at 240 Hz (Direct3D 12), 17-41 ms on Vulkan.
- **Direct3D 12, with vsync.** The swap chain is made with its frame latency waitable object at a frame latency of
  one (anew when the limiter is switched, as frame generation does). The host's WaitForNextFrame waits on that
  object, then on purpose until the latest start that still makes the coming refresh: the refresh from the swap
  chain's frame statistics (the newest picture shown and its refresh, an interval for each picture handed over
  since, less one refresh: the compositor takes a window's picture the refresh before it shows it), less the most
  work of the last 64 frames (input read to hand-over, and the card's part by its own timestamps in the processor's
  time, through the queue's clock calibration), less a margin. The margin doubles at once when a frame comes too late
  (by the card's clock, or by the frame statistics' refresh count) up to where no wait is left, and creeps back
  0.01 ms a frame in time, never below what was last too little. Frames that do not fit a refresh one after the
  other are let two ahead (the frame latency set to two; what the object then let ahead too much is taken back when
  it goes back to one). It never starts a frame so early that it would only wait for the card.
- **Vulkan, with vsync.** The present wait (`VK_KHR_present_id`, `VK_KHR_present_wait`, enabled at the device's
  making where offered; unused while the limiter and the measure are off): before the input read, until the picture
  before the last is shown (as Direct3D 12's frame latency of one), the chain's image acquired, the wait for the
  card. No wait for the refresh: the present wait's times come too unevenly on NVIDIA's Windows driver (the gaps
  between pictures spread by 6-7 ms at 240 Hz), and a refresh reckoned from them fed back into the waits (a first
  version all but stopped the viewer). The refresh rate for the "two ahead" choice comes from the system (the
  window's screen). Without the present wait (older drivers, MoltenVK): the frames on the card kept to two.
- **The wait for the card** (both backends, with vsync and without): a frame begins no earlier than the card is about
  to be free of the frames before (their hand-over and the card's least time for a frame of the last 64), less the
  most its own processor part took, less the lead: the longest the card took of the last 64 frames to start one once
  it was free for it (some tenths of a millisecond where the card is ours). With a fixed lead of 0.3 ms, the way
  without the limiter came 0.98 of its time between frames without vsync (0.91-1.06, window against window within
  runs, below), with this lead 1.00-1.02. Where other programs share the card, their work lengthens the lead by
  itself (1.5-15 ms measured), and the wait shortens towards none.
- **Without vsync** only the wait for the card is kept (it trims the card's queue where the card sets the pace); the
  swap chain is as without (Direct3D 12 makes it anew when vsync is switched, only where its flags change: a chain
  made anew for the window without vsync once ran three times slower).
- **With frame generation** the real frames waiting for the presenter are kept to one (none where a frame's work
  fits well within a refresh); the presenter and its swap chain are as without. Holding the presenter's own queue to
  one or two pictures as well was tried: it cut more at 60 Hz but cost up to half the pictures a second at 240 Hz
  and without vsync on this busy machine, so it is left out.
- **The frame rate kept** (`Keep`, gfx_latency.h). Fewer frames waiting absorb less of the card's uneven times when
  other programs share it: with the card 85-100% used by others, frames at 240 Hz came 5.5-6.7 ms apart without the
  limiter and 7.6-9.5 ms with it. So while frames do not keep up (without vsync, or with it more than a tenth slower
  than the refresh), the limiter now and then runs 120 frames the way without it (three ahead, no wait of its own)
  between two windows of 120 frames of its own, and keeps to the way without where that came more than 7% faster
  than both; later it runs its own way between two windows without, and goes back to it unless that is again as much
  slower. The first look comes 360 frames after the start, each later one twice as late as the one before (up to
  5760 frames between looks, and 9600 kept to the way without). Where frames keep up with the refresh it never
  looks. What the looks found is under "Measured".
- **The measure** (viewer `--latency-stats`; the window must be on the screen, uncovered: a covered window's frame
  statistics stop, and many runs here saw few showings): from each input read to the refresh its frame was first
  shown at (Direct3D 12: the frame statistics; Vulkan: the present wait's return, which comes a little late and
  unevenly), the gaps between pictures shown, the limiter's state (margin, lead, how often it stepped back and what
  the way without came to against its own) and its own cost. `--sync-interval 4` shows 60 pictures a second on the
  240 Hz screen (Direct3D 12), `--hitch n,ms` makes every n-th frame longer, `--low-latency-switch n` and
  `--vsync-switch n` switch them while running.

**Measured** (2026-10-18, render 4099028 and this change as it is; RTX 4090, 240 Hz screen, the remastered Seton's at
1280x800, light: the map alone, heavy: `--crowd UEL0201=10000 --turn turret` seen whole; each off and on run one after
the other, two rounds, 900 frames, 600 at 60 a second; the map's decals came in between the rounds). The machine was
busy all the while: 2-25 other viewers, replays and games, the card 7-100% used by them, so one run against the next
swings by up to five times in frame time; the ranges are both rounds'. Input read to the screen, average / 99th
hundredth, ms; "late": pictures shown a refresh or more after their time, of 200-800 seen:

| Case | Off | On | Late, off / on | ms a frame, off / on |
|---|---|---|---|---|
| 60 a second (`--sync-interval 4`), light | 70.5-70.6 / 70.7-74.6 | 16.6-20.8 / 19.7-37.4 | 0-4 / 0-1 | 16.6-16.7 both |
| 60 a second, heavy | 68.7-70.6 / 70.7 | 22.7 / 37.4 (70 seen) | 1-6 / 1 | 16.7-16.8 / 16.8-17.1 |
| 240 Hz, light | 20.9-24.3 / 28.2-37.3 | 11.3-18.9 / 16.6-24.9 | 6-148 / 1-256 | 4.29-5.29 / 4.25-6.07 |
| 240 Hz, heavy | 35.9 / 46.4 | 12.1 / 15.1 | 181 / 11 | 4.20-8.54 / 4.53 (and a run of 27, a 2 s stall) |
| a frame 8 ms longer every 30th, 60 a second | 70.6 / 70.7 | 19.3-22.7 / 20.8-25.9 | 0-1 / 0-2 | 16.6-16.7 both |
| a frame 3 ms longer every 30th, 240 Hz | 20.7 / 24.8 | 12.3 / 16.5 | 7 / 9 | 4.49 / 4.53 |
| no vsync, light | 8.7 / 11.2 | 9.8-17.5 / 17.6-26.9 | - | 1.50-7.99 / 1.76-3.38 |
| no vsync, heavy | 21.3-28.7 / 34.6-39.2 | 14.9-19.6 / 16.0-32.9 | - | 5.18-7.41 / 4.74-5.26 |
| frame generation, 60 a second | 153-185 / 169-204 | 89-112 / 154-197 | pictures 48.7-57.1 / 52.1-59.9 a second | |
| frame generation, 240 Hz | 48-54 / 64-86 | 30-37 / 45-58 | pictures 144-189 / 139-232 a second | |
| frame generation, no vsync | 43 / 81 | 42-120 / 73-233 | pictures 222-997 / 52-123 a second | |
| Vulkan, 240 Hz, light | 30.1-35.2 / 41.5-44.9 | 22.5-23.7 / 34.7-41.3 | (Vulkan's gaps too uneven to count) | 4.29-4.68 / 4.83-5.78 |
| Vulkan, 240 Hz, heavy | 28.7-32.9 / 46.2-52.2 | 16.1-33.2 / 36.9-56.1 | | 5.70-6.16 / 5.36-8.79 |
| Vulkan, a frame 3 ms longer every 30th | 25.9-33.1 / 34.9-51.2 | 14.9-22.4 / 25.9-39.2 | | 5.45-5.46 / 4.70-4.90 |
| Vulkan, no vsync | 18.0-34.3 / 38.6-49.8 | 7.3-15.9 / 16.1-30.3 | | 3.71-8.74 / 2.45-7.54 |
| Vulkan, frame generation, 240 Hz | 42.8-45.5 / 45.6-77.8 | 32.4-39.7 / 52.6-61.2 | pictures 202-239 / 214-230 a second | |

Earlier, on a less busy afternoon, with the limiter as it was before the lead and Keep (which change nothing where
frames keep up with the refresh): 60 a second 70.6 -> 10.5-21.8 ms, heavy 70.6 -> 19.2; 240 Hz 20.7 -> 12.4, heavy
19.0 -> 12.2; the frame rate the same within the noise in every case.

**The frame rate, run against run, is too noisy here to judge**; window against window within a run is not (Keep's
own comparison, printed by `--latency-stats`): the way without the limiter's time between frames over the limiter's
own, 120 frames each, on both sides: without vsync 1.02 on average (0.92-1.14, 14 runs: no cost), with frame
generation 0.96 (0.87-1.02, 10 runs), with vsync where frames fell behind the 240 Hz refresh (the card shared)
0.84 (0.63-0.98, 15 runs; Keep stepped back in most of them). Where frames keep up with the refresh (all 60 a second
runs) it never had to look.

The late pictures at 60 a second were looked into frame by frame (the frame statistics against each frame's
hand-over, on the version before Keep): the late ones had mostly been handed over three to four refreshes before
their deadline and were still shown a refresh later, so it is the compositor's side on a loaded machine (its
handling of an interval of four, too), against which the frames waiting in line without the limiter are a cushion:
1-50 late against 0-15 of about 520 then, 0-1 against 0-4 in the final runs.

**Costs.** The card: nothing (two timestamps a frame). The processor: 0.005-0.03 ms a frame for the limiter's own
reckoning; its waits are the point (they sit before the input read, where the frame would otherwise have waited
later). `--timings` at 2560x1440, 300 frames, three rounds each, three times over the day, the lowest: the card's whole
frame without vsync 2.00-2.05 ms off, 2.04-2.21 on; with vsync (240 Hz) 2.00-2.06 off, 2.47-2.49 on: the card now idles
between frames and clocks down, so each frame takes it a little longer (the limiter's reckoning counts that in, and the
frame rate is the same). The processor's "waiting for a free frame" falls from 0.3-7.3 ms to 0.05-2.9 (that wait now
comes before the input read). Switching vsync with the limiter on makes Direct3D 12's swap chain anew once (a wait for
the card, as switching frame generation does).

**Default on?** The renderer's recommendation: yes, Chris's call. With vsync and frames that keep up with the
refresh (the usual case) the frame rate is the same and the latency falls by a half to four fifths; without vsync
and with frame generation the frame rate is the same within runs (1.02, 0.96) and the latency the same or lower;
long frames are fine. Strictly by the ui's bar ("costs nothing measurable") it falls a little short, all of it seen
with other programs on the card: a picture now and then a refresh late at 60 a second (more often in the busiest
rounds); the card's lowest frame 0.4 ms longer with vsync (lower clocks between frames, the rate the same); and
where frames fall behind the refresh, the limiter's own way is slower (the way without took 0.84 of its time between
frames on average) until Keep has looked and gone back to the way without: its first look some 750 frames after the
start (about 3 s at 240 Hz), later ones further apart (up to some 10000 frames). A player can switch it off.

**For the interface (the ui chat).** The host sets `WorldPicture::SetLowLatency(on)` from the Video tab's "Low
Latency" (before or after Open; on each picture it shows in its window, the menu's too), and once a frame calls
`WorldPicture::WaitForNextFrame()` of the picture it is about to draw, just before it reads its input (its window's
messages, the mouse's place), then draws the frame (`DrawToWindow`). Without that call it still works, with the
waits at the frame's start, after the input read (less gained). Tooltip: "Fewer frames wait to be shown, so the
picture answers the mouse and the keys sooner; on a busy PC a frame may now and then come a refresh late."

**Not tested here:** a real 60 Hz screen (only 60 pictures a second on the 240 Hz one, where a late picture costs
4.2 ms, not a whole 16.7 ms refresh), an idle card (other chats' viewers, replays and games ran on it all day),
variable refresh (G-Sync, FreeSync), a window filling the screen (shown a refresh sooner: the deadline is then a
refresh early, which costs that refresh but misses none), other cards and drivers, Linux and the Mac.

## 10. The optimisation run (agreed with Chris, 2026-10-21)

**The target.** Chris: "if we can push it beyond 60 into 120 then that is even better. Since 60 is the minimum while
anything higher is the goal. Cause mods change things a lot too." And on 4K: "4k 120 for this game is possible so lets
try and aim for it and past it." So: 60 frames a second is the floor, 120 and more the aim, at native 4K (FSR stays a
player's option, not the default), with room left for mods. A frame's budget is about 8 ms on each side.

**Where it stands** (render f842afd, every look the game has on, SETONS_R, an eight-army M28 game at minute 10, 1440p,
RTX 4090, Ryzen 9 9950X3D; other chats' work on the card raised the middle zoom's typical frame):

| View | Processor (the renderer's own work) | Card | Frames a second today |
|---|---|---|---|
| Middle zoom over a base (150) | 11.4 ms: listing 4.9, recording 6.0 | 10.0 ms lowest, up to 23 typical | 45 to 85 |
| Whole map (900) | 12.8 ms: listing 4.6, recording 7.8 | 6.5 ms | 78 |

On the card at the middle zoom: units and scenery 2.1-9.9 ms, sun shadows 0.5-2.0, ground 1.7, the ground cover
0.2-1.4, the ray tracing scene 0.4-1.5, energy shields up to 1.4 while shown. At native 4K the card's share is about
2.25 times these.

**Spikes** (what a player feels, larger than any average): an upgrade starts with 30-36 ms of the simulation's beat
(benchmark B9: 16 at once about half a second; half in FlattenSkirt's 81 FlattenMapRect calls a factory, each updating
every path grid; half in M28's CheckIfSegmentsStillBuildable re-checking about 16,700 build places); Lua 5.0's full
collections 96-195 ms; a new footprint kind's first path grid 21-144 ms; on the renderer's side a new unit kind's first
model 50-145 ms, the ground cover's first time near the ground 30-200 ms, the game's first frame about 3 s; on the
interface's, the minimap redrawn whole as the weather moves.

**Chris's answers.**
1. The order: the spikes first, then a steady 120, then tens of thousands of units, then loading and memory.
2. M28's half of the upgrade's cost: our own M28 variant (its own folder, maudlin27 credited, CC BY-NC-SA, as CLAUDE.md
   sets out), separate from the engine.
3. 4K: native 120 and beyond is the aim.
4. Measuring: all chats pause for about an hour now and then, so the benchmarks run on a quiet machine.

**The renderer's part, in that order.**
1. Spikes: every unit kind the armies can build read and made at the game's start (or on the worker threads before it
   is first seen); the ground cover's first time made ahead; the first frame under a second later (loading).
2. A steady 120: the drawing recorded on several cores (Direct3D 12 and Vulkan both allow it); trees and rocks kept on
   the card and picked there (indirect draws, which also end the 3D plants' reserved room); the far sun-shadow cascades
   not redrawn every frame; the ray tracing scene refitted, not rebuilt; units' and trees' far levels and impostors
   with the visuals session.
3. Tens of thousands of units: the units' visibility tests on the card too, with the engine's B3-B5 run in the viewer.
4. Loading and memory: textures streamed in.

**How it is measured.** A fixed set of scenes (a game's start, a base at middle zoom, the whole map late, a big battle,
16 upgrades at once, the snow's start), at 1440p and 4K: the lowest and typical frame, the slowest 1 % and the count of
frames over 8.3 / 16.7 ms, before and after each change, on a quiet machine. Lower hardware is estimated from this PC
(a card like an RTX 3060 about three to four times slower, an ordinary six-core processor about 1.5 to 2 times).

**The quiet baseline (2026-10-23 12:09-12:41, render dc4feb0, nothing else running: CPU 5 %, card 19 % before).**
The set R1-R6 (the benchmark helper's tool, `render/tools/bench_render.py` once merged; results in the lab's
`_scratch/render/helpers/rbench/runs/2026-10-23_quiet_baseline/results.md`). Typical (median) frame, slowest 1 %,
frames over 8.3 ms, and which side holds it:

| Scene | 1440p: typical / 1 % / >8.3 | 4K: typical / 1 % / >8.3 | Held by |
|---|---|---|---|
| R1 start (600 frames) | 8.5 / 32.8 / 362 | 12.8 / 30.7 / 557 | first frames: frame 2 = 252 ms (1440p), "scenery listed" 207 ms (the first tree groups' models) |
| R2 base, middle zoom, min 10 | 9.1 / 12.7 / 985 | 14.8 / 19.5 / 1139 | processor 9.0 (1440p); card 14.5 (4K); one 47 ms frame: a new unit kind made inside "effects" (35 ms) |
| R3 whole map late | 14.8 / 19.5 / 1139 | 15.6 / 19.9 / 1139 | the processor (14.8 / 15.5; the card 7.3 / 13.8): listing the scenery |
| R4 big battle | 12.1 / 16.6 / 1139 | 14.0 / 19.1 / 1139 | the processor (12.0 / 13.8; card 8.3 / 14.2) |
| R5 16 upgrades | 8.0 / 10.7 / 313 | 15.4 / 17.3 / 1133 | card at 4K (15.2); one 40-50 ms frame: a new building model made inside "effects" |
| R6 snow's start | 5.6 / 7.0 / 4 | 11.0 / 13.9 / 1436 | card at 4K (10.8); no spike (the snow freeze is gone) |

What it says: (1) the spikes left are first-time models made in the frame (R1 frame 2, R2/R5's "effects" frames):
the spikes helper's work; (2) a steady 120 at 1440p is held by the processor in mid and late games (9-15 ms: listing
the scenery and units, recording): the step-2 work (recording on several cores, scenery picked on the card); (3) at
native 4K the card is at 11-15 ms in every busy scene: the card's share of step 2 (cascades not redrawn every frame,
the ray tracing scene refitted, far levels).
