-- The sound device when its output goes away (the review of 2026-11-09, toolkit H2), checked without ears and
-- without a headset to pull. A run without a window has no device, so the test opens one for a moment through the
-- host's moho64ui_SoundDevice (it waits in REAL time, about 6 s in all):
-- 1. "silent": no output at all, the mixer run by the clock (a live window with --no-sound or no device to be had):
--    a sound started before ends by itself.
-- 2. "output": the system's default output, as a live window opens it, playing NOTHING (no sound is started; the
--    part is left out when anything plays). After 0.3 s it loses its output as if the device were pulled and finds
--    none for 0.4 s (sound/device.h SetDeviceTestLoss): it must run the mixer by the clock meanwhile and open the
--    default output again. Then once more, lost after 1.2 s with an output there at once: opened again without a
--    wait. On a machine with no output, or one the session cannot open, the part says so and passes.
-- What it cannot show: a real device's own failure (the error a pulled headset gives), and the move to another
-- device when Windows' default changes.
--   moho64ui --init <init.lua> --stage setup --lua ui/tests/sound_device.lua --frames 8 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function between(x, low, high) return x ~= nil and x >= low and x <= high end

-- 1. the clock with no output
check("nothing plays at first", moho64ui_SoundVoices(), "")
PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown_Sml"}))  -- (0.7 s long)
check("a sound is started", moho64ui_SoundVoices() ~= "", true)
local state, frames = moho64ui_SoundDevice("silent", 1200)
LOG("TEST note: with no output: " .. tostring(state) .. "; " .. tostring(frames) .. " frames mixed in 1200 ms")
check("with no output the mixer runs by the clock: about 1.2 s of frames in 1.2 s", between(frames, 48000, 67200), true)
check("and the sound has ended by itself", moho64ui_SoundVoices(), "")
check("nothing is left to mix", moho64ui_SoundMix(100), 0)
check("a device cannot be asked for a kind it has not", moho64ui_SoundDevice("loud", 10), nil)

-- 2. the default output lost and found again
if moho64ui_SoundVoices() ~= "" then
    LOG("TEST note: something plays: the default output is not opened")
else
    state, frames = moho64ui_SoundDevice("output", 2400, 300, 400)
    LOG("TEST note: the default output: " .. tostring(state) .. "; " .. tostring(frames) .. " frames mixed in 2400 ms")
    if not state then
        LOG("TEST note: no sound output can be opened on this machine: the loss is not checked")
    elseif state == "" then
        LOG("TEST note: the output is not the session's (the old wave output): the loss is not checked")
    elseif string.find(state, "lost 0", 1, true) and string.find(state, "waiting for an output", 1, true) then
        LOG("TEST note: this machine has no sound output now: the session waits for one")
        check("waiting for an output the mixer runs by the clock", between(frames, 96000, 134400), true)
    else
        check("the output was lost once and the default output opened again", string.find(state, "session open, lost 1, opened again 1", 1, true) ~= nil, true)
        local _, _, clock = string.find(state, "(%d+) ms on the clock")
        clock = tonumber(clock)
        -- (no output for 0.4 s, tried again every second: about a second on the clock)
        check("the mixer ran by the clock while there was no output", between(clock, 300, 1500), true)
        check("and all through: about 2.4 s of frames in 2.4 s", between(frames, 96000, 134400), true)
        -- lost after it had run for more than a second, an output there: opened again at once
        state, frames = moho64ui_SoundDevice("output", 2000, 1200, 0)
        LOG("TEST note: the default output, lost after 1.2 s: " .. tostring(state) .. "; " .. tostring(frames) .. " frames mixed in 2000 ms")
        check("lost with an output there: opened again", string.find(tostring(state), "session open, lost 1, opened again 1", 1, true) ~= nil, true)
        _, _, clock = string.find(tostring(state), "(%d+) ms on the clock")
        check("at once: next to nothing on the clock", between(tonumber(clock), 0, 200), true)
        check("about 2 s of frames in 2 s", between(frames, 80000, 115200), true)
    end
end
LOG("TEST done: " .. failed .. " failed")
