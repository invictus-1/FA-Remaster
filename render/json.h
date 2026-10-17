// A small JSON reader, for the visuals track's files (the terrain's remap.json): objects, arrays, strings (with the
// usual escapes; \u as UTF-8), numbers, true, false, null. Read whole; a value asked for that is not there is an
// empty one (Null), so lookups can be chained.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace render {

struct JsonValue {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<JsonValue> items;                  // an array's
  std::vector<std::pair<std::string, JsonValue>> members;  // an object's, in the file's order

  // a member of an object (an empty value if this is none, or has none of that name)
  const JsonValue& operator[](const std::string& name) const;
  double NumberOr(double fallback) const { return kind == Number ? number : fallback; }
  std::string StringOr(const std::string& fallback) const { return kind == String ? text : fallback; }
};

// Reads `text` into `out`. False with where and why in `error`.
bool ReadJson(const std::string& text, JsonValue* out, std::string* error);

}  // namespace render
