#include "core/build_stamp.h"

#ifndef MOHO64_BUILD_STAMP
#define MOHO64_BUILD_STAMP "unstamped build"
#endif

namespace moho {

const char* BuildStamp() { return MOHO64_BUILD_STAMP; }

}  // namespace moho
