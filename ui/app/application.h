#pragma once
#include <functional>

struct lua_State;
namespace moho {
class Vfs;
namespace ui {
class Window;

struct ApplicationHost {
  std::function<bool(Vfs&)> mountResources;
  std::function<void(lua_State*)> registerBindings;
  std::function<void(lua_State*)> afterFrame;
  std::function<void(lua_State*)> beforeStateClose;
  std::function<bool()> readyForGame;
  // Non-null while the window is alive; null before its destruction.
  std::function<void(Window*)> windowChanged;
};

int RunApplication(int argc, char** argv, const ApplicationHost& host = {});

}  // namespace ui
}  // namespace moho
