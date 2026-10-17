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

-- the files' fades by time (the sound library's curves by "AttackTime" and "ReleaseTime"). The
-- music stopped softly goes out over its tracks' release curve (Music.xsb: 0 dB, -2.4 at 1.6 s,
-- -7.2 at 3 s, -20 at 4.7 s, -96 at 6.06 s), which the game's music script waits for before the
-- next piece (UserMusic.lua: StopSound, then WaitFor).
local function curves()  -- what the curves of the one voice playing give now
    return tonumber(string.match(moho64ui_SoundVoices(), "curves ([%d%.]+)")) or -1
end
local function between(x, a, b) return x > a and x < b end
local piece = PlaySound(Sound({Bank = "Music", Cue = "Main_Menu"}))
moho64ui_SoundMix(500)
check("the music has no attack: its curves give 1", curves(), 1)
StopSound(piece)
moho64ui_SoundMix(1000)
check("stopped softly: a second into its release, 1.5 dB down", between(curves(), 0.80, 0.88), true)
check("the release lasts as long as its curve", string.find(moho64ui_SoundVoices(), "of 6061 ms") ~= nil, true)
moho64ui_SoundMix(3700)
check("4.7 s in: 20 dB down", between(curves(), 0.08, 0.12), true)
check("and still playing (WaitFor waits for it)", moho64ui_SoundPlace(piece) ~= nil, true)
moho64ui_SoundMix(1500)
check("ended at its curve's end", moho64ui_SoundPlace(piece), nil)
check("silent", moho64ui_SoundMix(100), 0)
-- the menu's ambient loop comes in over half a second and goes over one (AttackTime and
-- ReleaseTime curves of its track, as the units' running sounds have)
local loop = PlaySound(Sound({Bank = "AmbientTest", Cue = "AMB_Menu_Loop"}))
moho64ui_SoundMix(100)
check("a loop's attack: 0.1 s in, 14 dB down", between(curves(), 0.15, 0.25), true)
moho64ui_SoundMix(500)
check("full after half a second", between(curves(), 0.99, 1.01), true)
StopSound(loop)
moho64ui_SoundMix(500)
check("stopped softly: half a second into its release, nearly gone", between(curves(), 0, 0.01), true)
check("still playing", moho64ui_SoundPlace(loop) ~= nil, true)
moho64ui_SoundMix(600)
check("ended after its release's second", moho64ui_SoundPlace(loop), nil)
local again = PlaySound(Sound({Bank = "AmbientTest", Cue = "AMB_Menu_Loop"}))
moho64ui_SoundMix(700)
StopSound(again)
moho64ui_SoundMix(100)
StopSound(again, true)
moho64ui_SoundMix(20)
check("stopped at once while it goes out: ended at once", moho64ui_SoundPlace(again), nil)

-- GetCueBank (in every state of the original): a sound's cue and bank
local cue, bank = GetCueBank(Sound({Bank = "Interface", Cue = "UI_Menu_MouseDown"}))
check("GetCueBank gives the cue and the bank", tostring(cue) .. " " .. tostring(bank), "UI_Menu_MouseDown Interface")

-- a voice: its bank is among the voices of the language
local voice = PlayVoice(Sound({Bank = "XGG", Cue = "XGG_Computer_CV01_04765"}))
check("PlayVoice finds a voice bank", voice ~= nil and string.sub(moho64ui_SoundInfo().wave, 1, 4), "XGG#")
StopAllSounds()
moho64ui_SoundMix(50)
check("StopAllSounds: silent", moho64ui_SoundMix(100), 0)

-- the world's sounds: heard from where the listener is, by the sound's own curves in the game's files
moho64ui_SoundMix(500)
local step = {"UEL", "UEL0001_Move_Step"}
check("before there is a listener nothing of the world is heard", moho64ui_SoundAt(step[1], step[2], 100, 0, 100), false)
moho64ui_SoundListener(100, 40, 100, 100, 0, 100, 60)
check("near the listener a unit's step is heard", moho64ui_SoundAt(step[1], step[2], 100, 0, 100), true)
local near = moho64ui_SoundMix(300)
moho64ui_SoundMix(3000)
moho64ui_SoundListener(100, 600, 100, 100, 0, 100, 700)
check("from farther off it still plays", moho64ui_SoundAt(step[1], step[2], 100, 0, 100), true)
local mid = moho64ui_SoundMix(300)
check("and is much softer", mid > 0 and mid < near * 0.5, true)
moho64ui_SoundMix(3000)
moho64ui_SoundListener(100, 6000, 100, 100, 0, 100, 6000)
check("from very far it is not played at all", moho64ui_SoundAt(step[1], step[2], 100, 0, 100), false)
check("two sounds of the world were heard", moho64ui_SoundInfo().world, 2)
-- a sound to one side is louder in that ear: mix a step far to the east and compare the ears through the file
moho64ui_SoundListener(100, 40, 100, 100, 0, 100, 60)
check("a playing sound follows the listener: moving away silences it", (function()
    moho64ui_SoundAt(step[1], step[2], 100, 0, 100)
    local a = moho64ui_SoundMix(100)
    moho64ui_SoundListener(100, 6000, 100, 100, 0, 100, 6000)
    local b = moho64ui_SoundMix(100)
    return a > 0 and b == 0
end)(), true)
StopAllSounds()
moho64ui_SoundMix(100)

LOG("TEST done: " .. failed .. " failed")
