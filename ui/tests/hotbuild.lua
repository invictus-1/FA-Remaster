-- FAF's hotbuild keys (/lua/keymap/hotbuild.lua; the Key Bindings dialog's "Hotbuilding" categories): a key bound to
-- a hotbuild action puts the building in the player's hand, as the build button would, and pressed again with the
-- same selection it goes on to the next building of its group (the cycle); Alt goes back. Bound here the way the Key
-- Bindings dialog binds (keymapper.SetUserKeyMapping, then its OK: SaveUserKeyMap, IN_ClearKeyMap,
-- IN_AddKeyMapTable(GetKeyMappings(true)), hotbuild.addModifiers() for the Shift and Alt forms), pressed with the
-- host's moho64ui_Key, and placed with a left click. Run it as a launched game with cheats (session_ai_cheats.lua,
-- the UEF commander), the world's picture, --frames 2400.
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
    WaitSeconds(6)
    local CM = import("/lua/ui/game/commandmode.lua")
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    cam:SnapTo(p, nil, 120)

    local actions = KeyMapper.GetKeyActions()
    check("FAF's hotbuild action for mass extractors is there", actions.mass and actions.mass.category, "hotbuilding")
    check("and the one for factories", actions.builders and actions.builders.category, "hotbuilding")
    -- J and K are free in the GPG preset (defaultKeyMap.lua) the lab's preferences use
    KeyMapper.SetUserKeyMapping('J', nil, 'mass')
    KeyMapper.SetUserKeyMapping('K', nil, 'builders')
    KeyMapper.SaveUserKeyMap()
    IN_ClearKeyMap()
    IN_AddKeyMapTable(KeyMapper.GetKeyMappings(true))
    import("/lua/keymap/hotbuild.lua").addModifiers()
    check("the key is bound to the hotbuild action", moho64ui_KeyMapAction("J"), actions.mass.action)
    check("with Shift too (hotbuild's modifiers)", moho64ui_KeyMapAction("Shift-J"), actions.mass.action)
    check("and with Alt", moho64ui_KeyMapAction("Alt-K"), actions.builders.action)
    check("the binding is kept in the preferences", import("/lua/user/prefs.lua").GetFromCurrentProfile("UserKeyMap").J, "mass")

    local function mode()
        local m = CM.GetCommandMode()
        return tostring(m[1]) .. " " .. tostring(m[2] and m[2].name)
    end
    SelectUnits({cdr})
    WaitSeconds(0.5)
    moho64ui_Key("j")
    WaitSeconds(0.2)
    check("J puts the T1 mass extractor in hand", mode(), "build ueb1103")
    -- placed with a left click on the nearest mass deposit (type 1): the extractor's order goes out
    local spot = nil
    for _, d in GetDepositsAroundPoint(p[1], p[3], 80, 1) or {} do
        if not spot or d.Dist < spot.Dist then spot = d end
    end
    check("a mass deposit near the commander", spot ~= nil, true)
    spot = spot or {X1 = p[1] + 12, X2 = p[1] + 12, Z1 = p[3] + 6, Z2 = p[3] + 6}
    local at = {(spot.X1 + spot.X2) / 2, p[2], (spot.Z1 + spot.Z2) / 2}
    cam:SnapTo(at, nil, 80)
    WaitSeconds(0.5)
    local heard = {}
    local oldIssued = CM.OnCommandIssued
    CM.OnCommandIssued = function(command)
        table.insert(heard, command)
        return oldIssued(command)
    end
    local s = view:Project(at)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    WaitSeconds(0.2)
    CM.OnCommandIssued = oldIssued
    local built = heard[1]
    check("a left click places it: a building order for the extractor",
        built and (built.CommandType .. " " .. built.Blueprint), "BuildMobile ueb1103")
    CM.EndCommandMode(true)
    cam:SnapTo(p, nil, 120)
    WaitSeconds(0.5)

    -- the cycle: each press goes on to the next factory the commander can build, and round again
    SelectUnits({cdr})
    WaitSeconds(0.2)
    local seen = {}
    for i = 1, 4 do
        moho64ui_Key("k")
        WaitSeconds(0.1)
        table.insert(seen, mode())
    end
    check("K and K again: land, air, naval factory, then land again", table.concat(seen, ", "),
        "build ueb0101, build ueb0102, build ueb0103, build ueb0101")
    -- (Alt held as a player holds it: hotbuild asks IsKeyDown("MENU"), the key's own state)
    moho64ui_Key(309, "Alt", "down")
    moho64ui_Key("k", "Alt")
    moho64ui_Key(309, "", "up")
    WaitSeconds(0.1)
    check("Alt-K goes back one", mode(), "build ueb0103")
    CM.EndCommandMode(true)
    -- the cycle starts again after its reset time (the options' hotbuild_cycle_reset_time, 1.1 s here)
    WaitSeconds(2)
    SelectUnits({cdr})
    WaitSeconds(0.2)
    moho64ui_Key("k")
    WaitSeconds(0.1)
    check("after the reset time it starts at the land factory again", mode(), "build ueb0101")
    CM.EndCommandMode(true)

    -- nothing selected: no building in hand
    SelectUnits(nil)
    WaitSeconds(0.2)
    moho64ui_Key("j")
    WaitSeconds(0.1)
    check("with nothing selected the key does nothing", mode(), "false false")

    -- where the player binds them: the Key Bindings dialog (F1, toggle_key_bindings; FAF's keybindings.lua lists the
    -- actions by keycategories.lua's categories, the three hotbuild ones among them), J's binding listed as one
    ConExecute(actions.toggle_key_bindings.action)
    WaitSeconds(0.3)
    local categories = import("/lua/keymap/keycategories.lua").keyCategories
    check("the Key Bindings dialog's categories have Hotbuilding", categories.hotbuilding ~= nil, true)
    check("and Alternative Hotbuilding", categories.hotbuildingAlternative ~= nil, true)
    local details = KeyMapper.GetKeyMappingDetails()
    check("J's binding is listed as a Hotbuilding action", details.J and details.J.category, "HOTBUILDING")
    check("the dialog closes again without an error", pcall(import("/lua/ui/dialogs/keybindings.lua").CloseUI), true)
    LOG("TEST done: " .. failed .. " failed")
end)
