// Test operations use game semantics. Only adapters include application window APIs.
#pragma once
#include <string>
#include <vector>

struct lua_State;
namespace moho::ui {
class Window;
namespace testing {

struct Key {
  int virtualCode = 0;
  int character = 0;  // printable US character; special characters come from the window
};
bool ResolveKey(const std::string& name, Key* key);
bool DecodeText(const std::string& utf8, std::vector<int>* characters);
int SpecialCharacter(int virtualCode, bool ctrl);

struct Operation {
  enum Kind { KeyDown, KeyUp, Text, Move, ButtonDown, ButtonUp, Wheel, Resize } kind;
  Key key;
  std::string text;
  bool fromKey = false;
  bool shift = false, ctrl = false, alt = false, primary = false;
  float x = 0, y = 0;
  int button = 1, clicks = 1, notches = 0;
  int buttons = 0;  // portable left/middle/right bit mask
  float dx = 0, dy = 0;
};

// Window operations require an active application window.
class WindowScope {
 public:
  explicit WindowScope(Window* window);
  ~WindowScope();
 private:
  Window* previous_;
};
Window* ActiveWindow();
const char* WindowAdapterName();
bool InjectWindow(Window& window, const Operation& operation, std::string* error);
void RegisterTestBindings(lua_State* L);

}  // namespace testing
}  // namespace moho::ui
