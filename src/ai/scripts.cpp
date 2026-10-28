// FA Remastered's own AI: its Lua given to the sim state of a session that has it (see scripts.h).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code (docs/ai-architecture.md 0).
#include "ai/scripts.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace moho {
namespace ai {

bool JoinOurAiScripts(ScriptState& st) {
  std::string folder;
  if (const char* d = std::getenv("MOHO64_AI_DIR"); d && *d) {
    folder = d;
  } else if (const std::string exe = hostfs::ProgramFolder(); !exe.empty()) {
    for (const std::string& c : {exe + "/data/ai", exe + "/../data/ai"})
      if (folder.empty() && hostfs::IsDirectory(c)) folder = c;
  }
  const std::string root = folder.empty() ? std::string() : hostfs::Resolve(folder);
  if (root.empty() || !hostfs::IsDirectory(root)) {
    Logf(LogLevel::Warning, "moho64 ai: an army plays our own AI, but its folder (data/ai) is not beside the program or "
                            "one folder up (MOHO64_AI_DIR names it): its scripts are missing");
    return false;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::u8path(root);
  std::map<std::string, ScriptState::ExtraHook> hooks, own;
  int skipped = 0;
  // (recursive_directory_iterator's order is the file system's; the maps sort by path, so the result does not
  // depend on it)
  for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::u8string ext = it->path().extension().generic_u8string();
    if (hostfs::ToLower(std::string(ext.begin(), ext.end())) != ".lua") continue;
    const std::u8string relu = fs::relative(it->path(), base, ec).generic_u8string();
    const std::string rel(relu.begin(), relu.end());
    const std::u8string hostu = it->path().generic_u8string();
    std::ifstream f(it->path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ScriptState::ExtraHook file{hostfs::DisplayPath(std::string(hostu.begin(), hostu.end())), std::move(text)};
    const std::string lower = hostfs::ToLower(rel);
    if (lower.rfind("hook/", 0) == 0) {
      hooks[Vfs::Normalize("/" + rel.substr(5))] = std::move(file);
      continue;
    }
    const std::string vpath = Vfs::Normalize("/" + rel);
    if (st.vfs()->Exists(vpath)) {
      Logf(LogLevel::Warning, "moho64 ai: %s is a game file already: data/ai's is left out", vpath.c_str());
      ++skipped;
      continue;
    }
    own[vpath] = std::move(file);
  }
  st.AddExtraHooks(hooks);
  st.SetOwnFiles(std::move(own));
  Logf(LogLevel::Info, "moho64 ai: our own AI's scripts from %s: %zu hook file(s), %zu script(s)%s",
       hostfs::DisplayPath(root).c_str(), hooks.size(), st.OwnFileCount(), skipped ? " (some left out)" : "");
  return true;
}

}  // namespace ai
}  // namespace moho
