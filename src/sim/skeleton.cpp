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

void QuatFromRotationMatrixExe(const float M[3][3], float q[4]) {
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
}

Quat QuatFromAxesExe(const float A[9]) {
  float M[3][3];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) M[c][r] = A[r * 3 + c];  // (0x4f0b10..0x4f0b2d)
  float q[4];
  QuatFromRotationMatrixExe(M, q);
  return {q[1], q[2], q[3], q[0]};
}

void AxesFromQuatExe(const Quat& q, float A[9]) {
  const float tw = q.w * 2.0f, ty = q.y * 2.0f, tz = q.z * 2.0f, tx = q.x * 2.0f;
  const float tyy = ty * q.y, tzz = tz * q.z, tzx = tz * q.x, tyx = ty * q.x, txx = tx * q.x;
  const float twx = tw * q.x, tzy = tz * q.y, twy = tw * q.y, twz = tw * q.z;
  A[0] = 1.0f - (tzz + tyy);
  A[1] = twz + tyx;
  A[2] = tzx - twy;
  A[3] = tyx - twz;
  A[4] = 1.0f - (tzz + txx);
  A[5] = tzy + twx;
  A[6] = twy + tzx;
  A[7] = tzy - twx;
  A[8] = 1.0f - (tyy + txx);
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
  std::vector<Quat> restInvQ(static_cast<size_t>(total));  // bone+0x24 as the exe keeps it (not inverted)
  std::vector<Vec3> restInvP(static_cast<size_t>(total));
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
      QuatFromRotationMatrixExe(M, q);
      // 0x46fbf0, the inverse: the conjugate, and the negated translation turned by it (0x452d40)
      const Quat iq{-0.0f - q[1], -0.0f - q[2], -0.0f - q[3], q[0]};
      b.entRot = iq;
      b.entPos = vm::RotateM(iq, Vec3{-0.0f - ip.x, -0.0f - ip.y, -0.0f - ip.z});
      restInvQ[static_cast<size_t>(i)] = Quat{q[1], q[2], q[3], q[0]};
      restInvP[static_cast<size_t>(i)] = ip;
    }
    sk->byName_.emplace(Lower(b.name), i);
  }
  // CAniSkel::UpdateBoneBounds 0x54a540 (called last by CAniSkel::CAniSkel 0x54a335): every bone's box zeroed
  // (0x54a5a0..0x54a5fa), then for each vertex (0x44 bytes) the bone at its byte +0x40: the vertex's place turned by
  // that bone's +0x24 rotation (0x54a653) plus its translation (0x54a658..0x54a678), min (0x54a661..0x54a6a6: kept
  // unless the point is not greater) and max (0x54a6ab..0x54a6ec). A bone index past the bones is the original's
  // error (0x54a716); here that vertex is passed over.
  {
    const int32_t vertexOffset = Read<int32_t>(d, 16), vertexCount = Read<int32_t>(d, 24);
    std::vector<Vec3> mn(static_cast<size_t>(total)), mx(static_cast<size_t>(total));
    if (vertexOffset >= 48 && vertexCount > 0 &&
        static_cast<size_t>(vertexOffset) + static_cast<size_t>(vertexCount) * 68 <= d.size()) {
      for (int v = 0; v < vertexCount; ++v) {
        const size_t at = static_cast<size_t>(vertexOffset) + static_cast<size_t>(v) * 68;
        const int bi = static_cast<unsigned char>(d[at + 64]);
        if (bi >= total) continue;
        const size_t b = static_cast<size_t>(bi);
        const Vec3 r = vm::RotateM(restInvQ[b], Vec3{Read<float>(d, at), Read<float>(d, at + 4), Read<float>(d, at + 8)});
        const Vec3 l{restInvP[b].x + r.x, restInvP[b].y + r.y, restInvP[b].z + r.z};
        mn[b] = {l.x > mn[b].x ? mn[b].x : l.x, l.y > mn[b].y ? mn[b].y : l.y, l.z > mn[b].z ? mn[b].z : l.z};
        mx[b] = {l.x > mx[b].x ? l.x : mx[b].x, l.y > mx[b].y ? l.y : mx[b].y, l.z > mx[b].z ? l.z : mx[b].z};
      }
    }
    // (InitPivotFromBoneBounds 0x634777..0x6347b2: (min + max) x 0.5 per axis)
    for (size_t b = 0; b < static_cast<size_t>(total); ++b)
      sk->bones_[b].boundsCenter = {(mn[b].x + mx[b].x) * 0.5f, (mn[b].y + mx[b].y) * 0.5f, (mn[b].z + mx[b].z) * 0.5f};
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
