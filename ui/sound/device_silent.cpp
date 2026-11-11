// The mixer run by the clock, with no sound output (sound/device.h). Ours.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "sound/device.h"
#include "sound/mixer.h"

namespace moho::ui::sound {

void SilentClock::Reset() { last_ = std::chrono::steady_clock::now(); }

int SilentClock::Step(Mixer* mixer) {
  using Seconds = std::chrono::duration<double>;
  const auto now = std::chrono::steady_clock::now();
  double owed = Seconds(now - last_).count();
  if (owed > 0.25) {
    owed = 0.25;
    last_ = now - std::chrono::milliseconds(250);
  }
  const int frames = static_cast<int>(owed * Mixer::kRate);
  if (frames <= 0) return 0;
  // (by the frames mixed, not to `now`: the part of a frame left over is owed to the next step)
  last_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(Seconds(static_cast<double>(frames) / Mixer::kRate));
  scratch_.resize(static_cast<size_t>(frames) * 2);
  mixer->Mix(scratch_.data(), frames);
  return frames;
}

namespace {

std::atomic<int> g_testLossAfterMs{-1}, g_testLossAwayMs{0};

class SilentDevice : public OutputDevice {
 public:
  explicit SilentDevice(Mixer* mixer) : mixer_(mixer) { thread_ = std::thread([this] { Run(); }); }
  ~SilentDevice() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  std::string State() const override { return "no output: the mixer on the clock, " + std::to_string(frames_.load() * 1000 / Mixer::kRate) + " ms"; }

 private:
  // A step every hundredth of a second, as a device asks.
  void Run() {
    SilentClock clock;
    clock.Reset();
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
      wake_.wait_for(lock, std::chrono::milliseconds(10));
      if (!running_) break;
      lock.unlock();
      frames_ += clock.Step(mixer_);
      lock.lock();
    }
  }

  Mixer* mixer_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool running_ = true;
  std::atomic<long long> frames_{0};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<OutputDevice> OpenSilentDevice(Mixer* mixer) { return std::make_unique<SilentDevice>(mixer); }

void SetDeviceTestLoss(int afterMs, int awayMs) {
  g_testLossAfterMs = afterMs;
  g_testLossAwayMs = awayMs;
}
void DeviceTestLoss(int* afterMs, int* awayMs) {
  *afterMs = g_testLossAfterMs;
  *awayMs = g_testLossAwayMs;
}

}  // namespace moho::ui::sound
