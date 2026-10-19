# The photographic look (World::photoLook)

The visuals session's proposal 6 (`visuals\renderer_reply_lod.md` section 56), approved by Chris on 2026-10-18 for
examples first: "a photographic look preset (eye adaptation, a film-like colour grade, maybe lens touches)". An
option, off by default; with it off the last pass is `Post`'s own and the picture is today's to the pixel. Modern
look only. Code: `render\photo_look.h` / `.cpp`; hooks marked `(photolook)` in `post.h` / `.cpp`, `world.h` / `.cpp`
and `main.cpp`.

## What it does

At the end of the finishing (`Post::Present`, after the glow), in place of the plain tone curve:

1. **The eye** follows the scene's brightness. A compute pass meters 512 x 288 places spread over the picture (each the
   mean of about sixteen pixels, those near the middle counted up to four times as much as the corners) into a
   histogram of 64 bins (four to a stop); a second pass leaves out the darkest 35 % and the brightest 4 % (the night
   sky, the sun on the water, a nuke's flash), takes the mean stop of the rest and moves the exposure half of the way
   (`strength` 0.5) toward the key, never more than 0.4 stop brighter or 0.8 stop darker than today's exposure (the
   time of day's own, which stays the base). Eased: toward brighter in about 1.2 s, toward darker in about 0.5 s. Kept on
   the card (two 1 x 1 targets taking turns); nothing is read back. Set at once after a cut or when the look comes on;
   held still in a photo (`--photo`, key P) and in pictures drawn between the game's frames (the minimap's ground).
2. **The grade**, on the screen's values after today's tone curve: a white balance (on the light, before the curve),
   an S-curve on each channel round a pivot, black lifted a little, less colour in the deepest shadows and the
   brightest lights, shadows and highlights leaning toward two colours. Named looks (`PhotoLook::Named`):
   - `film`: a neutral film: a little more contrast in the middle, slightly milky blacks, calmer extremes, faintly cool
     shadows and warm lights.
   - `warm`: late sun on a colour film: warmer light, golden highlights, teal shadows, a little more colour.
   - `coldwar`: a cold war newsreel's stock: cold light, steel-blue shadows, pale lights, little colour, hard.
   - `plain`: no grade (the eye and the lens touches alone).
3. **Lens touches**, each on its own (`--photo-look-tune vignette=0,grain=0,fringe=0` switches them off): a light
   vignette (a quarter of the light lost in the very corners, on the light before the curve, falling off as the
   distance cubed); faint grain (a hash of the pixel and the frame's number: new every frame, fixed for a pixel within
   a frame, so it never crawls; a standard deviation of about 1.2 % of the screen's range in the middle tones, a
   quarter of that in the blacks and whites); a colour fringe toward the corners (red a pixel outward, blue a pixel
   inward in the very corners at 1080p, growing as the distance cubed). Not on the minimap's ground.

The icons, life bars, order lines and the interface are drawn after it, untouched.

## Viewer

- `--photo-look [film|warm|coldwar|plain]` (film unless named).
- `--photo-look-tune name=value,...` sets any of its numbers (`PhotoLook::Tune`): `warmth, tint, contrast, pivot, lift,
  saturation, extremes, shadows r:g:b, shadowToning, highlights r:g:b, highlightToning, adapt (0 off), key, strength,
  mostUp, mostDown, upSeconds, downSeconds, darkShare, brightShare, vignette, grain, fringe`.
- `--photo-look-compare film,warm,coldwar`: for choosing, one picture in a 2 x 2 grid: today's finish top left, the
  named looks in the other quarters, each a quarter of the same moment (the only way to compare a game replay's
  moment: two runs of a `--sim` replay never show quite the same particles). With `--photo x.png --photo-scale 2
  --photo-sample 1` each quarter is the window's size.

## Numbers

The key (-2.0 stops of the exposed light) is the middle of what today's look meters on the showcase's day pictures
(`_scratch\render\helpers\photolook\meter.py`, the meter done again on the finished pictures: -1.8 to -2.4), so that a
usual day stays as it is: dusk into the low sun meters -1.0 (the eye closes 0.5 stop), snow -1.2 to -1.6 (0.2 to 0.4
stop darker), night -2.9 to -4.1 (0.4 stop brighter, the most it opens: 0.6 at first made the night look like an
overcast day).

Measured in the viewer (`_scratch\render\helpers\photolook`, series of the same moment, today against the eye alone):
through a long battle on the land bridge the eye's picture stays within 0.4 % of its own brightness from frame to frame
(the ratio to today's 1.062 to 1.066 over 8 s); through a nuke's flash (`--volume-test 1,150`) it takes up to 0.8 stop
of the glare off at once (the picture's mean 0.92 today, 0.86 with the eye) and opens again within about 5 s as the
flash fades.

## Costs

`--timings` at 2560 x 1440, 330 frames, the lowest times, RTX 4090 (2026-10-18): the eye 0.024 ms (Vulkan 0.026), the
last pass 0.031 ms (0.034) in place of today's 0.013 or so; in all about 0.04 ms more (Vulkan 0.044). With the
comparison grid the last pass is drawn four times.

## Open

- With FSR 1 (`--fsr`) the last pass is drawn at the scene's size before the upscale, so the grain is upscaled with it
  (coarser).
- A photo (`--photo`, key P) is drawn larger and made smaller, which averages the grain away (by 2 at
  `--photo-sample 2`).
- The glow's threshold still uses today's exposure, not the eye's (a bright scene the eye darkens glows as today's).
- The game's settings (the interface session's bundles) do not offer it yet.
