#include <memory>
#include <string>
#include <cstdio>

#include "app/application.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "tests/framework/host/input_adapter.h"
#include "tests/framework/host/results.h"
#include "tests/framework/host/clipboard.h"
#include "tests/framework/host/options.h"
#include "user/user_state.h"

int main(int argc, char** argv) {
  try {
    moho::ui::testing::HostOptions options(argc, argv);
    moho::ui::testing::Results results(options.path, options.run, options.name, options.kind);
    moho::LogObserverScope observer([&](moho::LogLevel level, moho::LogKind diagnostic, std::string_view message) {
      if (level == moho::LogLevel::Error || diagnostic == moho::LogKind::ScriptError) results.Error(std::string(message));
    });
    moho::ui::ApplicationHost host;
    host.mountResources = [](moho::Vfs& vfs) {
      for (const std::string& folder : {std::string("tests/framework/lua"),
                                      moho::hostfs::ProgramFolder() + "/../tests/framework/lua"}) {
        if (moho::hostfs::IsDirectory(folder) && vfs.Mount(folder, "/testing")) return true;
      }
      moho::Logf(moho::LogLevel::Error, "test host: cannot mount tests/framework/lua");
      return false;
    };
    host.registerBindings = [&](lua_State* L) {
      moho::ui::testing::RegisterTestBindings(L);
      results.Register(L);
    };
    host.afterFrame = [&](lua_State* L) { results.Poll(L); };
    host.beforeStateClose = [&](lua_State* L) { results.CloseState(L); };
    if (options.hasPreparation && !options.path.empty()) host.readyForGame = [&]() { return results.ReadyForGame(); };
    std::unique_ptr<moho::ui::testing::WindowScope> window;
    host.windowChanged = [&](moho::ui::Window* current) {
      if (!current) moho::ui::testing::RestoreClipboard();
      window.reset();
      if (current) window = std::make_unique<moho::ui::testing::WindowScope>(current);
    };
    moho::ui::AllowTestFileWrites(true);
    int status = moho::ui::RunApplication(options.argc(), options.args.data(), host);
    moho::ui::testing::RestoreClipboard();
    return results.Exit(status);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "test host: %s\n", error.what());
    return 2;
  }
}
