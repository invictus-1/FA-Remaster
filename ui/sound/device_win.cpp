#include "sound/device.h"

#include "core/log.h"
#include "sound/mixer.h"

#ifdef _WIN32
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace moho::ui::sound {
namespace {

template <class T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// Windows' sound session of today (WASAPI, shared mode): the system's sound engine asks for sound
// in its own rhythm (every hundredth of a second) and we keep its buffer of about two such
// pieces full. That is as short a way from a click to its sound as the shared engine allows, and
// it does not crackle: the old wave output with buffers this small does (its pump found less
// than it needed at times and put silence in: heard as static over everything).
class SessionDevice : public OutputDevice {
 public:
  explicit SessionDevice(Mixer* mixer) : mixer_(mixer) {}

  bool Open() {
    event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return false;
    thread_ = std::thread([this] { Run(); });
    while (state_ == 0) Sleep(1);  // until the thread knows whether the device is there
    return state_ == 1;
  }

  ~SessionDevice() override {
    running_ = false;
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (event_) CloseHandle(event_);
    if (starved_ > 0) Logf(LogLevel::Warning, "moho64ui: sound: the device ran dry %d times", starved_.load());
  }

 private:
  bool Start() {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator_))))
      return false;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint_))) return false;
    if (FAILED(endpoint_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_)))) return false;
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = Mixer::kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = Mixer::kRate * 4;
    REFERENCE_TIME period = 100000, shortest = 0;  // in 100 ns
    client_->GetDevicePeriod(&period, &shortest);
    // Our format whatever the device's is: the system converts.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2 * period, 0, &format, nullptr))) return false;
    if (FAILED(client_->SetEventHandle(event_))) return false;
    if (FAILED(client_->GetBufferSize(&frames_))) return false;
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_)))) return false;
    Fill();
    if (FAILED(client_->Start())) return false;
    Logf(LogLevel::Info, "moho64ui: sound: device open, %.1f ms under way at most", 1000.0 * frames_ / Mixer::kRate);
    return true;
  }

  // Fill what is free of the system's buffer; false when it was empty (it had run dry).
  bool Fill() {
    UINT32 queued = 0;
    if (FAILED(client_->GetCurrentPadding(&queued)) || queued >= frames_) return true;
    UINT32 free = frames_ - queued;
    BYTE* data = nullptr;
    if (FAILED(render_->GetBuffer(free, &data))) return true;
    mixer_->Mix(reinterpret_cast<int16_t*>(data), static_cast<int>(free));
    render_->ReleaseBuffer(free, 0);
    return queued > 0;
  }

  void Run() {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    bool ok = Start();
    state_ = ok ? 1 : 2;
    while (ok && running_) {
      if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) continue;
      if (!running_) break;
      if (!Fill()) ++starved_;
    }
    if (client_ && ok) client_->Stop();
    Release(render_), Release(client_), Release(endpoint_), Release(enumerator_);
    if (com) CoUninitialize();
  }

  Mixer* mixer_;
  HANDLE event_ = nullptr;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  IMMDevice* endpoint_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  UINT32 frames_ = 0;
  std::atomic<int> state_{0};  // 0 starting, 1 playing, 2 no device
  std::atomic<bool> running_{true};
  std::atomic<int> starved_{0};
  std::thread thread_;
};

// The old wave output, for a system where the session cannot be opened: a few buffers go round;
// whenever the system has played one, it is filled again from the mixer. The buffers are not
// small (a twentieth of a second each): small ones crackle here.
class WaveOutDevice : public OutputDevice {
 public:
  WaveOutDevice(Mixer* mixer, int frames, int buffers) : mixer_(mixer), frames_(frames), headers_(static_cast<size_t>(buffers)), data_(static_cast<size_t>(buffers)) {}

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
    for (size_t i = 0; i < headers_.size(); ++i) {
      data_[i].assign(static_cast<size_t>(frames_) * 2, 0);
      headers_[i] = {};
      headers_[i].lpData = reinterpret_cast<LPSTR>(data_[i].data());
      headers_[i].dwBufferLength = static_cast<DWORD>(frames_ * 4);
      waveOutPrepareHeader(out_, &headers_[i], sizeof(WAVEHDR));
      headers_[i].dwFlags |= WHDR_DONE;  // free to be filled
    }
    running_ = true;
    thread_ = std::thread([this] { Run(); });
    Logf(LogLevel::Info, "moho64ui: sound: device open (the old wave output), %.0f ms under way at most",
         1000.0 * frames_ * static_cast<double>(headers_.size()) / Mixer::kRate);
    return true;
  }

  ~WaveOutDevice() override {
    running_ = false;
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (out_) {
      waveOutReset(out_);
      for (WAVEHDR& h : headers_) waveOutUnprepareHeader(out_, &h, sizeof(WAVEHDR));
      waveOutClose(out_);
    }
    if (event_) CloseHandle(event_);
  }

 private:
  void Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (running_) {
      for (size_t i = 0; i < headers_.size(); ++i) {
        if (!(headers_[i].dwFlags & WHDR_DONE)) continue;
        headers_[i].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        mixer_->Mix(data_[i].data(), frames_);
        waveOutWrite(out_, &headers_[i], sizeof(WAVEHDR));
      }
      WaitForSingleObject(event_, 50);
    }
  }

  Mixer* mixer_;
  int frames_;
  HWAVEOUT out_ = nullptr;
  HANDLE event_ = nullptr;
  std::vector<WAVEHDR> headers_;
  std::vector<std::vector<int16_t>> data_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// A check without ears: what the system's sound engine sends to the default output while we
