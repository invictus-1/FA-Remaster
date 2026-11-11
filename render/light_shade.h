// [lightshade] The ground's shadow on the larger small lights (lights.h, Lights::shade), as HLSL source: the one
// function every shader that reads the lights' buffer asks, spliced into lighting.h's block and ray_scene.h's (whichever
// comes first in a shader declares the buffer, and this with it).
#pragma once

#define RENDER_LIGHT_SHADE_BLOCK R"hlsl(
// [lightshade] How much of a light to all sides the ground lets through to the place p (1 all, 0 none; lights.h: the larger
// lights' ground tables). `beam`: the light's third row (x: the row its table begins at, 0: it has none; y: how strongly
// the table counts). The table's first row says where its middle lies (x, z: where the light stood when it was made),
// how wide its squares are and how many it has each way (an even number); then two heights a square (two squares a
// row), the squares laid over the ground the light reaches: nothing of the light is seen from below the first, all of
// it from above the second; read between the four squares round the place. The place is taken a little higher than it
// is (the rays' own rise, lighting.h's kRayGroundRise, and three tenths of a square's width: what the squares smooth
// away of the ground; measured against the map's heights marched exactly, less lets flat ground shadow itself), and
// the step from none to all is never thinner than about as much: ground that merely rolls is not shadowed by its own
// bumps, and a shadow's edge is soft however near the ridge.
float LightShade(float4 beam, float3 p) {
  [branch] if (beam.x < 0.5) return 1;
  float4 head = lightData[uint(beam.x)];
  float cell = head.z;
  float2 g = clamp((p.xz - head.xy) / cell + (0.5 * head.w - 0.5), 0.0, head.w - 1.001);
  float2 f = floor(g), t = g - f;
  uint n = uint(head.w), i = uint(f.y) * n + uint(f.x), at = uint(beam.x) + 1;
  float4 r0 = lightData[at + (i >> 1)], r1 = lightData[at + ((i + 1) >> 1)], r2 = lightData[at + ((i + n) >> 1)], r3 = lightData[at + ((i + n + 1) >> 1)];
  bool odd = (i & 1) != 0;
  float2 lines = lerp(lerp(odd ? r0.zw : r0.xy, odd ? r1.xy : r1.zw, t.x), lerp(odd ? r2.zw : r2.xy, odd ? r3.xy : r3.zw, t.x), t.y);
  float seen = saturate((p.y + 0.35 + 0.3 * cell - lines.x) / max(lines.y - lines.x, 0.5 + 0.3 * cell));
  return 1 - beam.y * (1 - seen * seen * (3 - 2 * seen));
}
)hlsl"
