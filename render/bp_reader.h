// Reads a blueprint file (.bp) as plain data, without running Lua: enough of the table syntax those
// files use ("UnitBlueprint { Display = { Mesh = { LODs = { { ... } } }, UniformScale = 0.07 } }").
// The viewer uses it to find a unit's mesh and textures. In the running game the simulation has the
// real, merged blueprints; this reader does not see what scripts change after loading.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace render {

struct BpValue {
  enum Kind { Nil, Number, String, Bool, Table } kind = Nil;
  double number = 0;
  std::string string;
  bool boolean = false;
  std::vector<std::pair<std::string, BpValue>> fields;  // Name = value
  std::vector<BpValue> items;                           // values without a name, in order

  // A named field, or a shared Nil value when there is none (so lookups can be chained).
  const BpValue& operator[](std::string_view name) const;
  const BpValue& Item(size_t index) const;
  double NumberOr(double fallback) const { return kind == Number ? number : fallback; }
  std::string StringOr(const std::string& fallback) const { return kind == String ? string : fallback; }
  bool IsTable() const { return kind == Table; }
};

// The table of the first "Name { ... }" statement in the file. False when none can be read.
bool ReadBlueprintFile(std::string_view text, BpValue* out);

}  // namespace render
