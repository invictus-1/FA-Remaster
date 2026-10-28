// Zooming out through the clouds (a trial; docs/render-lighting.md, the next looks, item 14; World::cloudZoom, viewer
// --cloud-zoom). With the volumetric clouds (clouds.h) on, the game's camera, as it zooms out and rises, goes up
// through the cloud layer instead of the layer rising with it: the clouds pass the camera, then lie below it over
// the map, and thin out the higher it goes, so the strategic view stays as readable as without them. Zooming in
// goes down through them again.
//  - Below the layer (the eye more than 40 under its underside) nothing changes: the clouds are the sky's, as
//    without it, marched as without it. From there the layer stays where it is (Clouds::zoom), and the zoom's own
//    march (MarchZoom, cloud_zoom.cpp) goes through it wherever it lies before what was drawn, the ground too.
//  - How the clouds thin, three ways together (For, below), of the three the task offered to weigh:
//    by the eye's height over the layer's top, over a band (`band`): their outer parts eaten (`erodeMost`, so they
//    break up and shrink rather than turn to glass) and the rest kept faint (`keepMost`). The main one: from high
//    above the map must read as today, and it leaves a few small clouds, as fair-weather cumulus look from far up.
//    Round the view's middle, where the player looks, they part (`partErode`, `partKeep`, a soft ring from
//    `partFrom` to `partTo` half heights off the middle), from the layer's top up (`partDepth`), so that a camera
//    stopped just above it still sees what it looks at; kept mild, as stronger it shows as a hole that follows the
//    camera, and not inside the layer, where in a cloud it showed as a dark disc in the fog. Right at the eye they
//    fade (`near`), so that inside the layer the eye sees a little way into them and wisps go by round the view.
//    Their shadows on the ground stay as they are at every height (the third way offered: from high above the
//    clouds are mostly their shadows, as without the zoom, and a few thin clouds).
//  - Seen from close they would be soft blobs (the noise is sized for clouds far off in the sky): the zoom's march
//    makes them denser (`gain`, crisper outlines), heaps their tops in lumps (`lumps`), lets a finer detail bite
//    into their outer parts (`fine`), takes its first steps toward the light short (lumps and billows shade each
//    other), darkens thin parts seen away from the light (`powder`), lets more light into their insides while the
//    eye is in the layer (`inside`), and reads where they are from a finer picture round the camera. All of that
//    comes in over the 40 units under the layer, so nothing jumps where the march goes over to the zoom's.
//  - The frames before are found by where the clouds are, not by their direction alone, and kept less while the eye
//    moves against their distance (`letGo`): the eye goes through them, and they change fast as it does.
//  - Lay puts the clouds before what was drawn over the scene (after the rain, before the fog of war); the sky's
//    pass lays those against the sky as before.
// Not for a cinematic camera (cinecam.h): it keeps the clouds over its eye as without the zoom.
#pragma once

#include <string>
#include <vector>

#include "camera.h"
#include "clouds.h"
#include "gfx.h"
#include "view.h"

namespace render {

class CloudZoom {
 public:
  explicit CloudZoom(gfx::Device* device) : device_(device) {}

  // The clouds' zoom for this frame (Clouds::zoom) from where the camera is; `on`: the option, with volumetric clouds
  // drawn. Off for a cinematic camera.
  Clouds::Zoom For(const Camera& camera, const Clouds& clouds, bool on) const;
  // Over the scene's colour (a pass of it begun, colour only): the clouds that lie before what was drawn, from the
  // clouds' picture of this frame (Clouds::Picture, when Clouds::Zoomed). `sceneDepth`: the scene's depth.
  void Lay(const View& view, gfx::Texture clouds, gfx::Texture sceneDepth);
  // [zoomhitch] Lay's pipeline made now (World::PrepareAhead, on the device's own thread), not the first frame the eye
  // rises into the clouds (some 560-610 away on SETONS_R); once: after that it returns at once.
  void PrepareAhead();

