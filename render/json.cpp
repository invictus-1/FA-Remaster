#include "json.h"

#include <cstdlib>

namespace render {

const JsonValue& JsonValue::operator[](const std::string& name) const {
  static const JsonValue kNone;
  if (kind != Object) return kNone;
  for (const auto& m : members)
    if (m.first == name) return m.second;
  return kNone;
}

namespace {

struct Reader {
  const std::string& s;
  size_t at = 0;
  std::string why;

  void Space() {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
  }
  bool Fail(const char* what) {
    if (why.empty()) why = std::string(what) + " at byte " + std::to_string(at);
    return false;
  }
  bool Word(const char* w) {
    size_t n = 0;
    while (w[n]) ++n;
    if (s.compare(at, n, w) != 0) return false;
    at += n;
    return true;
  }
  void Utf8(unsigned code, std::string* out) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xc0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      out->push_back(static_cast<char>(0xe0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  bool Text(std::string* out) {
    if (at >= s.size() || s[at] != '"') return Fail("a string expected");
    ++at;
    while (at < s.size() && s[at] != '"') {
      char c = s[at++];
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (at >= s.size()) return Fail("a string cut short");
      c = s[at++];
      switch (c) {
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': out->push_back('\r'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          if (at + 4 > s.size()) return Fail("a \\u cut short");
          Utf8(static_cast<unsigned>(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16)), out);
          at += 4;
          break;
        }
        default: out->push_back(c);  // (\" \\ \/)
      }
    }
    if (at >= s.size()) return Fail("a string not closed");
    ++at;
    return true;
  }
  bool Value(JsonValue* v, int depth) {
    if (depth > 64) return Fail("nested too deep");
    Space();
    if (at >= s.size()) return Fail("a value expected");
    const char c = s[at];
    if (c == '{') {
      ++at;
      v->kind = JsonValue::Object;
      Space();
      if (at < s.size() && s[at] == '}') return ++at, true;
      for (;;) {
        Space();
        std::string name;
        if (!Text(&name)) return false;
        Space();
        if (at >= s.size() || s[at] != ':') return Fail("':' expected");
        ++at;
        JsonValue member;
        if (!Value(&member, depth + 1)) return false;
        v->members.emplace_back(std::move(name), std::move(member));
        Space();
        if (at < s.size() && s[at] == ',') {
          ++at;
          continue;
        }
        if (at < s.size() && s[at] == '}') return ++at, true;
        return Fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      ++at;
      v->kind = JsonValue::Array;
      Space();
      if (at < s.size() && s[at] == ']') return ++at, true;
      for (;;) {
        JsonValue item;
        if (!Value(&item, depth + 1)) return false;
        v->items.push_back(std::move(item));
        Space();
        if (at < s.size() && s[at] == ',') {
          ++at;
          continue;
        }
        if (at < s.size() && s[at] == ']') return ++at, true;
        return Fail("',' or ']' expected");
      }
    }
    if (c == '"') {
      v->kind = JsonValue::String;
      return Text(&v->text);
    }
    if (Word("true")) return v->kind = JsonValue::Bool, v->boolean = true, true;
    if (Word("false")) return v->kind = JsonValue::Bool, v->boolean = false, true;
    if (Word("null")) return v->kind = JsonValue::Null, true;
    char* end = nullptr;
    const double n = std::strtod(s.c_str() + at, &end);
    if (end == s.c_str() + at) return Fail("a value expected");
    at = static_cast<size_t>(end - s.c_str());
    v->kind = JsonValue::Number;
    v->number = n;
    return true;
  }
};

}  // namespace

bool ReadJson(const std::string& text, JsonValue* out, std::string* error) {
  Reader r{text};
  *out = JsonValue{};
  if (!r.Value(out, 0)) {
    if (error) *error = r.why;
    return false;
  }
  r.Space();
  if (r.at != text.size()) {
    if (error) *error = "more after the value at byte " + std::to_string(r.at);
    return false;
  }
  return true;
}

}  // namespace render
