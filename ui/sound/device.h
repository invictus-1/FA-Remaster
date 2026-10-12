// The sound device: a thread of its own asks the mixer for the next piece of sound whenever the
// system's sound output has room for it (about every hundredth of a second).
#pragma once
#include <memory>

namespace moho::ui::sound {

class Mixer;

class OutputDevice {
 public:
  virtual ~OutputDevice() = default;
};
// nullptr when the system has no sound output (or on a system this is not built for).
std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer* mixer);

}  // namespace moho::ui::sound
