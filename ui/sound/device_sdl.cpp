// Sound output through SDL3 (Linux, Mac; on Windows in test builds, MOHO64UI_SDL): the system's
// default output, 48000 frames a second, stereo, 16 bits, fed from the mixer whenever SDL asks for
// more (on SDL's own sound thread, as the Windows device's thread feeds WASAPI).
#include "sound/device.h"

#ifdef MOHO64UI_SDL
#include <SDL3/SDL.h>

#include <vector>

#include "core/log.h"
#include "sound/mixer.h"

namespace moho::ui::sound {
namespace {

class SdlDevice : public OutputDevice {
 public:
  explicit SdlDevice(Mixer* mixer) : mixer_(mixer) {}
  ~SdlDevice() override {
    if (stream_) SDL_DestroyAudioStream(stream_);  // (stops the feeding first)
  }
  bool Open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      Logf(LogLevel::Warning, "moho64ui: sound: no sound output (SDL: %s)", SDL_GetError());
      return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, Mixer::kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Feed, this);
    if (!stream_) {
      Logf(LogLevel::Warning, "moho64ui: sound: the sound output did not open (SDL: %s)", SDL_GetError());
      return false;
    }
    return SDL_ResumeAudioStreamDevice(stream_);
  }

 private:
  // SDL asks for `wanted` more bytes.
  static void SDLCALL Feed(void* self, SDL_AudioStream* stream, int wanted, int) {
    auto* d = static_cast<SdlDevice*>(self);
    const int frames = wanted / 4;
    if (frames <= 0) return;
    d->buffer_.resize(static_cast<size_t>(frames) * 2);
    d->mixer_->Mix(d->buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, d->buffer_.data(), frames * 4);
  }
  Mixer* mixer_;
  SDL_AudioStream* stream_ = nullptr;
  std::vector<int16_t> buffer_;
};

}  // namespace

std::unique_ptr<OutputDevice> OpenOutputDevice(Mixer* mixer) {
  auto device = std::make_unique<SdlDevice>(mixer);
  if (!device->Open()) return nullptr;
  Logf(LogLevel::Info, "moho64ui: sound: SDL output, %d frames a second", Mixer::kRate);
  return device;
}

}  // namespace moho::ui::sound

#endif  // MOHO64UI_SDL
