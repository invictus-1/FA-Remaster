#include "bp_reader.h"

#include <cctype>
#include <cstdlib>

namespace render {
namespace {

const BpValue kNil;

struct Parser {
  std::string_view s;
  size_t p = 0;
  int depth = 0;

  void SkipSpace() {
    for (;;) {
      while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
      if (p + 1 < s.size() && s[p] == '-' && s[p + 1] == '-') {
        if (s.compare(p, 4, "--[[") == 0) {
          size_t e = s.find("]]", p + 4);
          p = e == std::string_view::npos ? s.size() : e + 2;
        } else {
          while (p < s.size() && s[p] != '\n') ++p;
        }
      } else if (p < s.size() && s[p] == '#') {  // the old comment sign of the game's Lua
        while (p < s.size() && s[p] != '\n') ++p;
      } else {
        return;
      }
    }
  }
  bool Eat(char c) {
    SkipSpace();
    if (p < s.size() && s[p] == c) {
      ++p;
      return true;
    }
    return false;
  }
  static bool NameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
  std::string Name() {
    SkipSpace();
    size_t b = p;
    while (p < s.size() && NameChar(s[p])) ++p;
    return std::string(s.substr(b, p - b));
  }
  std::string Quoted() {
    char q = s[p++];
    std::string out;
    while (p < s.size() && s[p] != q) {
      if (s[p] == '\\' && p + 1 < s.size()) {
        char c = s[++p];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
      } else {
        out += s[p];
      }
      ++p;
    }
    if (p < s.size()) ++p;
    return out;
  }

  // Skips whatever follows a value up to the next separator at this nesting level (an operator and
  // its operand, a call's arguments): the files hardly use expressions, and none the viewer needs.
  void SkipRest() {
    int nest = 0;
    for (; p < s.size(); ++p) {
      char c = s[p];
      if (c == '\'' || c == '"') {
        Quoted();
        --p;
      } else if (c == '{' || c == '(') {
        ++nest;
      } else if (c == '}' || c == ')') {
        if (nest == 0) return;
        --nest;
      } else if ((c == ',' || c == ';') && nest == 0) {
        return;
      }
    }
  }

  bool Value(BpValue* v) {
    SkipSpace();
    if (p >= s.size()) return false;
    char c = s[p];
    if (c == '{') return TableBody(v);
    if (c == '\'' || c == '"') {
      v->kind = BpValue::String;
      v->string = Quoted();
      return true;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '+') {
      char* end = nullptr;
      std::string text(s.substr(p, 64));
      v->number = std::strtod(text.c_str(), &end);
      if (end == text.c_str()) {  // a sign in front of something that is not a number: left as nil
        ++p;
        return true;
      }
      v->kind = BpValue::Number;
      p += static_cast<size_t>(end - text.c_str());
      return true;
    }
    if (NameChar(c)) {
      std::string name = Name();
      if (name == "true" || name == "false") {
        v->kind = BpValue::Bool;
        v->boolean = name == "true";
        return true;
      }
      if (name == "nil") return true;
      SkipSpace();
      if (p < s.size() && s[p] == '{') return TableBody(v);  // Sound { ... } and the like: keep the table
      return true;                                           // a variable or a call: left as nil, the rest is skipped
    }
    return false;
  }

  bool TableBody(BpValue* v) {
    if (!Eat('{') || ++depth > 64) return false;
    v->kind = BpValue::Table;
    for (;;) {
      SkipSpace();
      if (p >= s.size()) return false;
      if (s[p] == '}') {
        ++p;
        --depth;
        return true;
      }
      BpValue item;
      std::string key;
      bool named = false;
      size_t start = p;
      if (NameChar(s[p]) && !std::isdigit(static_cast<unsigned char>(s[p]))) {
        key = Name();
        SkipSpace();
        if (p < s.size() && s[p] == '=' && !(p + 1 < s.size() && s[p + 1] == '=')) {
          ++p;
          named = true;
        } else {
          p = start;
        }
      } else if (s[p] == '[') {  // ['key'] = value or [3] = value
        ++p;
        BpValue k;
        if (!Value(&k) || !Eat(']') || !Eat('=')) return false;
        if (k.kind == BpValue::String) {
          key = k.string;
          named = true;
        }
      }
      if (!Value(&item)) return false;
      SkipRest();
      if (named) v->fields.emplace_back(std::move(key), std::move(item));
      else v->items.push_back(std::move(item));
      SkipSpace();
      if (p < s.size() && (s[p] == ',' || s[p] == ';')) ++p;
    }
  }
};

}  // namespace

const BpValue& BpValue::operator[](std::string_view name) const {
  for (const auto& f : fields)
    if (f.first == name) return f.second;
  return kNil;
}

const BpValue& BpValue::Item(size_t index) const { return index < items.size() ? items[index] : kNil; }

bool ReadBlueprintFile(std::string_view text, BpValue* out) {
  Parser parser{text};
  // find the first "Name {"
  for (;;) {
    parser.SkipSpace();
    if (parser.p >= text.size()) return false;
    if (Parser::NameChar(text[parser.p])) {
      parser.Name();
      parser.SkipSpace();
      if (parser.p < text.size() && text[parser.p] == '{') break;
    } else {
      ++parser.p;
    }
  }
  *out = BpValue{};
  return parser.TableBody(out);
}

}  // namespace render
