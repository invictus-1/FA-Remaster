#include "sound/device.h"

#include "core/log.h"
#include "sound/mixer.h"

#if defined(_WIN32) && !defined(MOHO64UI_SDL)
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <atomic>
#include <chrono>
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

// The system's word that the default output is another device now (a headset plugged in or pulled, another output
// chosen in Windows' sound settings): it only raises a flag and wakes the device's thread, which does the rest (the
// system's rule for these calls: no waiting, nothing released in them). Owned by that thread as a plain object: the
// reference counts only count.
class DefaultWatcher final : public IMMNotificationClient {
 public:
  DefaultWatcher(std::atomic<bool>* changed, HANDLE wake) : changed_(changed), wake_(wake) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
  ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--refs_); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IMMNotificationClient))) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) {
      changed_->store(true);
      SetEvent(wake_);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  std::atomic<bool>* changed_;
  HANDLE wake_;
  std::atomic<long> refs_{1};
};

// Windows' sound session of today (WASAPI, shared mode): the system's sound engine asks for sound
// in its own rhythm (every hundredth of a second) and we keep its buffer of about two such
// pieces full. That is as short a way from a click to its sound as the shared engine allows, and
// it does not crackle: the old wave output with buffers this small does (its pump found less
// than it needed at times and put silence in: heard as static over everything).
//
// The session belongs to ONE output device, the default one when it was opened. It follows the default (ours; the
// review of 2026-11-09, toolkit H2: a headset pulled was silence until the program was started again):
// - a call of the session that fails means its device is gone or was set up anew (AUDCLNT_E_DEVICE_INVALIDATED and
//   its kin): the session is closed and the default output opened again. A device that is gone also stops asking for
//   sound, so the session is tried at every wait that runs out (a fifth of a second), not only when it asks;
// - the system says when another device becomes the default (DefaultWatcher): the session moves to it;
// - while no output can be opened (none there, or the new one not ready yet) it is tried again every second and
//   at once when the default changes, and the mixer runs on the clock meanwhile (SilentClock): sounds end as they
//   would aloud.
// Not tried here: a real device pulled and a real change of the default (ui/tests/sound_device.lua makes the loss up,
// SetDeviceTestLoss, and so runs the closing, the clock and the opening again against the machine's real output).
// At the first opening an output that is there but does not open this way is still left to the old wave output.
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

  std::string State() const override {
    char text[160];
    std::snprintf(text, sizeof text, "session %s, lost %d, opened again %d, %lld ms on the clock", open_ ? "open" : "waiting for an output",
                  lost_.load(), reopened_.load(), silentFrames_.load() * 1000 / Mixer::kRate);
    return text;
  }

 private:
  using Clock = std::chrono::steady_clock;
  enum class Fed { Full, Dry, Lost };  // Dry: filled, but it was empty (it had run dry)

  // The default output opened and playing. False: not opened (nothing is left open); `none` then says whether the
  // system has no output at all right now.
  bool OpenEndpoint(bool* none) {
    *none = FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint_)) || Clock::now() < testAwayUntil_;
    if (*none || !StartSession()) {
      CloseEndpoint();
      return false;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(endpoint_->GetId(&id)) && id) id_ = id;
    if (id) CoTaskMemFree(id);
    openedAt_ = Clock::now();
    open_ = true;
    return true;
  }

  bool StartSession() {
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
    HRESULT why = S_OK;
    if (Fill(&why) == Fed::Lost) return false;
    if (FAILED(client_->Start())) return false;
    Logf(LogLevel::Info, "moho64ui: sound: device open, %.1f ms under way at most", 1000.0 * frames_ / Mixer::kRate);
    return true;
  }

  void CloseEndpoint() {
    if (client_) client_->Stop();
    Release(render_), Release(client_), Release(endpoint_);
    frames_ = 0;
    id_.clear();
    open_ = false;
  }

  // The id of the system's default output now ("" when there is none).
  std::wstring DefaultId() {
    std::wstring text;
    IMMDevice* now = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &now)) || !now) return text;
    LPWSTR id = nullptr;
    if (SUCCEEDED(now->GetId(&id)) && id) text = id;
    if (id) CoTaskMemFree(id);
    now->Release();
    return text;
  }

  // Fill what is free of the system's buffer. Lost: a call failed (`why`), the session is of no use any more.
  Fed Fill(HRESULT* why) {
    UINT32 queued = 0;
    if (FAILED(*why = client_->GetCurrentPadding(&queued))) return Fed::Lost;
    if (queued >= frames_) return Fed::Full;
    UINT32 free = frames_ - queued;
    BYTE* data = nullptr;
    if (FAILED(*why = render_->GetBuffer(free, &data))) return Fed::Lost;
    mixer_->Mix(reinterpret_cast<int16_t*>(data), static_cast<int>(free));
    if (FAILED(*why = render_->ReleaseBuffer(free, 0))) return Fed::Lost;
    return queued > 0 ? Fed::Full : Fed::Dry;
  }

  void Run() {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int testAfterMs = -1, testAwayMs = 0;  // (SetDeviceTestLoss: once for this device)
    DeviceTestLoss(&testAfterMs, &testAwayMs);
    DefaultWatcher watcher(&changed_, event_);
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                         reinterpret_cast<void**>(&enumerator_)));
    bool none = false;
    if (ok && !OpenEndpoint(&none)) {
      ok = none;  // (there, but not to be opened this way: the old wave output's)
      if (ok) Logf(LogLevel::Warning, "moho64ui: sound: the system has no sound output now: silent until there is one");
    }
    const bool watching = ok && SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(&watcher));
    Clock::time_point retry = Clock::now() + std::chrono::seconds(1);
    clock_.Reset();
    state_ = ok ? 1 : 2;
    while (ok && running_) {
      if (client_) {
        const DWORD woke = WaitForSingleObject(event_, 200);
        if (!running_) break;
        HRESULT why = S_OK;
        Fed fed = Fed::Full;
        bool moved = false;
        if (changed_.exchange(false)) {
          const std::wstring now = DefaultId();
          moved = !now.empty() && now != id_;
        }
        if (moved) {
          Logf(LogLevel::Info, "moho64ui: sound: another device is the default output now: moving to it");
          fed = Fed::Lost;
        } else if (testAfterMs >= 0 && Clock::now() - openedAt_ >= std::chrono::milliseconds(testAfterMs)) {
          testAwayUntil_ = Clock::now() + std::chrono::milliseconds(testAwayMs);
          testAfterMs = -1;
          Logf(LogLevel::Warning, "moho64ui: sound: the output is taken away for the check (SetDeviceTestLoss)");
          fed = Fed::Lost;
        } else {
          // (also when the wait ran out: a device that is gone asks for nothing any more)
          fed = Fill(&why);
          if (fed == Fed::Lost)
            Logf(LogLevel::Warning, "moho64ui: sound: the output is gone (0x%08lx): opening the default output again",
                 static_cast<unsigned long>(why));
        }
        if (fed == Fed::Dry && woke == WAIT_OBJECT_0) ++starved_;
        if (fed != Fed::Lost) continue;
        ++lost_;
        // The default output is tried at once: the system may have one again already. Not when the session failed
        // within a second of its opening: an output that fails as soon as it is open is not opened over and over.
        const bool atOnce = moved || Clock::now() - openedAt_ >= std::chrono::seconds(1);
        CloseEndpoint();
        clock_.Reset();
        retry = atOnce ? Clock::now() : Clock::now() + std::chrono::seconds(1);
      }
      // No output: tried again every second and when the default changes; the mixer on the clock meanwhile.
      if (changed_.exchange(false) || Clock::now() >= retry) {
        if (OpenEndpoint(&none)) {
          ++reopened_;
          Logf(LogLevel::Info, "moho64ui: sound: the default output is open again");
          continue;
        }
        retry = Clock::now() + std::chrono::seconds(1);
      }
      silentFrames_ += clock_.Step(mixer_);
      WaitForSingleObject(event_, 10);
    }
    if (watching) enumerator_->UnregisterEndpointNotificationCallback(&watcher);
    CloseEndpoint();
    Release(enumerator_);
    if (com) CoUninitialize();
  }

  Mixer* mixer_;
  HANDLE event_ = nullptr;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  IMMDevice* endpoint_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  UINT32 frames_ = 0;
  std::wstring id_;                    // the open output's id (DefaultId)
  Clock::time_point openedAt_{};       // when it was opened
  Clock::time_point testAwayUntil_{};  // (SetDeviceTestLoss: no output until then)
  SilentClock clock_;
  std::atomic<bool> changed_{false};  // the default output changed (DefaultWatcher)
  std::atomic<int> state_{0};         // 0 starting, 1 playing or waiting for an output, 2 no session to be had
  std::atomic<bool> running_{true};
  std::atomic<bool> open_{false};
  std::atomic<int> starved_{0}, lost_{0}, reopened_{0};
  std::atomic<long long> silentFrames_{0};
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
  std::string State() const override { return player ? player->State() : std::string(); }
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

#elif !defined(MOHO64UI_SDL)  // (SDL's: device_sdl.cpp)

namespace moho::ui::sound {
std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer*) { return nullptr; }
}  // namespace moho::ui::sound

#endif
