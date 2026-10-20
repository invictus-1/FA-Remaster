local framework = import('/testing/framework.lua')
-- The first-start graphics check (ui/data/hook/lua/ui/game/gamemain.lua; the measurement: the host's
-- FrameCostMeter): six games one after another in one run, each started over (RestartSession) from the one before,
-- with the card's time made up by the tests' moho64ui_SetFrameCostForTests (a run without a window draws no frames on
-- the card). 1: a fast card, no question, the check noted in the profile. 2: a slow card, the question with the
-- number; "Keep" changes nothing. 3: the next game asks no more. 4: "Lower settings" with the card staying slow: Ray
-- Tracing off first (where the card can ray trace), then the Remastered Preset a level at a time, stopping at Low; the
-- note. 5: "Lower settings" with the card fast after the first step: one step only. 6: the question left up and the
-- program ended at once, for a --screenshot of it. Run it like restart.lua (--prefs on a COPY of a preferences file:
-- the script keeps the stage there; with the world's picture) and --frames 6000:
--   moho64ui_test --own ui/data --init <init.lua> --prefs <copy> --stage setup --size 1920x1080 --session session_ai.lua
--            --seed 7 --game-lua ui/tests/firststart.lua --frames 6000 --screenshot firststart.png --log out.log
-- The stage is kept in preferences; the test host retains failures across the games' separate states.
local test = framework.suite('firststart')
local check = function(what, got, want) return test:equal(what, got, want) end
local Prefs = import("/lua/user/prefs.lua")
local logic = import("/lua/options/optionslogic.lua")
local looks = import("/lua/options/options.lua")
local main = import("/lua/ui/game/gamemain.lua")
local key = 'remaster_first_start_check'
local stage = GetPreference("moho64ui_test_firststart", 1)

-- the remastered looks' options (the preset's keys and the preset itself) as one line: as they stand, or as a level
-- of the preset has them
local function Line(values)
    local keys = {}
    for k in looks.RemasterPresetValues(4) do table.insert(keys, k) end
    table.insert(keys, 'remaster_preset')
    table.sort(keys)
    local out = ""
    for _, k in keys do out = out .. k .. "=" .. tostring(values[k]) .. " " end
    return out
end
local function Remastered() return Line(logic.GetCurrent()) end
local function LevelLine(level)
    local values = looks.RemasterPresetValues(level)
    values.remaster_preset = level
    if moho64ui_RayTracingSupported() == false then values.remaster_ray_tracing = logic.GetCurrent().remaster_ray_tracing end
    return Line(values)
end
-- the profile's options: every look at Ultra (the player's start), and the check noted or not
local function SetUp(noted)
    local now = logic.GetCurrent()
    for k, v in looks.RemasterPresetValues(4) do now[k] = v end
    now.remaster_preset = 4
    logic.SetCurrent(now)
    local profileOptions = Prefs.GetFromCurrentProfile('options') or {}
    profileOptions[key] = noted and 1 or nil
    Prefs.SetToCurrentProfile('options', profileOptions)
    SavePreferences()
end
local function Noted()
    return (Prefs.GetFromCurrentProfile('options') or {})[key]
end
local function WaitFor(cond, seconds)
    local waited = 0
    while not cond() and waited < seconds do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    return cond()
end
local function NextGame(nextStage, noted)
    SetUp(noted)
    SetPreference("moho64ui_test_firststart", nextStage)
    SavePreferences()
    test:checkpoint()
    RestartSession()
end
local function Buttons(popup)
    local d = popup and popup.content
    if not d or not d._button1 or not d._button2 then return "none" end
    return d._button1.label:GetText() .. " / " .. d._button2.label:GetText()
end

