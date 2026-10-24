-- The wave banks, mapped into memory when the sound starts (as the original's sound engine holds
-- them): no bank is read when a cue of it first plays. A cue of a bank never used before plays at
-- once; with its wave's bytes brought in on the sound system's own thread (a live window's way),
-- nothing of the wave is touched on the frame, the cue is not prepared until its first part is in,
-- and then it sounds. Without a sound device: moho64ui_SoundMix moves the sounds on.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/sound_banks.lua --frames 300 --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
ForkThread(function()
    WaitSeconds(0.1)
    StopAllSounds()
    local start = moho64ui_SoundBanks()
    check("the game's wave banks are mapped from the start (more than 50)", start.mapped > 50, true)
    check("none of them read whole", start.readWhole, 0)
    check("the music is streamed", start.streamed >= 1, true)
    LOG(string.format("TEST note: %d wave banks mapped, %.0f MB of files, %d streamed", start.mapped, start.mappedMB, start.streamed))

    -- the waves' bytes brought in on the thread: a cue of a bank never used plays at once, nothing of it touched here
    moho64ui_SoundBanks(true)
    local here = moho64ui_SoundBanks().touchedHereMB
    local h = PlaySound(Sound({Bank = "UEB", Cue = "UEB2108_Activate"}))
    check("PlaySound gives a handle at once", h ~= nil, true)
    check("its wave is the bank's", string.sub(moho64ui_SoundInfo().wave, 1, 4), "UEB#")
    check("nothing of the wave touched on the frame", moho64ui_SoundBanks().touchedHereMB, here)
    LOG(string.format("TEST note: the first play of a bank took %.2f ms here", moho64ui_SoundBanks().longestPlayMs))
    moho64ui_SoundBanksRead()
    check("once its first part is in, it is prepared", SoundIsPrepared(h), true)
    check("the thread touched it", moho64ui_SoundBanks().touchedMB > 0, true)
    check("and it sounds", moho64ui_SoundMix(200) > 0.001, true)
    StopAllSounds()
    moho64ui_SoundMix(50)

    -- prepared only (as a movie's sound): it waits for StartSound
    local p = PlaySound(Sound({Bank = "UAB", Cue = "UAB0201_Activate"}), true)
    moho64ui_SoundBanksRead()
    check("a prepared cue of another bank: prepared", SoundIsPrepared(p), true)
    check("silent until started", moho64ui_SoundMix(100), 0)
    StartSound(p)
    check("started: it sounds", moho64ui_SoundMix(200) > 0.001, true)
    StopAllSounds()
    moho64ui_SoundMix(50)

    -- the music still streams from its file
    local music = PlaySound(Sound({Bank = "Music", Cue = "Main_Menu"}))
    check("the music plays", music ~= nil and SoundIsPrepared(music), true)
    check("and sounds", moho64ui_SoundMix(2000) > 0.01, true)
    StopAllSounds()
    moho64ui_SoundMix(50)

    -- touching on the thread off (the runs without a window): the wave is touched there and then
    moho64ui_SoundBanks(false)
    local before = moho64ui_SoundBanks().touchedHereMB
    local now = PlaySound(Sound({Bank = "XSB", Cue = "XSB1104_Activate"}))
    check("off: prepared at once", SoundIsPrepared(now), true)
    check("off: its wave touched here", moho64ui_SoundBanks().touchedHereMB > before, true)
    check("and it sounds", moho64ui_SoundMix(200) > 0.001, true)
    StopAllSounds()
    LOG("TEST done: " .. failed .. " failed")
end)
