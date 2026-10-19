-- FA Remastered: the first-start graphics check (Chris, 2026-10-18: "if the PC can't hold 60 fps, turn ray tracing
-- off first, then step the preset down, and say so once"; "Make sure it does a pop up asking if the player wants to
-- lower settings"). This program's own, not the original's (this file is a hook: the program reads it after the
-- game's /lua/ui/game/gamemain.lua, which stays as it is). Every remastered look starts on at its highest; once per
-- profile, in the player's first game (not a replay), when the game has settled, the graphics card's time per frame
-- of the world's picture is measured (the host's moho64ui_MeasureFrameCost: the median of 120 frames, the card's own
-- time, not the frame interval that vertical sync holds at the screen's rate). Above 16.7 ms (60 frames a second) a
-- dialog asks whether to lower the settings. Only on "Lower settings": Ray Tracing goes first, then the Remastered
-- Preset one level at a time (options.lua's RemasterLowerStep), measured again after each step, until the card keeps
-- up or Low is reached (never Off, the original's look); then one note says what was changed. The changes go in as the
-- Options dialog's OK puts them in (optionslogic SetCurrent: each option's own `set`, the remaster_* console commands,
-- and the profile's options saved), so they stand as the player's own choice. Either way the profile's options then
-- hold remaster_first_start_check = 1 and the check never runs again for that profile. Without the host's measurement
-- (another program) nothing runs; a run without a window never finishes one (the card draws no frames), so it never
-- asks.
local RemasterUIUtil = import("/lua/ui/uiutil.lua")

-- What the check is doing and what it showed, for the tests: state 'waiting', 'measuring', 'asking', 'lowering',
-- 'fast' (no question), 'nothing' (slow, but nothing left to lower: no question), 'kept', 'lowered'; ms (the first
-- measurement), question and note (the dialogs' texts), dialog and noteDialog (the popups), steps (each step's
-- changes and the time measured after it).
RemasterFirstStart = { state = 'off', steps = {} }

local remasterCheckKey = 'remaster_first_start_check'
local remasterBudgetMs = 1000 / 60
local remasterLevelNames = { [0] = "Off", "Low", "Medium", "High", "Ultra", "Custom" }

local function RemasterLog(text)
    LOG("FA Remastered: the first-start graphics check: " .. text)
end

-- The check has run for this profile: noted in its options (beside the Options menu's own keys; the menu's Reset
-- drops it with the rest, and the check then runs once more).
local function RemasterCheckDone()
    local Prefs = import("/lua/user/prefs.lua")
    local profileOptions = Prefs.GetFromCurrentProfile('options') or {}
    profileOptions[remasterCheckKey] = 1
    Prefs.SetToCurrentProfile('options', profileOptions)
    SavePreferences()
end

-- The card's milliseconds a frame: a new measurement, waited for (the host's frames give it).
local function RemasterMeasure()
    RemasterFirstStart.state = 'measuring'
    moho64ui_MeasureFrameCost()
    local ms
    repeat
        WaitSeconds(0.1)
        ms = moho64ui_FrameCost()
    until ms
    return ms
end

-- Ray Tracing off first, then the Remastered Preset a level at a time, measured after each step; then the note.
local function RemasterLower(ms)
    local logic = import("/lua/options/optionslogic.lua")
    local looks = import("/lua/options/options.lua")
    RemasterFirstStart.state = 'lowering'
    local rayTracingOff, level = false, nil
    while ms > remasterBudgetMs do
        local current = logic.GetCurrent()
        local changes, stepLevel = looks.RemasterLowerStep(current)
        if not changes then break end
        for key, value in changes do current[key] = value end
        logic.SetCurrent(current)  -- (the Options dialog's OK: each changed option's `set`, then the profile saved)
        if changes.remaster_ray_tracing == 0 then rayTracingOff = true end
        level = stepLevel or level
        ms = RemasterMeasure()
        table.insert(RemasterFirstStart.steps, { changes = changes, ms = ms })
        RemasterLog(string.format("%s; the card now takes %.2f ms a frame",
            stepLevel and ("the Remastered Preset lowered to " .. remasterLevelNames[stepLevel]) or "Ray Tracing turned off", ms))
    end
    local named = logic.GetCurrent().remaster_preset
    local changed = {}
    if rayTracingOff then table.insert(changed, "Ray Tracing is now off") end
    if level then
        if named == level then
            table.insert(changed, "the Remastered Preset is at " .. remasterLevelNames[level])
        else
            table.insert(changed, "the remastered looks are lowered to the " .. remasterLevelNames[level] .. " level")
        end
    end
    local text = "To keep the game smooth, " .. table.concat(changed, " and ") .. ". "
    if ms > remasterBudgetMs then
        text = text .. string.format("Your graphics card still takes %.0f ms a frame: Upscaling in Options > Video can make it faster. ", ms)
    else
        text = text .. string.format("Your graphics card now takes %.0f ms a frame. ", ms)
    end
    text = text .. "Options > Video changes it back."
    RemasterFirstStart.note = text
    RemasterFirstStart.state = 'lowered'
    RemasterLog(text)
    RemasterFirstStart.noteDialog = RemasterUIUtil.ShowInfoDialog(GetFrame(0), text, "<LOC _Ok>")
end

function RemasterFirstStartCheck()
    if not moho64ui_MeasureFrameCost or SessionIsReplay() then return end
    if (import("/lua/user/prefs.lua").GetFromCurrentProfile('options') or {})[remasterCheckKey] then return end
    -- (settled: the loading screen is gone when the game's first frame runs this; the opening camera glide to the
    -- commander, StartupSequence above, is over after 2.5 s; and no cinematic shows)
    RemasterFirstStart.state = 'waiting'
    while GetGameTimeSeconds() < 5 or IsNISMode() do WaitSeconds(0.5) end
    local ms = RemasterMeasure()
    RemasterFirstStart.ms = ms
    if ms <= remasterBudgetMs then
        RemasterLog(string.format("the card takes %.2f ms a frame: fast enough for 60 frames a second", ms))
        RemasterCheckDone()
        RemasterFirstStart.state = 'fast'
        return
    end
    local logic = import("/lua/options/optionslogic.lua")
    if not import("/lua/options/options.lua").RemasterLowerStep(logic.GetCurrent()) then
        RemasterLog(string.format("the card takes %.2f ms a frame, but the remastered looks are as low as they go: no question", ms))
        RemasterCheckDone()
        RemasterFirstStart.state = 'nothing'
        return
    end
    while IsNISMode() do WaitSeconds(0.5) end
    local text = string.format("Your graphics card takes %.0f ms a frame; 60 frames a second need under 16.7. Lowering the settings keeps the game smooth: Ray Tracing goes first, then the Remastered Preset one level at a time.", ms)
    RemasterFirstStart.question = text
    RemasterFirstStart.state = 'asking'
    RemasterLog(string.format("the card takes %.2f ms a frame: asking", ms))
    -- (the game runs on behind it, as behind the game's own dialogs; Enter lowers, Escape keeps)
    RemasterFirstStart.dialog = RemasterUIUtil.QuickDialog(GetFrame(0), text,
        "Lower settings", function()
            RemasterLog("the player chose to lower the settings")
            RemasterCheckDone()
            ForkThread(RemasterLower, ms)
        end,
        "Keep", function()
            RemasterLog("the player kept the settings")
            RemasterCheckDone()
            RemasterFirstStart.state = 'kept'
        end,
        nil, nil, true, { escapeButton = 2, enterButton = 1, worldCover = false })
end

local RemasterGameOnFirstUpdate = OnFirstUpdate
function OnFirstUpdate()
    RemasterGameOnFirstUpdate()
    ForkThread(RemasterFirstStartCheck)
end
