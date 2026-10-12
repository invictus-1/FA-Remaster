// The sound device: a thread of its own asks the mixer for the next piece of sound, a hundredth
// of a second at a time, and hands it to the system's sound output.
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
