-- Colour Vision (ours, the Options dialog's Interface tab; Chris, 2026-10-23): the armies' colours as the player sees
-- them from a palette that stays apart for a kind of colour blindness. The option switched the dialog's way
-- (optionslogic.SetCurrent: its `set`, the console's remaster_color_vision); then what the scripts get
-- (GetArmiesTable's color and iconColor: the score board, chat names, the unit panel), what the picture's strategic
-- icons and minimap get (moho64ui_IconColors, the host's SetIconColors), that the army's own colour stays as given
-- (moho64ui_ArmyColors: what a marker ping sends the simulation, our ping.lua hook), the team colour button's own
-- colours winning while it is on, and Off giving back the original's colours exactly.
-- FAF's GetArmiesTable is a copy renewed every 4 seconds (/lua/ui/override/ArmiesTable.lua): the test waits that long
-- after each switch. moho64ui_IconColors's numbers are the Lua state's (single-precision): compared as numbers.
-- Run it like game.lua with session_ai_cheats.lua at 1920x1080, --frames 2700. Ends with "TEST done".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local logic = import("/lua/options/optionslogic.lua")
local looks = import("/lua/options/options.lua")
local ping = import("/lua/ui/game/ping.lua")

-- each kind's first two army slots and the civilians' (ui/user/session_bindings.cpp, kVisionPalettes)
local palettes = {
    [1] = { "fff0e442", "ff785ef0", civilian = "ffdddddd" },  -- Deuteranopia
    [2] = { "fff0e442", "ff785ef0", civilian = "ffdddddd" },  -- Protanopia
    [3] = { "ff44bb99", "ffdc267f", civilian = "ffdddddd" },  -- Tritanopia
    [4] = { "ff648fff", "ffffb000", civilian = "ffdddddd" },  -- High Contrast
}
local function SetVision(mode)
    local now = logic.GetCurrent()
    now.remaster_color_vision = mode
    logic.SetCurrent(now)
    WaitSeconds(4.5)  -- (FAF's copy of the armies' table)
end
local function Num(hex) return tonumber(hex, 16) end
local function Armies()
    local out = {}
    for i, a in GetArmiesTable().armiesTable do
        out[i] = { color = string.lower(a.color), icon = string.lower(a.iconColor), civilian = a.civilian }
    end
    return out
end

ForkThread(function()
    WaitSeconds(3)
    -- the option is in the Interface tab, Off by default
    local item
    for _, it in looks.options.ui.items do
        if it.key == 'remaster_color_vision' then item = it end
    end
    check("Colour Vision is in the Interface tab", item ~= nil, true)
    check("its default is Off", item and item.default, 0)
    check("its kinds", item and table.getn(item.custom.states), 5)

    SetVision(0)
    local off = Armies()
    local count = table.getn(GetArmiesTable().armiesTable)
    check("Off: the console's value", moho64ui_Looks().colorVision, 0)
    check("Off: the session has armies", count >= 2, true)
    check("Off: the icons keep the armies' own colours", table.getn(moho64ui_IconColors()), 0)
    check("Off: army 1 as the lobby gave it (FAF's Cybran red)", off[1].color, "ffe80a0a")
    check("Off: army 2 as the lobby gave it (FAF's dark red)", off[2].color, "ff901427")
    for i = 1, count do
        local given, seen = moho64ui_ArmyColors(i)
        check("Off: army " .. i .. " seen as given", seen, given)
    end
    check("Off: a ping's colour goes out as it is", ping.RemasterPingColorForSim({ Owner = 0, Color = off[1].color }), nil)
    check("the ping module sends through its own SimCallback (our hook's)", ping.SimCallback ~= nil and ping.SimCallback ~= _G.SimCallback, true)

    for mode = 1, 4 do
        SetVision(mode)
        local p = palettes[mode]
        local now = Armies()
        check(mode .. ": the console's value", moho64ui_Looks().colorVision, mode)
        check(mode .. ": the host's", (moho64ui_ColorVision()), mode)
        check(mode .. ": army 1's colour", now[1].color, p[1])
        check(mode .. ": army 2's colour", now[2].color, p[2])
        check(mode .. ": army 1's icon colour as its colour", now[1].icon, p[1])
        local icons = moho64ui_IconColors()
        check(mode .. ": the picture's icon colours, one per army", table.getn(icons), count)
        check(mode .. ": army 1's icons", icons[1], Num(p[1]))
        check(mode .. ": army 2's icons", icons[2], Num(p[2]))
        local models = moho64ui_ModelColors()  -- (the units themselves: WorldPicture::SetArmyColors)
        check(mode .. ": the units' own colours, as the icons", table.getn(models) .. " " .. tostring(models[1] == icons[1] and models[2] == icons[2]), count .. " true")
        local seenAll, apart = true, true
        for i = 1, count do
            local want = now[i].civilian and p.civilian or nil
            if want and now[i].color ~= want then seenAll = false end
            if icons[i] ~= Num(now[i].color) then seenAll = false end
            local given = moho64ui_ArmyColors(i)
            if string.lower(given) ~= off[i].color then apart = false end
            for j = i + 1, count do
                if not now[i].civilian and not now[j].civilian and now[i].color == now[j].color then seenAll = false end
            end
        end
        check(mode .. ": every army's icons as its colour, civilians grey, no two armies alike", seenAll, true)
        check(mode .. ": every army's own colour (the simulation's) unchanged", apart, true)
        -- a marker ping: out with the army's own colour, shown in the palette's
        -- (exactly as the original's GetArmiesTable gives it, "FFe80a0a" as GameColors.lua spells it)
        check(mode .. ": a ping's colour goes to the simulation as the army's own",
            ping.RemasterPingColorForSim({ Owner = 0, Color = now[1].color }), (moho64ui_ArmyColors(1)))
        check(mode .. ": a ping from the simulation is shown in the palette's",
            ping.RemasterPingColorShown({ Owner = 0, Color = off[1].color }), now[1].color)
        check(mode .. ": another colour is left alone", ping.RemasterPingColorForSim({ Owner = 0, Color = "ff123456" }), nil)
        if mode > 1 then
            check(mode .. ": the kind before's colour goes back too (FAF's copy lags)",
                ping.RemasterPingColorForSim({ Owner = 0, Color = palettes[mode - 1][1] }), (moho64ui_ArmyColors(1)))
        end
    end

    -- the team colour button (FAF's list) has its own colours while it is on
    SetVision(1)
    TeamColorMode("ff436eee,ffe80a0a")
    TeamColorMode(true)
    local icons = moho64ui_IconColors()
    check("team colours on: the button's colours win", icons[1], Num("ff436eee"))
    local _, teamOn = moho64ui_ColorVision()
    check("team colours on: the host says so", teamOn, true)
    TeamColorMode(false)
    icons = moho64ui_IconColors()
    check("team colours off again: Colour Vision's", icons[1], Num(palettes[1][1]))
    looks.RemasterRecolourScoreBoard()  -- (no error with the score board up)

    -- back to Off: the original's colours exactly
    SetVision(0)
    local back = Armies()
    local same = true
    for i = 1, count do
        if back[i].color ~= off[i].color or back[i].icon ~= off[i].icon then same = false end
    end
    check("Off again: every army's colours as at first", same, true)
    check("Off again: the icons keep the armies' own colours", table.getn(moho64ui_IconColors()), 0)
    check("Off again: the units keep the game's own colours", table.getn(moho64ui_ModelColors()), 0)
    check("Off again: a ping's colour shown as it is", ping.RemasterPingColorShown({ Owner = 0, Color = off[1].color }), nil)
    LOG("TEST done: " .. failed .. " failed")
end)
