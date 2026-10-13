-- The in-game interface on a running game, checked from the script side of a launched game's
-- state: the session's facts, units, selection, the panels' data, the world view and a move order.
-- It needs a recorded game configuration (what --session-out writes when a game is launched from
-- the lobby) with the player in the first slot and one opponent; run it with fog of war on and
-- with the configuration's FogOfWar set to "none" (the checks about the enemy differ):
--   moho64ui --init <init.lua> --stage setup --session <config.lua> --seed 7 --game-lua ui/tests/game.lua
--            --frames 2200 --log out.log
-- (and once with GameOptions.GameSpeed = "adjustable": the checks of the game's speed differ)
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
-- (With --prefs the run saves that preferences file, as a game does: give it a copy.)
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local function run()
    WaitSeconds(5)  -- until the loading picture that covers the screen has faded

    -- the session's facts
    check("a session is active and it is no replay", tostring(SessionIsActive()) .. tostring(SessionIsReplay()), "truefalse")
    check("the interface state is 'game'", GetCurrentUIState(), "game")
    check("the player's army is the first", GetFocusArmy(), 1)
    local armies = GetArmiesTable()
    check("the armies table counts its armies", armies.numArmies >= 2 and armies.numArmies == table.getn(armies.armiesTable), true)
    local me = armies.armiesTable[armies.focusArmy]
    check("the player's army is human, has a name and a colour", tostring(me.human) .. type(me.nickname) .. string.len(me.color), "truestring8")
    local scenario = SessionGetScenarioInfo()
    check("the scenario has the map's size", type(scenario.size) == "table" and scenario.size[1] > 0, true)
    check("the same scenario table on every call", SessionGetScenarioInfo() == scenario, true)
    check("the first beat's sync told the playable area", scenario.PlayableAreaWidth, scenario.size[1])
    local t0 = GetGameTimeSeconds()
    check("ten ticks are a second", GetSimTicksPerSecond() .. " " .. tostring(math.abs(GameTick() / 10 - t0) < 0.001), "10 true")
    local eco = GetEconomyTotals()
    check("the economy has stored mass and energy and room for more",
        eco.stored.MASS > 0 and eco.stored.ENERGY > 0 and eco.maxStorage.MASS >= eco.stored.MASS, true)
    check("the game time as text", string.find(GetGameTime(), "^%d%d:%d%d:%d%d$") ~= nil, true)
    check("each beat arrives as Sync", type(Sync) == "table" and type(PreviousSync) == "table", true)

    -- units
    local avatars = GetArmyAvatars()
    check("one commander", table.getn(avatars), 1)
    local cdr = avatars[1]
    local bp = cdr:GetBlueprint()
    check("a unit knows its blueprint", bp.BlueprintId, cdr:GetUnitId())
    check("its entity id is a string, and finds it again", type(cdr:GetEntityId()) .. tostring(GetUnitById(cdr:GetEntityId()) == cdr), "stringtrue")
    check("army, health, alive", cdr:GetArmy() .. tostring(cdr:GetHealth() > 0 and cdr:GetHealth() <= cdr:GetMaxHealth()) .. tostring(cdr:IsDead()), "1truefalse")
    check("categories by name", tostring(cdr:IsInCategory("COMMAND")) .. tostring(cdr:IsInCategory("FACTORY")), "truefalse")
    check("the category functions take units", tostring(EntityCategoryContains(categories.COMMAND, cdr)) ..
        table.getn(EntityCategoryFilterDown(categories.COMMAND, {cdr})) .. table.getn(EntityCategoryFilterOut(categories.COMMAND, {cdr})), "true10")
    check("the same object for a unit every time", GetArmyAvatars()[1] == cdr, true)
    local pos = cdr:GetPosition()
    check("a position on the map", pos[1] > 0 and pos[1] < scenario.size[1] and pos[3] > 0 and pos[3] < scenario.size[2], true)

    -- selection
    SelectUnits(nil)  -- the game script may have selected the commander for the player
    check("nothing selected is nil", GetSelectedUnits(), nil)
    SelectUnits({cdr})
    local sel = GetSelectedUnits()
    check("SelectUnits selects", tostring(sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr), "true")
    -- selecting a unit plays its select sound: the blueprint's sound object, of the interface's bank
    SelectUnits(nil)
    local cues = moho64ui_SoundInfo().played
    SelectUnits({cdr})
    check("selecting the commander plays one cue", moho64ui_SoundInfo().played, cues + 1)
    check("its select sound is a wave of its faction's select bank", string.find(moho64ui_SoundInfo().wave, "Select#", 1, true) ~= nil, true)
    local orders, toggles, buildable = GetUnitCommandData(sel)
    local has = {}
    for _, o in orders do has[o] = true end
    check("the selection's orders come from the blueprint", tostring(has.RULEUCC_Move) .. tostring(has.RULEUCC_Attack) .. tostring(has.RULEUCC_Nuke), "truetruenil")
    check("what it can build is a category", EntityCategoryContains(buildable, cdr:GetUnitId()), false)
    local factory = string.sub(cdr:GetUnitId(), 1, 2) .. "b0101"  -- the faction's first land factory
    check("a commander can build its land factory", EntityCategoryContains(buildable, factory), true)

    -- the map's border: the game script names the machinery's mesh for the skin and the map's shape
    local borders = moho64ui_MapBorders()
    check("the script names one border mesh for a square map", table.getn(borders), 1)
    check("it is a map-border mesh blueprint", string.find(borders[1] or "", "/meshes/game/map-border_squ_", 1, true), 1)

    -- the world view and the camera
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    -- the game's script opens on the commander (gamemain.StartupSequence: UIZoomTo(avatars, 1));
    -- the checks after this start from the camera's own first state
    local opened = cam:GetFocusPosition()
    check("the game opens with the camera on the commander, from a zoom of 40",
        string.format("%.1f", cam:GetZoom()) == "40.0" and math.abs(opened[1] - pos[1]) < 1 and math.abs(opened[3] - pos[3]) < 1, true)
    cam:Reset()
    local s = view:Project(pos)
    moho64ui_MouseMove(s.x, s.y)
    local w = GetMouseWorldPos()
    check("screen and world agree", math.abs(w[1] - pos[1]) < 0.5 and math.abs(w[3] - pos[3]) < 0.5, true)
    check("the same camera object for a name", GetCamera("WorldCamera") == cam, true)
    local saved = cam:SaveSettings()
    local zoom = cam:GetZoom()
    cam:SetZoom(zoom / 2, 0)
    check("SetZoom", cam:GetZoom(), zoom / 2)
    cam:RestoreSettings(saved)
    check("RestoreSettings brings the zoom back", cam:GetZoom(), zoom)
    -- the camera as the original's: it starts as far out as it goes (the map, 1024 wide in a 16:9
    -- view, takes 1 / 1.4 of what would fill it), north up, looking straight down; a wheel notch
    -- brings the target zoom nearer by 2^-0.4 and the zoom glides after it; nearer, it leans
    check("the camera starts at its farthest", zoom, cam:GetMaxZoom())
    check("the farthest zoom by the map and the view", math.abs(zoom - (1024 - 1 + 5) * 1.4 * 16 / 9) < 0.01, true)
    check("the nearest zoom", cam:GetMinZoom(), 5)
    -- the zoom is the width of world the view shows: from the farthest, the map's 1024 units take
    -- 1024 / (1029 * 1.4) of the view's height (measured on a screenshot of the original: 0.7087)
    local top, bottom = view:Project({512, pos[2], 0}), view:Project({512, pos[2], 1024})
    local share = (bottom.y - top.y) / view.Height()
    check("from the farthest the map takes 0.71 of the view's height", math.abs(share - 1024 / (1029 * 1.4)) < 0.01, true)
    check("heading and pitch at the start", string.format("%.2f %.2f", saved.Heading, saved.Pitch), "3.14 1.57")
    moho64ui_MouseWheel(s.x, s.y, 1)
    check("a wheel notch sets the target zoom", math.abs(cam:GetTargetZoom() - zoom * math.pow(2, -0.4)) < 0.01, true)
    check("the zoom glides: not there at once", cam:GetZoom(), zoom)
    WaitSeconds(2)
    check("... and there after a while", cam:GetZoom(), cam:GetTargetZoom())
    check("nearer, the camera leans", cam:SaveSettings().Pitch < saved.Pitch, true)
    local f = cam:GetFocusPosition()
    check("zooming in goes towards the cursor", f[1] > 512 and f[3] < 512, true)
    -- the arrow keys move the view while they are held: left is west, up is north
    moho64ui_Key(316, "", "down")
    WaitSeconds(0.3)
    moho64ui_Key(316, "", "up")
    local g = cam:GetFocusPosition()
    check("the left arrow moves the view west", g[1] < f[1] - 1 and math.abs(g[3] - f[3]) < 0.01, true)
    WaitSeconds(0.2)
    check("and it stands when the key is let go", cam:GetFocusPosition()[1], g[1])
    moho64ui_Key(317, "", "down")
    WaitSeconds(0.3)
    moho64ui_Key(317, "", "up")
    check("the up arrow moves it north", cam:GetFocusPosition()[3] < g[3] - 1, true)
    -- dragging with the middle button moves the view with the mouse: the ground stays under the pointer
    local d0 = cam:GetFocusPosition()
    moho64ui_MouseButton("Middle", true, 900, 500)
    moho64ui_MouseMove(880, 500)
    moho64ui_MouseButton("Middle", false, 880, 500)
    local d1 = cam:GetFocusPosition()
    local shown = cam:GetZoom() / view.Width()  -- map units per pixel
    check("a middle-button drag 20 pixels to the left moves the view east by as much", math.abs((d1[1] - d0[1]) - 20 * shown) < 0.01 and math.abs(d1[3] - d0[3]) < 0.01, true)
    cam:RestoreSettings(saved)
    WaitSeconds(0.1)

    -- an order: a right click on the ground moves the selection
    s = view:Project(pos)
    moho64ui_MouseButton("Right", true, s.x + 40, s.y)
    moho64ui_MouseButton("Right", false, s.x + 40, s.y)
    WaitSeconds(3)
    local now = cdr:GetPosition()
    check("the commander walks towards a right click", now[1] > pos[1] + 2, true)
    -- the world's sounds: from far out its steps are too soft to play; near, they are heard
    local heardFar = moho64ui_SoundInfo().world
    cam:SnapTo(now, nil, 60)
    WaitSeconds(2.5)
    check("near the walking commander its steps are heard", moho64ui_SoundInfo().world > heardFar, true)
    cam:RestoreSettings(saved)
    local queue = cdr:GetCommandQueue()
    check("its queue holds the move order", table.getn(queue) .. " " .. tostring(queue[1] and queue[1].type), "1 Move")
    check("an order's command name", GetUnitCommandFromCommandCap("RULEUCC_Stop"), "UNITCOMMAND_Stop")
    IssueUnitCommand({cdr}, "Stop")
    WaitSeconds(1)
    local a = cdr:GetPosition()
    WaitSeconds(1)
    local b = cdr:GetPosition()
    check("Stop halts it", math.abs(a[1] - b[1]) < 0.01 and math.abs(a[3] - b[3]) < 0.01, true)
    check("and empties its queue", table.getn(cdr:GetCommandQueue()), 0)

    -- switches: set through the simulation, read back from it
    local fire = GetFireState({cdr})
    check("the fire state is one of the three", fire == 0 or fire == 1 or fire == 2, true)
    local other = math.mod(fire + 1, 3)
    SetFireState({cdr}, other)
    WaitSeconds(0.3)
    check("SetFireState reaches the simulation and comes back", GetFireState({cdr}), other)
    SetFireState({cdr}, fire)
    check("nothing is paused", GetIsPaused({cdr}), false)

    -- command modes: the script's mode decides what a left click in the world does
    local CM = import("/lua/ui/game/commandmode.lua")
    ConExecute("StartCommandMode order RULEUCC_Patrol")
    local mode = CM.GetCommandMode()
    check("the console command StartCommandMode starts the script's mode", tostring(mode[1]) .. " " .. tostring(mode[2].name), "order RULEUCC_Patrol")
    CM.StartCommandMode("build", {name = factory})
    local before = GetEconomyTotals().stored.MASS
    -- 16 map units east of the commander: the simulation takes no site closer to the builder
    local site = cdr:GetPosition()
    site[1] = site[1] + 16
    s = view:Project(site)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    check("the script hears of the order and ends its mode", CM.GetCommandMode()[1], false)
    WaitSeconds(4)
    check("in build mode a left click makes the commander build there", cdr:GetWorkProgress() > 0 and GetEconomyTotals().stored.MASS < before, true)
    local focus = cdr:GetFocus()
    check("what it builds is its focus", focus ~= nil and focus:GetUnitId(), factory)
    check("and it uses mass for it", cdr:GetEconData().massConsumed > 0, true)

    -- a right click orders by what is under the cursor
    local here = cdr:GetPosition()
    cam:SnapTo(here, nil, 80)
    WaitSeconds(0.3)
    s = view:Project({here[1] - 20, here[2], here[3]})
    moho64ui_MouseMove(s.x, s.y)
    check("over the ground a right click moves", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    s = view:Project(here)
    moho64ui_MouseMove(s.x, s.y)
    check("over the selected unit itself too", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
    -- what the mouse is over, for the game's unit panel
    local info = GetRolloverInfo()
    check("the info under the mouse is the commander's", info ~= nil and info.blueprintId, cdr:GetUnitId())
    check("with the unit itself, its id and its army from 0", tostring(info.userUnit == cdr) .. " " .. info.entityId .. " " .. info.armyIndex, "true " .. cdr:GetEntityId() .. " 0")
    check("its health and its army's colour", info.health > 0 and info.health <= info.maxHealth and string.len(info.teamColor), 8)
    check("and what it works on", info.focus ~= nil and info.focus.blueprintId, factory)
    moho64ui_MouseMove(s.x - 300, s.y)
    check("over the ground: nothing", GetRolloverInfo(), nil)
    moho64ui_MouseMove(s.x, s.y)
    s = view:Project(focus:GetPosition())
    moho64ui_MouseMove(s.x, s.y)
    check("over a factory being built it guards (helps)", view:GetRightMouseButtonOrder(), "RULEUCC_Guard")
    local enemy = GetUnitById("1048576")  -- the second army's first unit: its commander
    check("the other army's commander is there", enemy ~= nil and enemy:GetArmy(), 2)
    local there = enemy:GetPosition()
    cam:SnapTo(there, nil, 80)
    WaitSeconds(0.3)
    s = view:Project(there)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.2)
    local pointer
    if SessionGetScenarioInfo().Options.FogOfWar == "none" then
        -- a game without fog of war: the enemy across the map is seen, and the mouse is told of it
        check("over an enemy it attacks", view:GetRightMouseButtonOrder(), "RULEUCC_Attack")
        local info = GetRolloverInfo()
        check("the info under the mouse is the enemy commander's", info ~= nil and info.entityId, "1048576")
        -- the mouse pointer: the world view's script chooses it every frame while the mouse is over it
        pointer = moho64ui_Cursor()
        check("the pointer over an enemy is the attack pointer", string.find(pointer or "", "/cursors/attack", 1, true) ~= nil, true)
        moho64ui_MouseMove(s.x - 300, s.y)
        WaitSeconds(0.2)
        pointer = moho64ui_Cursor()
        check("over the ground it is the plain one again", string.find(pointer or "", "/cursors/attack", 1, true), nil)
        moho64ui_MouseMove(s.x, s.y)
        moho64ui_MouseButton("Right", true, s.x, s.y)
        moho64ui_MouseButton("Right", false, s.x, s.y)
        WaitSeconds(1)
        queue = cdr:GetCommandQueue()
        check("and the right click gives the attack order", table.getn(queue) .. " " .. tostring(queue[1] and queue[1].type), "1 Attack")
    else
        -- with fog of war the player's army knows nothing of a commander across the map: it is not
        -- under the mouse, nothing is told of it, and a right click there is a move
        check("an enemy the army does not see is not attacked by a right click", view:GetRightMouseButtonOrder(), "RULEUCC_Move")
        check("nothing is told of it", GetRolloverInfo(), nil)
        pointer = moho64ui_Cursor()
        check("and the pointer stays the plain one", string.find(pointer or "", "/cursors/attack", 1, true), nil)
        moho64ui_MouseButton("Right", true, s.x, s.y)
        moho64ui_MouseButton("Right", false, s.x, s.y)
        WaitSeconds(1)
        queue = cdr:GetCommandQueue()
        check("the right click there is a move order", table.getn(queue) .. " " .. tostring(queue[1] and queue[1].type), "1 Move")
    end
    IssueUnitCommand({cdr}, "Stop")
    cam:RestoreSettings(saved)

    SelectUnits(nil)
    check("SelectUnits(nil) selects nothing", GetSelectedUnits(), nil)
    -- clicks in the world: a click on a unit selects it, a double click all of its kind on screen
    here = cdr:GetPosition()
    cam:SnapTo(here, nil, 80)
    WaitSeconds(0.3)
    s = view:Project(here)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    sel = GetSelectedUnits()
    check("a click on the commander selects it", sel ~= nil and sel[1] == cdr, true)
    SelectUnits(nil)
    moho64ui_MouseButton("Left", true, s.x, s.y, "Double")
    moho64ui_MouseButton("Left", false, s.x, s.y)
    sel = GetSelectedUnits()
    check("a double click selects its kind on screen (here: the one commander)", sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr, true)
    SelectUnits(nil)
    -- turning the camera: Space held and the mouse (a full turn for a view's width, the lean with
    -- the mouse's height); it swings back when Space is let go
    local turn0 = cam:SaveSettings()
    moho64ui_Key(32, "", "down")
    moho64ui_MouseMove(900, 500)
    moho64ui_MouseMove(1100, 440)
    local turn1 = cam:SaveSettings()
    check("Space and the mouse turn the camera", math.abs((turn0.Heading - turn1.Heading) - 200 * 2 * math.pi / view.Width()) < 0.001, true)
    check("and lean it", turn1.Pitch < turn0.Pitch - 0.1, true)
    moho64ui_Key(32, "", "up")
    WaitSeconds(0.5)
    local turn2 = cam:SaveSettings()
    check("it swings back when Space is let go", math.abs(turn2.Heading - turn0.Heading) < 0.001 and math.abs(turn2.Pitch - turn0.Pitch) < 0.001, true)
    moho64ui_Key(324, "", "down")
    WaitSeconds(0.3)
    moho64ui_Key(324, "", "up")
    local turn3 = cam:SaveSettings().Heading
    WaitSeconds(0.3)
    check("Insert turns it, and it stays turned", turn3 < turn0.Heading - 0.2 and cam:SaveSettings().Heading == turn3, true)
    cam:Reset()
    check("Reset brings north up again", string.format("%.2f", cam:SaveSettings().Heading), "3.14")
    moho64ui_Key(32, "", "down")
    moho64ui_MouseMove(900, 500)
    moho64ui_MouseMove(1100, 440)
    moho64ui_Key(32, "", "up")
    check("from far out Space and the mouse do not turn it", string.format("%.2f", cam:SaveSettings().Heading), "3.14")

    -- selecting by category (the keys for "all engineers", "nearest idle engineer", ...), adding
    -- to the selection, and the camera going to units
    local function count() return table.getn(GetSelectedUnits() or {}) end
    local at = cdr:GetPosition()
    SelectUnits(nil)
    UISelectionByCategory("COMMAND", false, false, false, false)
    sel = GetSelectedUnits()
    check("UISelectionByCategory selects the commander by its category", sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr, true)
    UISelectionByCategory("NAVAL", false, false, false, false)
    check("a category nothing is in selects nothing", count(), 0)
    UISelectionByCategory("NAVAL", true, false, false, false)
    check("nothing found leaves nothing selected", count(), 0)
    AddSelectUnits({cdr})
    check("AddSelectUnits adds to the selection", count(), 1)
    UISelectionByCategory("NAVAL", true, false, false, false)
    check("adding nothing keeps the selection", count(), 1)
    SelectUnits(nil)
    UISelectionByCategory("COMMAND", false, false, false, true)
    check("with mustBeIdle only when it has nothing to do", count() == 1, cdr:IsIdle())
    SelectUnits(nil)
    UISelectionByCategory("ALLUNITS", false, false, true, false)
    check("nearestToMouse selects one unit", count(), 1)
    cam:SnapTo(at, nil, 60)
    WaitSeconds(0.2)
    SelectUnits(nil)
    UISelectionByCategory("COMMAND", false, true, false, false)
    check("in view: the commander on screen is found", count(), 1)
    cam:SnapTo({at[1] > 512 and at[1] - 300 or at[1] + 300, at[2], at[3]}, nil, 60)
    WaitSeconds(0.2)
    UISelectionByCategory("COMMAND", false, true, false, false)
    check("in view: off screen it is not", count(), 0)
    UIZoomTo({cdr}, 0)
    local zf = cam:GetFocusPosition()
    check("UIZoomTo shows a unit from a zoom of 40", string.format("%.1f", cam:GetZoom()), "40.0")
    check("with the unit in the middle", math.abs(zf[1] - at[1]) < 0.01 and math.abs(zf[3] - at[3]) < 0.01, true)
    cam:SnapTo(at, nil, 300)
    UIZoomTo({cdr}, 0.5)
    WaitSeconds(0.1)
    local midway = cam:GetZoom()
    WaitSeconds(0.7)
    check("with seconds the camera takes that time", midway > 60 and midway < 300 and string.format("%.1f", cam:GetZoom()) == "40.0", true)
    SelectUnits(nil)
    cam:SnapTo(at, nil, 300)
    UISelectAndZoomTo(cdr, 0)
    sel = GetSelectedUnits()
    check("UISelectAndZoomTo selects the unit", sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr, true)
    check("and shows it from near", cam:GetZoom() > 40 and cam:GetZoom() < 50, true)
    -- the console commands the selection keys are bound to
    check("Ctrl-B is bound to the command", moho64ui_KeyMapAction("Ctrl-B"), "UI_SelectByCategory ENGINEER")
    SelectUnits(nil)
    ConExecute("UI_SelectByCategory COMMAND")
    check("UI_SelectByCategory selects by category", count(), 1)
    ConExecute("UI_SelectByCategory +excludeengineers LAND MOBILE")
    check("+excludeengineers leaves the commander out", count(), 0)
    ConExecute("UI_SelectByCategory LAND MOBILE")
    check("words side by side narrow the category", count(), 1)
    SelectUnits(nil)
    cam:SnapTo(at, nil, 300)
    ConExecute("UI_SelectByCategory +nearest +goto COMMAND")
    check("+nearest +goto selects it and the camera goes there", count() == 1 and cam:GetZoom() > 40 and cam:GetZoom() < 50, true)
    ConExecute("UI_ExpandCurrentSelection")
    check("UI_ExpandCurrentSelection keeps what is selected", count(), 1)
    SelectUnits(nil)
    cam:Reset()

    -- the numbered groups: the keys' commands pass the name on to the game's selection script
    check("Ctrl-1 makes group 1", moho64ui_KeyMapAction("Ctrl-1"), "UI_MakeSelectionSet 1")
    check("1 calls it back", moho64ui_KeyMapAction("1"), "UI_ApplySelectionSet 1")
    SelectUnits({cdr})
    ConExecute("UI_MakeSelectionSet 1")
    check("UI_MakeSelectionSet puts the selection into the set", cdr:HasSelectionSet("1"), true)
    SelectUnits(nil)
    ConExecute("UI_ApplySelectionSet 1")
    sel = GetSelectedUnits()
    check("UI_ApplySelectionSet selects the set", sel ~= nil and table.getn(sel) == 1 and sel[1] == cdr, true)
    ConExecute("UI_ApplySelectionSet 2")
    check("an empty set leaves the selection", count(), 1)
    SelectUnits(nil)
    ConExecute("UI_MakeSelectionSet 1")
    check("made from nothing the set is empty again", cdr:HasSelectionSet("1"), false)
    cam:Reset()

    -- range rings: the game's script has named its filters and switched the three cases on; a
    -- selected unit gets the rings of the filters it is in, measured from its blueprint
    local filters = import("/lua/ui/game/rangeoverlayparams.lua").RangeOverlayParams
    local bp = __blueprints[cdr:GetUnitId()]
    local reach = 0
    for _, w in bp.Weapon do
        if w.RangeCategory == 'UWRC_DirectFire' and w.MaxRadius > reach then reach = w.MaxRadius end
    end
    local function ring(radius, color)
        for _, r in moho64ui_RangeRings() do
            if math.abs(r.radius - radius) < 0.01 and r.color == string.lower(string.sub(color, 3)) then return true end
        end
        return false
    end
    moho64ui_MouseMove(3, 3)
    check("nothing selected, nothing under the mouse: no rings", table.getn(moho64ui_RangeRings()), 0)
    SelectUnits({cdr})
    check("the commander has a direct fire weapon", reach > 0, true)
    check("selected: its direct fire ring, in the filter's select colour", ring(reach, filters.DirectFire.SelectColor), true)
    check("and its omni ring", ring(bp.Intel.OmniRadius, filters.Omni.SelectColor), true)
    local at2 = cdr:GetPosition()
    local first = moho64ui_RangeRings()[1]
    check("the rings lie round the unit", math.abs(first.x - at2[1]) < 0.01 and math.abs(first.z - at2[3]) < 0.01, true)
    ConExecute("range_RenderSelected false")
    check("range_RenderSelected false: none", table.getn(moho64ui_RangeRings()), 0)
    SelectUnits(nil)
    SetOverlayFilters({"DirectFire"})
    check("a filter switched on: every unit of the player's in it, normal colour", ring(reach, filters.DirectFire.NormalColor), true)
    check("and only that filter's", ring(bp.Intel.OmniRadius, filters.Omni.NormalColor), false)
    SetOverlayFilters({})
    check("switched off again: none", table.getn(moho64ui_RangeRings()), 0)
    ConExecute("range_RenderSelected true")

    -- pause: asked for like an order, so it holds from a beat on; the game's time stands
    check("the game runs", SessionIsPaused(), false)
    local gamemain = import("/lua/ui/game/gamemain.lua")
    local oldOnPause, heardPause = gamemain.OnPause, false
    gamemain.OnPause = function(by, left)
        heardPause = true
        oldOnPause(by, left)
    end
    SessionRequestPause()
    WaitSeconds(0.5)
    check("SessionRequestPause pauses it", SessionIsPaused(), true)
    local stood = GetGameTimeSeconds()
    WaitSeconds(0.5)
    check("the game's time stands while paused", GetGameTimeSeconds(), stood)
    check("the game's script has heard of the pause", heardPause, true)
    gamemain.OnPause = oldOnPause
    SessionResume()
    WaitSeconds(0.5)
    check("SessionResume lets it go on", SessionIsPaused() == false and GetGameTimeSeconds() > stood, true)
    -- the game's speed, in the speed keys' steps: only where the game's option lets it be changed
    local mayChange = SessionGetScenarioInfo().Options.GameSpeed == "adjustable"
    check("the speed at the start", GetGameSpeed(), 0)
    SetGameSpeed(10)
    check("SetGameSpeed (only with GameSpeed = adjustable)", GetGameSpeed(), mayChange and 10 or 0)
    local t0 = GetGameTimeSeconds()
    WaitSeconds(1)
    local ran = GetGameTimeSeconds() - t0
    check("ten steps up run the game ten times as fast", mayChange and (ran > 8 and ran < 11) or (ran > 0.8 and ran < 1.2), true)
    ConExecute("WLD_ResetSimRate")
    check("WLD_ResetSimRate", GetGameSpeed(), 0)
    ConExecute("WLD_IncreaseSimRate")
    check("WLD_IncreaseSimRate", GetGameSpeed(), mayChange and 1 or 0)
    ConExecute("WLD_DecreaseSimRate")
    ConExecute("WLD_DecreaseSimRate")
    check("WLD_DecreaseSimRate", GetGameSpeed(), mayChange and -1 or 0)
    ConExecute("WLD_GameSpeed 99")
    check("WLD_GameSpeed keeps to at most 50", GetGameSpeed(), mayChange and 50 or 0)
    ConExecute("WLD_GameSpeed -99")
    check("and at least -10", GetGameSpeed(), mayChange and -10 or 0)
    ConExecute("WLD_GameSpeed 0")
    check("back to normal", GetGameSpeed(), 0)

    -- the life bars over units: off until the player switches them on (ours); the scripts' own
    -- "false" / "true" put them away and give back what the player chose
    local barsAtFirst = moho64ui_LifeBars()
    if barsAtFirst then ConExecute('UI_RenderUnitBars') end
    check("life bars are off (the game's script has run UI_RenderUnitBars true)", moho64ui_LifeBars(), false)
    check("the key for them is bound", moho64ui_KeyMapAction("Alt-L"), "UI_RenderUnitBars")
    ConExecute('UI_RenderUnitBars')
    check("the player's switch shows them", moho64ui_LifeBars(), true)
    ConExecute('UI_RenderUnitBars false')
    check("a script puts them away", moho64ui_LifeBars(), false)
    ConExecute('UI_RenderUnitBars true')
    check("and gives them back", moho64ui_LifeBars(), true)
    ConExecute('UI_RenderUnitBars')
    ConExecute('UI_RenderUnitBars true')
    check("switched off they stay off whatever a script says", moho64ui_LifeBars(), false)
    if barsAtFirst then ConExecute('UI_RenderUnitBars') end
    LOG("TEST done: " .. failed .. " failed")
end

-- (An error inside stops the thread and is logged; the last line is then missing.)
ForkThread(run)
