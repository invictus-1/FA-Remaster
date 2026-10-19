// (photolook) A photographic look (the visuals session's proposal 6, renderer_reply_lod.md section 56; Chris, 2026-10-18):
// the modern look finished as a camera's photo is, in place of the plain tone curve at the end of Post::Present (after
// the glow): an eye that adapts to the scene's brightness, a film-like colour grade, and a few touches of a lens, each
// switchable. A player's option, off by default (World::photoLook); with it off the last pass is Post's own and the
// picture is today's to the pixel. Modern look only; the icons, bars and the interface are drawn after it, untouched.
//
// How (written from the ideas of a camera's light meter and of film grading, as described in Reinhard et al.,
// "Photographic Tone Reproduction for Digital Images", SIGGRAPH 2002, for the key, and in common colour-grading
// practice; nothing copied):
//  - The eye: the picture's light metered as a camera meters it. 512 x 288 places spread over the picture, each the
//    mean of about sixteen pixels round it, the places near the middle counted more; their brightness (as today's
//    exposure makes it) in a histogram of 64 bins, four to a stop. The darkest share and the brightest share are left
//    out (the night sky, the sun on the water, a nuke's flash), the mean stop of the rest is compared with the key
//    (the brightness a well exposed picture of this game has on average), and the exposure goes part of the way
//    (`strength`) toward what would bring it there, never more than `mostUp` / `mostDown` stops from today's (the
//    time of day's own, which stays the base). Eased: toward brighter in about `upSeconds` (an eye opens slowly),
//    toward darker in about `downSeconds`. Kept on the card from frame to frame; nothing is read back.
//  - The grade, on the colours as a screen shows them, after today's tone curve: a white balance (on the light,
//    before the curve), a gentle S-curve round a pivot on each channel (as film's own curve), the blacks lifted a
//    little (film's base), less colour in the deepest shadows and the brightest lights, and shadows and highlights
//    leaning toward two colours (split toning). Each named look is a small set of these numbers (PhotoLook::Named).
//  - Lens touches: a light vignette (the light falling off toward the corners, on the light, before the curve); a
//    faint grain (a hash of the pixel and the frame's number: new each frame, fixed for a pixel within a frame, so it
//    never crawls; most in the middle tones, little in the blacks and the whites); a slight colour fringe toward the
//    frame's corners (red a little outward, blue a little inward, growing with the distance from the middle).
#pragma once
#include <string>

#include "gfx.h"
#include "view.h"

namespace render {

struct PhotoLook {
  bool on = false;
  std::string name = "film";  // the grade's name, as Named knows it
  // The grade (on the screen's values, 0..1, unless said).
  float warmth = 0;          // white balance on the light: + warmer (more red, less blue), - colder; 0.1 is a strong shift
  float tint = 0;            // + toward magenta, - toward green
  float contrast = 1;        // the S-curve's slope at its pivot (1: none)
  float pivot = 0.45f;       // where the S-curve turns
  float lift = 0;            // what black is lifted to
  float saturation = 1;      // colour overall (1: as it is)
  float extremes = 0;        // how much colour the deepest shadows and the brightest lights lose (0..1)
  float shadows[3] = {0, 0, 0};     // the colour the shadows lean toward (linear 0..1; its own brightness is taken out)
  float highlights[3] = {0, 0, 0};  // ... the highlights
  float shadowToning = 0, highlightToning = 0;  // how far toward them (0..1)
  // The eye (the exposure following the scene's brightness).
  bool adapt = true;
  // (the key: the middle of what today's look meters on the showcase's day pictures, -1.8 .. -2.4, so that a usual
  // day stays as it is; dusk into the sun meters -1.0, snow -1.2 .. -1.6, night -2.9 .. -4.1)
  float key = -2.0f;          // the mean brightness a good picture has, in stops (log2 of the exposed light)
  float strength = 0.5f;      // the share of the way to the key the exposure goes (0: none, 1: all the way)
  float mostUp = 0.4f, mostDown = 0.8f;     // the most it may open or close, in stops from today's
  float upSeconds = 1.2f, downSeconds = 0.5f;  // how long it takes (about) to open, and to close
  float darkShare = 0.35f, brightShare = 0.04f;  // the darkest and the brightest shares of the picture left out of the mean
  // Lens touches (0: none).
  float vignette = 0.25f;  // the light lost at the corners (0.25: a quarter)
  float grain = 0.6f;      // the grain's strength (1: about 2 % of the screen's range, as a standard deviation, in the middle tones)
  float fringe = 1.0f;     // the colour fringe at the corners (1: red and blue 0.1 % of the picture's height to either side)
  // For choosing between looks: up to three named looks (commas between) shown beside today's finish in a 2 x 2 grid
  // of the same picture, each a quarter of it (today's top left, then along the rows), each with this look's eye and
  // lens numbers and its own grade. Empty, as at first: this look alone, over the whole picture.
  std::string compare;

