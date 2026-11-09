// What build this program is: the commit it was built from ("+changes" when the folder had uncommitted changes),
// the build's time and the processor level, as the build script passed it (tools/build_zig.py: -DMOHO64_BUILD_STAMP
// on build_stamp.cpp alone, compiled on every build). "unstamped build" from a build that does not pass it. Named
// by --version, by the first line of a default log (core/log.h LogOpenDefault) and by a crash (core/crash.h), so
// that a log a player sends in says which program wrote it.
#pragma once

namespace moho {

const char* BuildStamp();

}  // namespace moho
