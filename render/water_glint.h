#pragma once
// [shore] The sun's glint on water as a pixel takes in more of it (the renderer's task, 2026-10-21: from the strategic view
// the sea's glitter twinkled as single bright pixels frame to frame, more of it since the far sea was filled in).
//
// The water's glint is a narrow lobe round the sun's mirror image: a power of about 500 on the cosine between the ray the
// surface mirrors and the way to the sun. That is right for one facet of a wave. A pixel far off takes in many waves whose
// slopes it cannot show (the wave pictures read at a coarser level, the finer swell and ripples left out where they would
// be finer than a pixel or two): each of its facets would throw the glint a little elsewhere, so the pixel should glint over
// a wider lobe, dimmer by as much. Drawn with the one narrow lobe, it is lit fully or not at all, and from far up the few
// pixels whose one normal happens to mirror the sun flash on and off as the waves move.
//
// So the lobe is widened by the slopes a pixel takes in (by how much water it covers) and dimmed so that the light it
// throws on the whole stays the same: close in the water glitters as before, from far up it is a calm sheen where the sun's
// mirror image lies. (The lobe pow(c, n) spreads the mirrored ray's angle by a variance of about 1/n; a facet tilted by t
// turns the mirrored ray by 2t, so slopes of variance s add 4s; and the lobe holds 2 pi / (n + 1) of light over the sphere.)
//
// HLSL, after the view's block: the modern water's shader (water.cpp, World::waterDetail) and the streams' (stream_water.cpp,
// World::streamWater) take it. Off (the water without World::waterDetail): the water's glint is as it was.

namespace render {

inline const char kGlintShaderBlock[] = R"hlsl(
// The glint's lobe at `cosine` (between the mirrored ray and the way to the sun), `sharp` its power for one facet (the
// water's 500), widened by the slopes' spread `slopes` that a pixel covering `grain` world units takes in: none below
// `from`, all of it from `to`; times the share of the light that keeps its sum (1 where nothing is taken in).
float SoftGlint(float cosine, float sharp, float grain, float from, float to, float slopes) {
  float lost = slopes * smoothstep(from, to, grain);
  float n = 1 / (1 / sharp + 4 * lost);
  return pow(saturate(cosine), n) * ((n + 1) / (sharp + 1));
}
)hlsl";

}  // namespace render
