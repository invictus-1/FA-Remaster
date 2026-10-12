#include "frame.h"

#include "linalg.h"

namespace render {

void BlendUnits(const FrameUnit* a, size_t countA, const FrameUnit* b, size_t countB, float t, std::vector<FrameUnit>* out,
                uint32_t nextBoneOffset) {
  out->clear();
  out->reserve(countA);
  size_t j = 0;
  for (size_t i = 0; i < countA; ++i) {
    FrameUnit u = a[i];
    u.nextFirstBone = u.firstBone;
    u.nextBoneCount = u.boneCount;
    if (b && t > 0) {
      while (j < countB && b[j].id < u.id) ++j;
      if (j < countB && b[j].id == u.id && b[j].blueprint == u.blueprint) {
        const FrameUnit& v = b[j];
        for (int k = 0; k < 3; ++k) u.position[k] += (v.position[k] - u.position[k]) * t;
        Quat q = Nlerp({u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]},
                       {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]}, t);
        u.orientation[0] = q.x;
        u.orientation[1] = q.y;
        u.orientation[2] = q.z;
        u.orientation[3] = q.w;
        u.fractionComplete += (v.fractionComplete - u.fractionComplete) * t;
        u.health += (v.health - u.health) * t;
        u.nextFirstBone = v.firstBone + nextBoneOffset;
        u.nextBoneCount = v.boneCount;
      }
    }
    out->push_back(u);
  }
}

}  // namespace render