// run (everything that plays on the machine, ours included), written to a file as it is: the
// first two channels, 32-bit floats. Its first line of text names the rate. Switched on by the
// environment variable MOHO64UI_SOUND_CAPTURE=<file>; nothing of it runs otherwise.
class Listener : public OutputDevice {
 public:
  explicit Listener(std::string path) : path_(std::move(path)) { thread_ = std::thread([this] { Run(); }); }
  ~Listener() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    std::FILE* file = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                         reinterpret_cast<void**>(&enumerator))) &&
              SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) &&
              SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
              SUCCEEDED(client->GetMixFormat(&format)) &&
              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, format, nullptr)) &&
              SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) &&
              SUCCEEDED(client->Start());
    // The engine's own format is 32-bit floats; anything else is not written.
    bool floats = ok && format->wBitsPerSample == 32 && format->nChannels >= 1;
    if (floats) file = std::fopen(path_.c_str(), "wb");
    if (file) std::fprintf(file, "rate %lu channels 2 float32\n", static_cast<unsigned long>(format->nSamplesPerSec));
    Logf(file ? LogLevel::Info : LogLevel::Warning, "moho64ui: sound: listening to the output into %s: %s", path_.c_str(), file ? "on" : "not possible");
    std::vector<float> pair;
    while (file && running_) {
      Sleep(5);
      for (;;) {
        UINT32 next = 0;
        if (FAILED(capture->GetNextPacketSize(&next)) || next == 0) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        pair.assign(static_cast<size_t>(frames) * 2, 0.0f);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
          const float* in = reinterpret_cast<const float*>(data);
          int ch = format->nChannels;
          for (UINT32 i = 0; i < frames; ++i) {
            pair[i * 2] = in[i * static_cast<UINT32>(ch)];
            pair[i * 2 + 1] = in[i * static_cast<UINT32>(ch) + (ch > 1 ? 1 : 0)];
          }
        }
        std::fwrite(pair.data(), sizeof(float), pair.size(), file);
        capture->ReleaseBuffer(frames);
      }
    }
    if (file) std::fclose(file);
    if (client && ok) client->Stop();
    if (format) CoTaskMemFree(format);
    Release(capture), Release(client), Release(endpoint), Release(enumerator);
    if (com) CoUninitialize();
  }

  std::string path_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

// The device and, when asked for, the listener beside it.
class Devices : public OutputDevice {
 public:
  std::unique_ptr<OutputDevice> listener, player;
  ~Devices() override {
    player.reset();
    listener.reset();
  }
};

}  // namespace

std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer* mixer) {
  auto devices = std::make_unique<Devices>();
  if (const char* path = std::getenv("MOHO64UI_SOUND_CAPTURE"))
    if (*path) devices->listener = std::make_unique<Listener>(path);
  // MOHO64UI_SOUND=waveout: the old wave output (=waveout-small: with the small buffers that
  // crackle, to compare).
  const char* choice = std::getenv("MOHO64UI_SOUND");
  std::string want = choice ? choice : "";
  if (want.rfind("waveout", 0) != 0) {
    auto session = std::make_unique<SessionDevice>(mixer);
    if (session->Open()) {
      devices->player = std::move(session);
      return devices;
    }
    Logf(LogLevel::Warning, "moho64ui: sound: the sound session cannot be opened; trying the old wave output");
  }
  bool small = want == "waveout-small";
  auto wave = std::make_unique<WaveOutDevice>(mixer, small ? Mixer::kRate / 100 : Mixer::kRate / 20, small ? 3 : 4);
  if (!wave->Open()) return nullptr;
  devices->player = std::move(wave);
  return devices;
}

}  // namespace moho::ui::sound

#else

namespace moho::ui::sound {
std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer*) { return nullptr; }
}  // namespace moho::ui::sound

#endif