  // The named looks: "film" (a neutral film), "warm" (late sun, golden lights, teal shadows), "coldwar" (cold, steel
  // blue, little colour, hard), "plain" (no grade: today's colours, the eye and the lens touches alone). Sets the
  // grade's numbers and `name`; false (nothing changed) for a name it does not know.
  bool Named(const std::string& look);
  static const char* Names() { return "film, warm, coldwar, plain"; }
  // Numbers set by name=value pairs with commas between (the viewer's --photo-look-tune): the grade's, the eye's and
  // the lens's above by their names (shadows and highlights as r:g:b). False, with why, for one it does not know.
  bool Tune(const std::string& list, std::string* why);
};

class PhotoFinish {
 public:
  explicit PhotoFinish(gfx::Device* device) : device_(device) {}
  ~PhotoFinish();
  PhotoFinish(const PhotoFinish&) = delete;
  PhotoFinish& operator=(const PhotoFinish&) = delete;

  // Before the frame's finishing (World::Render): the look, the frame's time for the eye's easing (0: the eye stays as
  // it is, for a photo or a side picture drawn between the game's frames), `snap`: the eye set at once (a cut), and
  // whether the lens's touches are drawn (not on a side picture).
  void Set(const PhotoLook& look, float seconds, bool snap, bool touches);
  // Post::Present, before its last pass: meters `picture` (the light it finishes, `width` x `height`) for the eye and
  // moves the eye on. Its shaders are made the first time. False when it cannot run (the last pass is then Post's
  // own). Between passes: it dispatches.
  bool Prepare(const View& view, gfx::Texture picture, int width, int height);
  // ... then the last pass, in place of Post's, in the pass Post has begun (into the window, or `toTarget`: into a
  // target before FSR 1): `picture` and `glow` as Post's own last pass reads them.
  void Finish(const View& view, bool toTarget, gfx::Texture picture, gfx::Texture glow);
  bool Failed() const { return state_ < 0; }

 private:
  bool Init(std::string* error);
  // The shaders' numbers for `look` on a picture `width` x `height` (the eye's, the grade's, the lens's).
  void Constants(const PhotoLook& look, int width, int height, void* out) const;
  gfx::Device* device_;
  int state_ = 0;  // 0 shaders not made yet, 1 made, -1 they cannot be
  gfx::Pipeline meter_, adapt_, window_, toTarget_;
  gfx::Texture partial_;   // R32UI: each group's histogram (64 bins x the groups)
  gfx::Texture eye_[2];    // RGBA32F 1 x 1: x the eye's stops from today's exposure; y the metered mean; z what it went toward
  int now_ = 0;            // which of eye_ is this frame's
  bool haveEye_ = false;   // an eye kept from a frame before
  uint32_t frame_ = 0;     // (the grain's frame number)
  PhotoLook look_;
  float seconds_ = 0;
  bool snap_ = false, touches_ = true;
  int width_ = 1, height_ = 1;  // (the picture Prepare metered)
};

}  // namespace render