  // The look, changeable between frames (the viewer's --cloud-zoom-look name=value,...: Set).
  float band = 900;          // over how far above the layer's top they thin to the most (world units)
  float erodeMost = 0.5f;    // at its end: how much of their outer parts is eaten (Clouds::Zoom::erode)
  float keepMost = 0.08f;    // and what of their density is kept (a thin veil)
  float partErode = 0.3f;    // round the view's middle, once the eye is in the layer: eaten ...
  float partKeep = 0.3f;     // ... and kept
  float partDepth = 0;        // ... coming in from this far under the layer's top (world units; 0: from the top, whole 60 over it)
  float partFrom = 0.35f, partTo = 1.15f;  // the parting whole up to this far off the middle, gone from this far (half heights)
  float near = 60;           // clouds nearer the eye than this fade away (world units)
  float gain = 2.5f;         // seen from close: the density times this (crisper outlines) ...
  float lumps = 0.5f;        // ... their tops raised and lowered in lumps this far (in the layer's thickness) ...
  float powder = 0.5f;       // ... their thin outer parts darker seen away from the light (the billows' edges) ...
  float fine = 0.9f;         // ... and a finer detail biting this deep into the outer parts
  float fineSize = 0.2f;     // its size against the detail's
  float history = 0.85f;     // how much of the frame before a pixel keeps (Clouds::keep's, as without the zoom) ...
  float letGo = 10;          // ... less as the eye moves: none once it has gone 1 / this of the clouds' distance
  float inside = 0.5f;       // the least of the falling light deep inside them (light scattered on and on; 0.3 without the zoom) ...
  float insideFade = 0.12f;  // ... fading by this share of their depth toward the light (0.25 without)
  // (sky) The layer lifted (Chris, 2026-10-20: "lift that much higher cause I think its too close to the ground and can
  // make things blurry"): with the zoom on, its underside lies at `lift` times the map's larger side over the ground's
  // zero (at least where it lies without the zoom, Clouds::base's 150; at most `liftMost`), so that the eye goes up
  // through it only far out (on a 20 km map from some 600 over the ground, the map mostly in view), and the zooms
  // played at see the land clear under the sky. Without the zoom the clouds lie where they did.
  float lift = 0.6f;
  float liftMost = 1500;
  // The clouds' underside for this frame (World, before their Prepare): lifted while `on` (the option, with volumetric
  // clouds, whatever the camera: a cinematic one sees them where the game's does), else as the clouds had it the first
  // time this was asked.
  void Place(Clouds* clouds, bool on, float mapWidth, float mapHeight);
  // Sets those named in a list "band=900,keep=0.08,..." (the names as above, without "Most"); false and why when one is wrong.
  bool Set(const std::string& list, std::string* why);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  bool tried_ = false;
  float ownBase_ = -1;  // (sky) the clouds' own underside, kept the first time Place is asked
};

// For the clouds' Build (clouds.cpp): the zoom's own march through the layer, MarchZoom (the HLSL in cloud_zoom.cpp),
// made from the clouds' shader source (`cloudsSource`: its noise, coverage and helpers, with the view's block before)
// with the zoom's part put after it; none, and why printed, when it cannot be made. Its numbers are in a constant
// buffer of its own (b2, CloudZoomMarchNumbers), the finer coverage picture round the camera it reads first at t5;
// the march without the zoom and its constants are the clouds' own, untouched.
gfx::Pipeline MakeCloudZoomMarch(gfx::Device* device, const std::string& cloudsSource);
struct CloudZoomMarchNumbers {
  float look[4], part[4], more[4], gain[4], eye[4], light[4];  // (as the shader's CloudZoomMarch says)
};
// Those numbers for a frame: the zoom (Clouds::zoom), the layer's underside, the camera, where the eye was the frame
// before, the view's size.
CloudZoomMarchNumbers CloudZoomNumbers(const Clouds::Zoom& zoom, float base, const Camera& camera, Vec3 lastEye, int width, int height);
// Seen from a few hundred units above, the clouds' coverage picture (31 world units a pixel, read between them) is
// coarse for their outlines: the zoom's march reads a finer one round the camera first, of this many pixels across
// this many world units (6 a pixel).
inline constexpr int kCloudZoomCoverSize = 1024;
inline constexpr float kCloudZoomCoverReach = 6000;

// A zoom of the game's camera along a path (the viewer's --cam-zoom-path "t:zoom;t:zoom;..."), to see the zoom through
// the clouds as a player makes it: at t seconds of the run's clock the zoom given, in between from one to the next by
// its logarithm, eased at each key; the camera leans, widens and stands off as the original's camera does with its
// zoom (fa-remaster-ui's worldview.cpp, from the original's console variables): the zoom is how many map units the
// view shows across its longer side, from 5 to the farthest (the map takes 1 / 1.4 of the view: "max" in the path),
// the lean 40 degrees over the ground nearest to straight down farthest, the field of view across the longer side
// 65 degrees nearest to 60 farthest. The heading and the point looked at stay the camera's.
struct ZoomPath {
  struct Key {
    float time, zoom;  // zoom <= 0: the farthest
  };
  std::vector<Key> keys;
  bool Parse(const std::string& text, std::string* why);
  // The camera at that time, for a map of that size seen in a view of that width / height.
  void Apply(float time, float mapWidth, float mapHeight, float aspect, Camera* camera) const;
  float End() const { return keys.empty() ? 0 : keys.back().time; }
};

}  // namespace render