test:fork(function()
    local rayTracing = moho64ui_RayTracingSupported() ~= false
    if stage == 1 then
        check("the tests may make up the card's time", moho64ui_SetFrameCostForTests(8), true)
        SetUp(false)
        local before = Remastered()
        check("a fast card: the check finds it fast", WaitFor(function() return main.RemasterFirstStart.state == 'fast' end, 30), true)
        check("its measure is the card's time", math.floor(main.RemasterFirstStart.ms * 100 + 0.5), 800)
        check("and the host's", moho64ui_FrameCost() and math.floor(moho64ui_FrameCost() * 100 + 0.5), 800)
        check("no question", main.RemasterFirstStart.dialog, nil)
        check("nothing changed", Remastered(), before)
        check("the check is noted in the profile's options", Noted(), 1)
        NextGame(2, false)
        return
    end
    moho64ui_SetFrameCostForTests(24)
    if stage == 2 then
        local before = Remastered()
        check("a slow card: the question", WaitFor(function() return main.RemasterFirstStart.state == 'asking' end, 30), true)
        check("with the card's time and what 60 frames a second need", main.RemasterFirstStart.question, "Your graphics card takes 24 ms a frame; 60 frames a second need under 16.7. Lowering the settings keeps the game smooth: Ray Tracing goes first, then the Remastered Preset one level at a time.")
        check("its two buttons", Buttons(main.RemasterFirstStart.dialog), "Lower settings / Keep")
        check("not noted while it asks", Noted(), nil)
        check("asking changes nothing", Remastered(), before)
        main.RemasterFirstStart.dialog.content._button2:OnClick()
        check("Keep: kept", main.RemasterFirstStart.state, 'kept')
        check("Keep: nothing changed", Remastered(), before)
        check("Keep: the check is noted", Noted(), 1)
        WaitSeconds(3)
        check("Keep: the question is gone", IsDestroyed(main.RemasterFirstStart.dialog), true)
        check("Keep: still nothing changed, no note", Remastered() .. tostring(main.RemasterFirstStart.note), before .. "nil")
        NextGame(3, true)
        return
    end
    if stage == 3 then
        WaitFor(function() return GetGameTimeSeconds() > 11 end, 30)
        check("noted: the next game does not check again", main.RemasterFirstStart.state .. " " .. tostring(main.RemasterFirstStart.dialog), "off nil")
        NextGame(4, false)
        return
    end
    if stage == 4 then
        check("the question again", WaitFor(function() return main.RemasterFirstStart.state == 'asking' end, 30), true)
        main.RemasterFirstStart.dialog.content._button1:OnClick()
        check("Lower settings: the check is noted at once", Noted(), 1)
        check("Lower settings: lowered", WaitFor(function() return main.RemasterFirstStart.state == 'lowered' end, 60), true)
        local steps = main.RemasterFirstStart.steps
        local want = rayTracing and 3 or 2
        check("three steps where the card can ray trace (two elsewhere), the card slow after each", table.getn(steps), want)
        if rayTracing then
            check("first Ray Tracing alone (the preset then names High: Ultra without it)", tostring(steps[1].changes.remaster_ray_tracing) .. " " .. tostring(steps[1].changes.remaster_preset) .. " " .. tostring(steps[1].changes.remaster_atmosphere), "0 3 nil")
        end
        local medium, low = steps[want - 1], steps[want]
        check("then Medium", medium and medium.changes.remaster_preset, 2)
        check("then Low, and no further (never Off)", low and low.changes.remaster_preset, 1)
        check("each step measured again", low and math.floor(low.ms + 0.5), 24)
        check("the looks are Low's", Remastered(), LevelLine(1))
        local now = moho64ui_Looks()
        check("and the program has them (Ray Tracing, Atmosphere, Soft Shadows, Battle Effects off; effects at half)", tostring(now.rayTracing) .. " " .. tostring(now.atmosphere) .. " " .. tostring(now.softShadows) .. " " .. tostring(now.battleEffects) .. " " .. now.effectsQuality, "false false false false 0.5")
        check("the note", main.RemasterFirstStart.note, rayTracing
            and "To keep the game smooth, Ray Tracing is now off and the Remastered Preset is at Low. Your graphics card still takes 24 ms a frame: Upscaling in Options > Video can make it faster. Options > Video changes it back."
            or "To keep the game smooth, the Remastered Preset is at Low. Your graphics card still takes 24 ms a frame: Upscaling in Options > Video can make it faster. Options > Video changes it back.")
        check("the note shows, with its one button", main.RemasterFirstStart.noteDialog and main.RemasterFirstStart.noteDialog.content._button1.label:GetText(), LOC("<LOC _Ok>"))
        check("the profile keeps them (saved as the Options dialog saves)", Prefs.GetFromCurrentProfile('options').remaster_preset, 1)
        NextGame(5, false)
        return
    end
    if stage == 5 then
        check("the question once more", WaitFor(function() return main.RemasterFirstStart.state == 'asking' end, 30), true)
        moho64ui_SetFrameCostForTests(12)
        main.RemasterFirstStart.dialog.content._button1:OnClick()
        check("Lower settings: lowered", WaitFor(function() return main.RemasterFirstStart.state == 'lowered' end, 60), true)
        check("one step: the card fast after it", table.getn(main.RemasterFirstStart.steps), 1)
        check("the looks", Remastered(), LevelLine(rayTracing and 3 or 2))
        check("the note", main.RemasterFirstStart.note, rayTracing
            and "To keep the game smooth, Ray Tracing is now off. Your graphics card now takes 12 ms a frame. Options > Video changes it back."
            or "To keep the game smooth, the Remastered Preset is at Medium. Your graphics card now takes 12 ms a frame. Options > Video changes it back.")
        NextGame(6, false)
        return
    end
    -- 6: the question shown, and the program ended with it up (the screenshot)
    SetPreference("moho64ui_test_firststart", 1)
    SavePreferences()
    check("the question for the picture", WaitFor(function() return main.RemasterFirstStart.state == 'asking' end, 30), true)
    test:finish()
    ExitApplication()
end)
