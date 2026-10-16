-- Ducking, as the original's sound manager: a voice played with PlayVoice(params, true) takes the
-- other sounds down while it plays. The game ramps its sound settings' variable "Duck" from 0 to 1
-- over "DuckLength" (0.5 s) and back when the last ducking voice ends; the cut is the settings'
-- curve by "Duck" (10 dB at most), which the menu's ambient loop has, and the voices do not.
-- Without a sound device: moho64ui_SoundMix moves the sounds on.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/duck.lua --frames 600 --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function duck() return moho64ui_SoundInfo().duck end
local function level(h) return (moho64ui_SoundPlace(h)) or -1 end
local function near(a, b, d) return math.abs(a - b) <= d end
local voiceCue = Sound({Bank = "XGG", Cue = "XGG_Computer_CV01_04765"})
ForkThread(function()
    StopAllSounds()
    WaitSeconds(0.1)
    local loop = PlaySound(Sound({Bank = "AmbientTest", Cue = "AMB_Menu_Loop"}))
    check("the menu's ambient loop plays", loop ~= nil, true)
    check("nothing ducked: the loop at its full level", near(level(loop), 1, 0.01), true)
    -- a ducking voice: from the next frame the duck goes down, in a straight line over half a second
    local voice = PlayVoice(voiceCue, true)
    check("a ducking voice plays", voice ~= nil, true)
    check("the duck starts from the next frame", duck(), 0)
    WaitSeconds(0.25)
    LOG("TEST note: a quarter second in, the duck at " .. string.format("%.3f", duck()))
    check("a quarter second in it is about half way", duck() > 0.3 and duck() < 0.7, true)
    WaitSeconds(0.5)
    check("after half a second it is full", duck(), 1)
    LOG("TEST note: the loop's level ducked: " .. string.format("%.3f", level(loop)))
    check("the loop 10 dB down (0.316)", near(level(loop), 0.316, 0.01), true)
    local vl = moho64ui_SoundPlace(voice)
    check("the voice itself is not ducked", vl ~= nil and near(vl, 1, 0.001), true)
    -- a second ducking voice holds it when the first ends
    local second = PlayVoice(voiceCue, true)
    StopSound(voice, true)
    WaitSeconds(0.6)
    check("with one of two stopped the duck holds", duck(), 1)
    -- a voice that does not duck changes nothing
    local plain = PlayVoice(voiceCue, false)
    StopSound(plain, true)
    StopSound(second)  -- (softly: it lets go once its sound has ended)
    moho64ui_SoundMix(300)
    WaitSeconds(0.1)
    check("the last ducking voice ended: the duck comes back", duck() < 1, true)
    WaitSeconds(0.6)
    check("and is gone half a second later", duck(), 0)
    check("the loop back at its full level", near(level(loop), 1, 0.01), true)
    -- setting a volume ends the duck at once (the original's SetVolume)
    local third = PlayVoice(voiceCue, true)
    WaitSeconds(0.6)
    check("ducked again", duck(), 1)
    SetVolume("Interface", GetVolume("Interface"))
    check("SetVolume ends the duck at once", duck(), 0)
    check("and the loop is back at once", near(level(loop), 1, 0.01), true)
    StopSound(third, true)
    WaitSeconds(0.6)
    check("the voice that had ducked lets go of nothing", duck(), 0)
    -- a prepared ducking voice ducks already (the original pushes at PlayVoice)
    local prepared = PlayVoice(voiceCue, true, true)
    WaitSeconds(0.6)
    check("a prepared ducking voice ducks from the call", duck(), 1)
    StopAllSounds()
    check("StopAllSounds ends the duck at once", duck(), 0)
    -- PlayTutorialVO(params, prepareOnly): never ducks; its second argument is prepareOnly
    local tutorial = PlayTutorialVO(voiceCue, true)
    WaitSeconds(0.6)
    check("PlayTutorialVO does not duck", duck(), 0)
    check("its second argument prepares it only", string.find(moho64ui_SoundVoices(), "(waiting)", 1, true) ~= nil, true)
    StopAllSounds()
    LOG("TEST done: " .. failed .. " failed")
end)
