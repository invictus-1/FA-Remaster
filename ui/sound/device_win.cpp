#include "sound/device.h"

#include "sound/mixer.h"

#ifdef _WIN32
#include <windows.h>

#include <mmsystem.h>

#include <atomic>
#include <thread>
#include <vector>

namespace moho::ui::sound {
namespace {

// Windows' wave output: a few small buffers go round; whenever the system has played one, it is
// filled again from the mixer and handed back. Small buffers keep the time between a click and
// its sound short (three of a hundredth of a second each are under way at most).
class WaveOutDevice : public OutputDevice {
 public:
  static const int kFrames = Mixer::kRate / 100;
  static const int kBuffers = 3;

  explicit WaveOutDevice(Mixer* mixer) : mixer_(mixer) {}

  bool Open() {
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = Mixer::kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = Mixer::kRate * 4;
    event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return false;
    if (waveOutOpen(&out_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
      out_ = nullptr;
      return false;
    }
    for (int i = 0; i < kBuffers; ++i) {
      data_[i].assign(static_cast<size_t>(kFrames) * 2, 0);
      headers_[i] = {};
      headers_[i].lpData = reinterpret_cast<LPSTR>(data_[i].data());
      headers_[i].dwBufferLength = static_cast<DWORD>(kFrames * 4);
      waveOutPrepareHeader(out_, &headers_[i], sizeof(WAVEHDR));
      headers_[i].dwFlags |= WHDR_DONE;  // free to be filled
    }
    running_ = true;
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  ~WaveOutDevice() override {
    running_ = false;
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (out_) {
      waveOutReset(out_);
      for (int i = 0; i < kBuffers; ++i) waveOutUnprepareHeader(out_, &headers_[i], sizeof(WAVEHDR));
      waveOutClose(out_);
    }
    if (event_) CloseHandle(event_);
  }

 private:
  void Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (running_) {
      for (int i = 0; i < kBuffers; ++i) {
        if (!(headers_[i].dwFlags & WHDR_DONE)) continue;
        headers_[i].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        mixer_->Mix(data_[i].data(), kFrames);
        waveOutWrite(out_, &headers_[i], sizeof(WAVEHDR));
      }
      WaitForSingleObject(event_, 50);
    }
  }

  Mixer* mixer_;
  HWAVEOUT out_ = nullptr;
  HANDLE event_ = nullptr;
  WAVEHDR headers_[kBuffers] = {};
  std::vector<int16_t> data_[kBuffers];
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer* mixer) {
  auto device = std::make_unique<WaveOutDevice>(mixer);
  if (!device->Open()) return nullptr;
  return device;
}

}  // namespace moho::ui::sound

#else

namespace moho::ui::sound {
std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer*) { return nullptr; }
}  // namespace moho::ui::sound

#endif
