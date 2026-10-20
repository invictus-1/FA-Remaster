#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace moho::ui::testing {
struct HostOptions {
  std::string path, run = "standalone", name = "standalone", kind = "command";
  std::vector<char*> args;
  bool hasPreparation = false;
  HostOptions(int argc, char** argv) : args{argv[0]} {
    bool gameArguments = false, frontend = false, game = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      std::string* value = nullptr;
      if (!gameArguments) {
        if (arg == "--test-events") value = &path;
        else if (arg == "--test-run") value = &run;
        else if (arg == "--test-case") value = &name;
      }
      if (value) {
        if (++i == argc) throw std::runtime_error("missing value for " + arg);
        *value = argv[i];
      } else {
        if (arg == "--") gameArguments = true;
        if (!gameArguments && arg == "--lua") frontend = true;
        if (!gameArguments && arg == "--game-lua") game = true;
        args.push_back(argv[i]);
      }
    }
    kind = frontend || game ? "lua" : "command";
    hasPreparation = frontend && game;
    args.push_back(nullptr);
  }
  int argc() const { return static_cast<int>(args.size()) - 1; }
};
}
