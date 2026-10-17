# Answers from the interface track

The other tracks write what they need from the interface into their own request files (`render/REQUESTS.md`,
the engine by message, the visuals track's notes). The interface track answers here. Newest first.

## render 33: Ground Detail — done (2026-10-17)

A fifth option in the Video tab, after Weather Effects: "Ground Detail" (`remaster_ground_detail`), off by
default, `SetGroundDetail` at the game picture's first frame and on every change, as the four of request 31.
Checked: `ui/tests/lobby.lua` (off unless chosen), and a live game in a window switching it on and off (no
errors). Weather Effects' comment in the options file now names wet shorelines and the shallows' caustics.

## render 31: the lighting upgrade's four settings — done (2026-10-17)

Four options in the Options menu's Video tab, after "Temporal Upscaling (FSR 3)", each off by default, as
asked:
- "Atmosphere" (`remaster_atmosphere`): `SetAtmosphere`.
- "Soft Shadows" (`remaster_soft_shadows`): `SetSoftShadows`.
- "Ray Tracing" (`remaster_ray_tracing`): `SetRayTracing`. Where `RayTracingSupported()` is false, the option
  offers one state, "Not on this graphics card", which the game's own options dialog greys out. The answer is
  taken after every `Open` (the window's menu picture and each game's picture) and read when the dialog
  opens. Before any picture has answered (a test run without a window), both states are offered.
- "Weather Effects" (`remaster_weather_effects`): `SetWeatherEffects`.

Each is a console command the option sends (`ui/user/keymap.cpp`), kept in `LookSettings`
(`ui/user/user_state.h`). The game's picture gets the call at its first frame and whenever the setting changes
(`ui/app/game_session.cpp`, the looks block, beside `SetTemporalUpscale`). The menu's picture and the
menu's moving background are left alone. Checked: `ui/tests/lobby.lua` (the four options, off unless chosen, and
switching them), and a live game in a window with all four switched on and off again (no errors; this card says
it can ray trace).
