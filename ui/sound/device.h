// The sound device: a thread of its own asks the mixer for the next piece of sound whenever the
// system's sound output has room for it (about every hundredth of a second).
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace moho::ui::sound {

class Mixer;

class OutputDevice {
 public:
  virtual ~OutputDevice() = default;
  // For checks and the log: what the device is doing and has been through, in a few words.
  virtual std::string State() const { return std::string(); }
};
// nullptr when the system has no sound output (or on a system this is not built for).
// On Windows the device follows the system's default output (ours; the original leaves its output to its sound
// library): when its output is taken away (a headset pulled, the device switched off) or another output becomes
// the default, it opens the default output anew, and while there is none it waits for one with the mixer running
// on the clock (SilentClock), so sounds end and scripts that wait for them go on.
std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer* mixer);
// No output at all: a thread that asks the mixer for sound by the clock and drops it. For a live window that has no
// device (none could be opened, or --no-sound): without a running mix no voice ever ends, so the categories' limits
// fill up and a script that waits for a sound's end (FAF's music thread) waits for ever.
std::unique_ptr<OutputDevice> OpenSilentDevice(Mixer* mixer);

// The mixer's clock without a device: Step mixes as many frames as the time since the last step (or Reset) asks
// for and drops them. A stop of more than a quarter of a second (the machine slept, the thread was held) is not
// caught up. One thread's.
class SilentClock {
 public:
  void Reset();
  int Step(Mixer* mixer);  // the frames mixed

 private:
  std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
  std::vector<int16_t> scratch_;
};

// For a check without a headset to pull (ui/tests/sound_device.lua): the Windows device opened next behaves as if
// its output were taken away `afterMs` after it opened (once), and finds no output at all for `awayMs` after that.
// afterMs < 0: off, as at first.
void SetDeviceTestLoss(int afterMs, int awayMs);
void DeviceTestLoss(int* afterMs, int* awayMs);

}  // namespace moho::ui::sound
