#pragma once
#include <map>
#include <string>

#include "script/script_handle.h"

namespace moho {

class Vfs;

// Headless resource retention for the simulation's CPrefetchSet API.
class PrefetchSet : public ScriptObject {
 public:
  std::map<std::string, std::string> resources;
  void Update(lua_State* L, int data, const Vfs& vfs);
};

void RegisterPrefetchBindings(lua_State* L);

}  // namespace moho
