#include <cstdio>

#include "core/log.h"
#include "tests/framework/host/options.h"
#include "tests/framework/host/results.h"

int RunTestApplication(int argc, char** argv);

int main(int argc, char** argv) {
  try {
    moho::ui::testing::HostOptions options(argc, argv);
    if (options.kind != "command") throw std::runtime_error("this host runs command checks");
    moho::ui::testing::Results results(options.path, options.run, options.name, options.kind);
    moho::LogObserverScope observer([&](moho::LogLevel level, moho::LogKind kind, std::string_view message) {
      if (level == moho::LogLevel::Error || kind == moho::LogKind::ScriptError) results.Error(std::string(message));
    });
    return results.Exit(RunTestApplication(options.argc(), options.args.data()));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "test host: %s\n", error.what());
    return 2;
  }
}
