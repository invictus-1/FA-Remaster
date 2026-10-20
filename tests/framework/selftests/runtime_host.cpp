#include <cstdio>
#include <string>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "tests/framework/host/input_adapter.h"
#include "tests/framework/host/results.h"
#include "user/user_state.h"

int main(int argc, char** argv) {
  if (argc != 7) return 2;
  auto framework = moho::hostfs::ReadFile(argv[1]);
  auto script = moho::hostfs::ReadFile(argv[5]);
  if (!framework || !script) return 2;
  moho::ui::testing::Results results(argv[2], argv[3], argv[4], "lua");
  moho::LogObserverScope observer([&](moho::LogLevel level, moho::LogKind kind, std::string_view message) {
    if (level == moho::LogLevel::Error || kind == moho::LogKind::ScriptError) results.Error(std::string(message));
  });
  moho::Vfs vfs;
  bool ok;
  {
    moho::ui::UserState::Config config;
    config.registerBindings = [&](lua_State* L) {
      moho::ui::testing::RegisterTestBindings(L);
      results.Register(L);
    };
    config.afterFrame = [&](lua_State* L) { results.Poll(L); };
    config.beforeStateClose = [&](lua_State* L) { results.CloseState(L); };
    moho::ui::UserState user(&vfs, config);
    ok = user.RunString(R"(
      WaitFrames = function(n) coroutine.yield((n or 1) + 1) end
      import = function(path)
        if path == '/testing/framework.lua' then return framework end
        if path == '/lua/lazyvar.lua' then
          return {Create = function(value)
            return setmetatable({value, SetValue = function(self, nextValue) self[1] = nextValue end},
                                {__call = function(self) return self[1] end})
          end}
        end
        if path == '/lua/maui/frame.lua' then
          return {Frame = function()
            local frame = setmetatable({}, {__index = function(self, key)
              return moho.frame_methods[key] or moho.control_methods[key]
            end})
            InternalCreateFrame(frame)
            return frame
          end}
        end
        error('unexpected import: ' .. path)
      end
    )", "=(host fixture)");
    ok = ok && user.ui().CreateRootFrames(user.L(), 800, 600);
    ok = ok && user.RunString("framework = (function()\n" + *framework + "\nend)()", "@framework.lua");
    ok = ok && user.RunString(*script, std::string("@") + argv[5]);
    const int frames = std::stoi(argv[6]);
    for (int i = 0; ok && i < frames && !moho::ui::ExitRequested(); ++i) user.Frame(1.0 / 60.0);
  }
  return results.Exit(ok ? 0 : 1);
}
