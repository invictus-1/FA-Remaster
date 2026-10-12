-- The sound functions, checked without a sound device: the cues of the game's own sound banks
-- are found, their waves are mixed (moho64ui_SoundMix moves the sound on and gives the loudest
-- sample), volumes and stops act on what is mixed.
--   moho64ui --init <init.lua> --stage setup --lua ui/tests/sound.lua --frames 8 --log out.log
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

-- a cue of the interface's bank
local before = moho64ui_SoundInfo().played
local click = PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown_Sml"}))
check("PlaySound gives a handle", type(click), "userdata")
check("one cue was played", moho64ui_SoundInfo().played, before + 1)
check("the cue's wave is the bank's", moho64ui_SoundInfo().wave, "Interface#11")
check("it is prepared", SoundIsPrepared(click), true)
check("it sounds", moho64ui_SoundMix(100) > 0.01, true)
moho64ui_SoundMix(1500)
check("and ends (the wave is 0.7 s long)", moho64ui_SoundMix(100), 0)

-- names are taken in any case; what is not there gives nil
check("a cue's name in another case", PlaySound(Sound({Bank = "interface", Cue = "ui_menu_mousedown_sml"})) ~= nil, true)
moho64ui_SoundMix(1500)
check("no such cue: nil", PlaySound(Sound({Bank = "Interface", Cue = "No_Such_Cue"})), nil)
check("no such bank: nil", PlaySound(Sound({Bank = "NoSuchBank", Cue = "No_Such_Cue"})), nil)

-- a sound that waits for StartSound
local waiting = PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_Rollover"}), true)
check("prepared only: silent", moho64ui_SoundMix(100), 0)
StartSound(waiting)
check("started: it sounds", moho64ui_SoundMix(100) > 0.001, true)
StopSound(waiting, true)
moho64ui_SoundMix(50)
check("stopped at once: silent after a moment", moho64ui_SoundMix(50), 0)

-- volumes by category; a category stands inside another
check("a category's volume starts at 1", GetVolume("Interface"), 1)
SetVolume("Interface", 0)
PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown_Sml"}))
check("the Interface category at 0: silent", moho64ui_SoundMix(200), 0)
SetVolume("Interface", 1)
check("back at 1: it is heard again", moho64ui_SoundMix(200) > 0.01, true)
moho64ui_SoundMix(1500)
SetVolume("Global", 0)
PlaySound(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown_Sml"}))
check("Global at 0 silences the Interface sounds inside it", moho64ui_SoundMix(200), 0)
SetVolume("Global", 1)
check("GetVolume gives what was set", GetVolume("Global"), 1)
moho64ui_SoundMix(1500)

-- the music: read from its file as it plays; one piece at a time
local music = PlaySound(Sound({Bank = "Music", Cue = "Main_Menu"}))
check("the music's cue plays", music ~= nil and moho64ui_SoundInfo().wave, "Music#4")
check("it sounds", moho64ui_SoundMix(2000) > 0.01, true)
PauseSound("Music", true)
check("its category paused: silent", moho64ui_SoundMix(200), 0)
PauseSound("Music", false)
check("and it goes on after", moho64ui_SoundMix(500) > 0.01, true)
local battle = PlaySound(Sound({Bank = "Music", Cue = "Battle"}))
check("a second piece of music plays", battle ~= nil, true)
moho64ui_SoundMix(500)  -- the first one fades out
StopSound(battle, true)
moho64ui_SoundMix(50)
check("the first had made room for it: silent once the second is stopped", moho64ui_SoundMix(100), 0)

-- a voice: its bank is among the voices of the language
local voice = PlayVoice(Sound({Bank = "XGG", Cue = "XGG_Computer_CV01_04765"}))
check("PlayVoice finds a voice bank", voice ~= nil and string.sub(moho64ui_SoundInfo().wave, 1, 4), "XGG#")
StopAllSounds()
moho64ui_SoundMix(50)
check("StopAllSounds: silent", moho64ui_SoundMix(100), 0)

LOG("TEST done: " .. failed .. " failed")
