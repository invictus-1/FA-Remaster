// The user state's sound functions: PlaySound and its handles, voices, the categories' volumes.
// Behind them the host's sound system (sound/sound_system.h); without one (a host that gives
// none) they answer as if nothing could be played.
//
// From the original (FA exe): PlaySound(sndParams, prepareOnly) 0x8ad180 takes the parameters
// object Sound{} made and gives a handle or nil (CUserSoundManager::ScriptPlaySound 0x8ab2b0).
// How its sound manager limits, fades and ducks is not read; what is here follows the sound
// files' own settings.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "script/lua.hpp"
#include "sim/blueprints.h"
#include "sound/sound_system.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

const char* const kHandle = "moho64ui.sound";

sound::SoundSystem* System(lua_State* L) { return UserState::From(L)->config().sound; }

// A sound parameters object. In scripts of the interface's own it is the table Sound{} was given
// (Bank, Cue; any case of the names). In a blueprint it is the engine's sound object: the game's
// rules are copied into a session's state with their sounds made into objects (a unit's
// Audio.UISelection, played when it is selected); the engine tells its two names.
bool ReadParams(lua_State* L, int idx, std::string* bank, std::string* cue) {
  if (lua_type(L, idx) == LUA_TUSERDATA) {
    lua_pushcfunction(L, l_GetCueBank);
    lua_pushvalue(L, idx);
    lua_call(L, 1, 2);
    if (lua_isstring(L, -2)) *cue = lua_tostring(L, -2);
    if (lua_isstring(L, -1)) *bank = lua_tostring(L, -1);
    lua_pop(L, 2);
    return !bank->empty() && !cue->empty();
  }
  if (!lua_istable(L, idx)) return false;
  for (const char* name : {"Bank", "bank"}) {
    lua_pushstring(L, name);
    lua_rawget(L, idx);
    if (lua_isstring(L, -1)) *bank = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  for (const char* name : {"Cue", "cue"}) {
    lua_pushstring(L, name);
    lua_rawget(L, idx);
    if (lua_isstring(L, -1)) *cue = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  return !bank->empty() && !cue->empty();
}

void PushHandle(lua_State* L, int handle) {
  int* box = static_cast<int*>(lua_newuserdata(L, sizeof(int)));
  *box = handle;
  luaL_getmetatable(L, kHandle);
  lua_setmetatable(L, -2);
}
int ToHandle(lua_State* L, int idx) {
  int* box = static_cast<int*>(luaL_checkudata(L, idx, kHandle));
  return box ? *box : 0;
}

int Play(lua_State* L, bool prepareOnly) {
  std::string bank, cue;
  sound::SoundSystem* s = System(L);
  int handle = s && ReadParams(L, 1, &bank, &cue) ? s->Play(bank, cue, prepareOnly) : 0;
  if (handle) PushHandle(L, handle);
  else lua_pushnil(L);
  return 1;
}

// handle = PlaySound(sndParams, prepareOnly)
int l_PlaySound(lua_State* L) { return Play(L, lua_toboolean(L, 2) != 0); }
// PlayVoice(params, duck), PlayTutorialVO(params): a voice is a cue like any other, its bank among
// the voices of the language. (Ducking the other sounds under it is not built.)
int l_PlayVoice(lua_State* L) { return Play(L, false); }

int l_SoundIsPrepared(lua_State* L) {
  ToHandle(L, 1);
  lua_pushboolean(L, 1);  // nothing is loaded in the background here
  return 1;
}
int l_StartSound(lua_State* L) {
  if (sound::SoundSystem* s = System(L)) s->Start(ToHandle(L, 1));
  return 0;
}
// StopSound(handle [, immediate = false])
int l_StopSound(lua_State* L) {
  if (lua_isnil(L, 1) || lua_isnone(L, 1)) return 0;
  if (sound::SoundSystem* s = System(L)) s->Stop(ToHandle(L, 1), lua_toboolean(L, 2) != 0);
  return 0;
}
int l_StopAllSounds(lua_State* L) {
  if (sound::SoundSystem* s = System(L)) s->StopAll();
  return 0;
}

// SetVolume(category, volume), volume = GetVolume(category)
int l_SetVolume(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  float volume = static_cast<float>(luaL_checknumber(L, 2));
  sound::SoundSystem* s = System(L);
  if (s && !s->SetVolume(category, volume)) Logf(LogLevel::Warning, "SetVolume: no sound category '%s'", category);
  return 0;
}
int l_GetVolume(lua_State* L) {
  float volume = 1;
  if (sound::SoundSystem* s = System(L)) s->GetVolume(luaL_checkstring(L, 1), &volume);
  lua_pushnumber(L, volume);
  return 1;
}
// PauseSound(category, pause), PauseVoice(category, pause)
int l_PauseSound(lua_State* L) {
  if (sound::SoundSystem* s = System(L)) s->Pause(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
  return 0;
}
int l_AudioSetLanguage(lua_State* L) {
  if (sound::SoundSystem* s = System(L)) s->SetLanguage(luaL_checkstring(L, 1));
  return 0;
}
// DisableWorldSounds(), EnableWorldSounds(): the world's own sounds (units, weapons) are not
// played yet, so there is nothing to switch.
int l_Nothing(lua_State*) { return 0; }

// moho64ui_SoundInfo() -> {played = cues played so far, wave = the last one's wave ("Interface#11"),
// world = how many of them were sounds of the world}
int l_SoundInfo(lua_State* L) {
  sound::SoundSystem* s = System(L);
  lua_newtable(L);
  lua_pushstring(L, "played");
  lua_pushnumber(L, s ? s->played() : 0);
  lua_rawset(L, -3);
  lua_pushstring(L, "wave");
  lua_pushstring(L, s ? s->lastWave().c_str() : "");
  lua_rawset(L, -3);
  lua_pushstring(L, "world");
  lua_pushnumber(L, s ? s->worldPlayed() : 0);
  lua_rawset(L, -3);
  return 1;
}
// moho64ui_SoundListener(eyeX, eyeY, eyeZ, focusX, focusY, focusZ, zoom): where the world's sounds
// are heard from (a game tells this itself, from its camera); the screen's right is the map's east.
int l_SoundListener(lua_State* L) {
  float eye[3], focus[3], right[3] = {1, 0, 0};
  for (int i = 0; i < 3; ++i) eye[i] = static_cast<float>(luaL_checknumber(L, 1 + i)), focus[i] = static_cast<float>(luaL_checknumber(L, 4 + i));
  if (sound::SoundSystem* s = System(L)) s->SetListener(eye, focus, right, static_cast<float>(luaL_checknumber(L, 7)));
  return 0;
}
// moho64ui_SoundVoices() -> a text: one line for every voice that plays or waits
int l_SoundVoices(lua_State* L) {
  sound::SoundSystem* s = System(L);
  lua_pushstring(L, s ? s->mixer().Describe().c_str() : "");
  return 1;
}
// moho64ui_SoundAt(bank, cue, x, y, z) -> true when the sound is heard from where the listener is
int l_SoundAt(lua_State* L) {
  float position[3];
  for (int i = 0; i < 3; ++i) position[i] = static_cast<float>(luaL_checknumber(L, 3 + i));
  sound::SoundSystem* s = System(L);
  lua_pushboolean(L, s && s->PlayAt(luaL_checkstring(L, 1), luaL_checkstring(L, 2), position) != 0);
  return 1;
}
// moho64ui_SoundMix(milliseconds [, file]) -> the loudest sample (0..1) of that much sound, mixed
// here and now. For runs without a sound device: it moves the sounds on as a device would. With a
// file's name the sound is added to that file as it is (16 bits, stereo, 48000 a second), to be
// compared with the game's sound data.
int l_SoundMix(lua_State* L) {
  sound::SoundSystem* s = System(L);
  int frames = static_cast<int>(luaL_checknumber(L, 1) * sound::Mixer::kRate / 1000.0);
  int peak = 0;
  if (s && frames > 0) {
    std::vector<int16_t> out(static_cast<size_t>(frames) * 2);
    s->mixer().Mix(out.data(), frames);
    for (int16_t v : out) peak = std::max(peak, v < 0 ? -static_cast<int>(v) : static_cast<int>(v));
    if (lua_isstring(L, 2)) {  // and added to a file as it is: 16 bits, stereo, 48000 a second
      if (std::FILE* f = std::fopen(lua_tostring(L, 2), "ab")) {
        std::fwrite(out.data(), sizeof(int16_t), out.size(), f);
        std::fclose(f);
      }
    }
  }
  lua_pushnumber(L, peak / 32767.0);
  return 1;
}

}  // namespace

void RegisterSoundBindings(lua_State* L) {
  luaL_newmetatable(L, kHandle);
  lua_pop(L, 1);
  SetGlobal(L, "PlaySound", l_PlaySound);
  SetGlobal(L, "PlayVoice", l_PlayVoice);
  SetGlobal(L, "PlayTutorialVO", l_PlayVoice);
  SetGlobal(L, "SoundIsPrepared", l_SoundIsPrepared);
  SetGlobal(L, "StartSound", l_StartSound);
  SetGlobal(L, "StopSound", l_StopSound);
  SetGlobal(L, "StopAllSounds", l_StopAllSounds);
  SetGlobal(L, "SetVolume", l_SetVolume);
  SetGlobal(L, "GetVolume", l_GetVolume);
  SetGlobal(L, "PauseSound", l_PauseSound);
  SetGlobal(L, "PauseVoice", l_PauseSound);
  SetGlobal(L, "AudioSetLanguage", l_AudioSetLanguage);
  SetGlobal(L, "DisableWorldSounds", l_Nothing);
  SetGlobal(L, "EnableWorldSounds", l_Nothing);
  lua_register(L, "moho64ui_SoundInfo", l_SoundInfo);
  lua_register(L, "moho64ui_SoundListener", l_SoundListener);
  lua_register(L, "moho64ui_SoundAt", l_SoundAt);
  lua_register(L, "moho64ui_SoundVoices", l_SoundVoices);
  lua_register(L, "moho64ui_SoundMix", l_SoundMix);
}

}  // namespace moho::ui
