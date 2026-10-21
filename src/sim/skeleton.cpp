#include "sim/skeleton.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/profile.h"
#include "core/vfs.h"
#include "sim/vecmath.h"

namespace moho {

Quat QuatMul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 QuatRotate(const Quat& q, const Vec3& v) {
  // v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
  float cx = q.y * v.z - q.z * v.y, cy = q.z * v.x - q.x * v.z, cz = q.x * v.y - q.y * v.x;
  float dx = q.y * cz - q.z * cy, dy = q.z * cx - q.x * cz, dz = q.x * cy - q.y * cx;
  return {v.x + 2 * (q.w * cx + dx), v.y + 2 * (q.w * cy + dy), v.z + 2 * (q.w * cz + dz)};
}

namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

template <typename T>
T Read(const std::string& d, size_t at) {
  T v;
  std::memcpy(&v, d.data() + at, sizeof v);
  return v;
}

}  // namespace

std::unique_ptr<Skeleton> Skeleton::FromScm(const std::string& d) {
  if (d.size() < 48 || d.compare(0, 4, "MODL") != 0) return nullptr;
  int32_t boneOffset = Read<int32_t>(d, 8);
  int32_t total = Read<int32_t>(d, 44);
  if (boneOffset < 48 || total < 0 || static_cast<size_t>(boneOffset) + static_cast<size_t>(total) * 108 > d.size())
    return nullptr;
  auto sk = std::make_unique<Skeleton>();
  sk->bones_.resize(total);
  for (int i = 0; i < total; ++i) {
    size_t at = boneOffset + static_cast<size_t>(i) * 108 + 64;  // after the rest-pose inverse
    Bone& b = sk->bones_[i];
    b.localPos = {Read<float>(d, at), Read<float>(d, at + 4), Read<float>(d, at + 8)};
    b.localRot = {Read<float>(d, at + 16), Read<float>(d, at + 20), Read<float>(d, at + 24), Read<float>(d, at + 12)};
    int32_t nameAt = Read<int32_t>(d, at + 28);
    b.parent = Read<int32_t>(d, at + 32);
    if (nameAt >= 0 && static_cast<size_t>(nameAt) < d.size()) b.name = d.c_str() + nameAt;
    if (b.parent >= 0 && b.parent < i) {
      const Bone& p = sk->bones_[b.parent];
      b.modelRot = QuatMul(p.modelRot, b.localRot);
      Vec3 r = QuatRotate(p.modelRot, b.localPos);
      b.modelPos = {p.modelPos.x + r.x, p.modelPos.y + r.y, p.modelPos.z + r.z};
    } else {
      b.parent = -1;
      b.modelRot = b.localRot;
      b.modelPos = b.localPos;
    }
    {
      // bone+0x24: quaternion of the rest-pose inverse's 3 x 3 part (0x4f0ae0 hands 0x4f0cb0 the TRANSPOSE: Wm3's
      // Quaternion::FromRotationMatrix, in its order of operations) and its translation (the matrix's fourth row)
      const size_t m = at - 64;
      float M[3][3];
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) M[c][r] = Read<float>(d, m + static_cast<size_t>(r * 4 + c) * 4);
      const Vec3 ip{Read<float>(d, m + 48), Read<float>(d, m + 52), Read<float>(d, m + 56)};
      float q[4];  // w, x, y, z
      const float trace = (M[0][0] + M[1][1]) + M[2][2];
      if (trace > 0.0f) {
        const float root = std::sqrt(trace + 1.0f);
        q[0] = root * 0.5f;
        const float k = 0.5f / root;
        q[1] = (M[2][1] - M[1][2]) * k;
        q[2] = (M[0][2] - M[2][0]) * k;
        q[3] = (M[1][0] - M[0][1]) * k;
      } else {
        static const int next[3] = {1, 2, 0};
        int a = 0;
        if (M[1][1] > M[0][0]) a = 1;
        if (M[2][2] > M[a][a]) a = 2;
        const int b2 = next[a], c2 = next[b2];
        const float root = std::sqrt(((M[a][a] - M[b2][b2]) - M[c2][c2]) + 1.0f);
        q[a + 1] = root * 0.5f;
        const float k = 0.5f / root;
        q[0] = (M[c2][b2] - M[b2][c2]) * k;
        q[b2 + 1] = (M[a][b2] + M[b2][a]) * k;
        q[c2 + 1] = (M[a][c2] + M[c2][a]) * k;
      }
      // 0x46fbf0, the inverse: the conjugate, and the negated translation turned by it (0x452d40)
      const Quat iq{-0.0f - q[1], -0.0f - q[2], -0.0f - q[3], q[0]};
      b.entRot = iq;
      b.entPos = vm::RotateM(iq, Vec3{-0.0f - ip.x, -0.0f - ip.y, -0.0f - ip.z});
    }
    sk->byName_.emplace(Lower(b.name), i);
  }
  return sk;
}

int Skeleton::Find(const std::string& name) const {
  auto it = byName_.find(Lower(name));
  return it == byName_.end() ? -1 : it->second;
}

const Skeleton* SkeletonCache::Get(const std::string& meshFile) {
  std::string key = Lower(meshFile);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second.get();
  prof::SectionTimer sec(prof::kSecSkeletonRead);  // (MOHO64_PROFILE_HITCH only)
  std::unique_ptr<Skeleton> sk;
  if (auto data = vfs_->ReadFile(meshFile)) sk = Skeleton::FromScm(*data);
  const Skeleton* p = sk.get();
  cache_.emplace(key, std::move(sk));
  return p;
}

}  // namespace moho
