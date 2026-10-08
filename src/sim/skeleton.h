// Skeletons: the bones of an entity's mesh (.scm), in the order the file lists them (bone
// indices in scripts are that order; bone 0 is the root).
//
// .scm (version 5): "MODL", then 11 ints - version, bone offset, weighted bone count, vertex
// offset, extra vertex offset, vertex count, index offset, index count, info offset, info count,
// total bone count. Each bone is 108 bytes: rest-pose inverse (4x4 floats), position (3 floats)
// and rotation (w, x, y, z) relative to the parent, name offset, parent index (-1 for the root)
// and two reserved ints.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/entity.h"

namespace moho {

class Vfs;

struct Bone {
  std::string name;
  int parent = -1;
  Vec3 localPos;   // relative to the parent
  Quat localRot;
  Vec3 modelPos;   // rest pose in model space (before the mesh's uniform scale)
  Quat modelRot;
};

class Skeleton {
 public:
  // nullptr if the data is not a readable .scm.
  static std::unique_ptr<Skeleton> FromScm(const std::string& data);

  const std::vector<Bone>& bones() const { return bones_; }
  int Count() const { return static_cast<int>(bones_.size()); }
  // Bone index by name (case-insensitive), or -1.
  int Find(const std::string& name) const;

 private:
  std::vector<Bone> bones_;
  std::map<std::string, int> byName_;  // lower-case name -> first bone with it
};

// Loaded skeletons by mesh file (each file read once).
class SkeletonCache {
 public:
  explicit SkeletonCache(Vfs* vfs) : vfs_(vfs) {}
  const Skeleton* Get(const std::string& meshFile);  // nullptr if missing or unreadable

 private:
  Vfs* vfs_;
  std::map<std::string, std::unique_ptr<Skeleton>> cache_;
};

Quat QuatMul(const Quat& a, const Quat& b);
Vec3 QuatRotate(const Quat& q, const Vec3& v);

}  // namespace moho
