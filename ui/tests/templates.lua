-- Build templates: selected buildings become a template, the template is handed to the script
-- and taken back, and placed it orders every building of it where it stood beside the first.
-- With them the small functions around a game that have no test of their own.
-- Run it like victory.lua (a launched game whose options have CheatsEnabled = "true": the
-- buildings are made by the game's own cheat callback), with --frames 6900.
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
    WaitSeconds(8)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local x, z = math.floor(c[1]), math.floor(c[3])

    check("no template at first: an empty table", next(GetActiveBuildTemplate()), nil)
    SelectUnits({cdr})
    GenerateBuildTemplateFromSelection()
    check("a selection without buildings makes none", next(GetActiveBuildTemplate()), nil)

    -- three power generators (two by two each): one, one east of it, one north of it
    local function spawn(dx, dz)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {x + dx, c[2], z + dz}, bpId = "ueb1101", count = 1, yaw = 0}})
        WaitSeconds(0.5)
    end
    spawn(-20, 8)
    spawn(-18, 8)
    spawn(-20, 6)
    WaitSeconds(1)
    UISelectionByCategory("STRUCTURE", false, false, false, false)
    check("the three buildings are selected", table.getn(GetSelectedUnits() or {}), 3)
    GenerateBuildTemplateFromSelection()
    local t = GetActiveBuildTemplate()
    check("the template has its size and three buildings", table.getn(t), 5)
    check("as wide as two generators", t[1], 4)
    check("and as deep", t[2], 4)
    check("the first building is the one made first, at the template's own place", t[3][1] .. " " .. t[3][3] .. " " .. t[3][4], "ueb1101 0 0")
    check("the second stands two east of it", t[4][3] .. " " .. t[4][4], "2 0")
    check("the third two north of it", t[5][3] .. " " .. t[5][4], "0 -2")
    check("in the order they were made", t[3][2] < t[4][2] and t[4][2] < t[5][2], true)
    ClearBuildTemplates()
    check("cleared: an empty table again", next(GetActiveBuildTemplate()), nil)

    -- placed: as the game's script does it (build_templates.lua shifts the places by half a cell
    -- when the first building's footprint is even, then the construction panel starts the mode).
    -- This generator's footprint is one cell (its skirt is two), so nothing is shifted and it
    -- stands on the middle of the cell under the mouse.
    local bp = __blueprints[t[3][1]]
    local function shift(axis)
        return math.mod(math.ceil(bp.Footprint and bp.Footprint[axis] or bp[axis] or 1), 2) == 1 and 0 or 0.5
    end
    local sx, sz = shift("SizeX"), shift("SizeZ")
    for i = 3, table.getn(t) do
        t[i][3] = t[i][3] + sx
        t[i][4] = t[i][4] + sz
    end
    UIZoomTo({cdr}, 0)
    WaitSeconds(1)
    for i = 1, 2 do moho64ui_MouseWheel(800, 300, -1) WaitSeconds(0.1) end
    WaitSeconds(1.5)
    SelectUnits({cdr})
    CM.StartCommandMode("build", {name = t[3][1]})
    SetActiveBuildTemplate(t)
    check("the script's template is the active one", table.getn(GetActiveBuildTemplate()), 5)
    local s = view:Project({x + 16.7, c[2], z - 11.3})  -- (somewhere inside the cell)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    WaitSeconds(0.8)
    local sites = {}
    for _, o in cdr:GetCommandQueue() do
        if o.type == "BuildMobile" then table.insert(sites, o.position) end
    end
    check("one click orders the three buildings", table.getn(sites), 3)
    if table.getn(sites) == 3 then
        check("the first on the grid where the mouse was", sites[1][1] .. " " .. sites[1][3], (x + 16.5 + sx) .. " " .. (z - 11.5 + sz))
        check("the second two east of it", (sites[2][1] - sites[1][1]) .. " " .. (sites[2][3] - sites[1][3]), "2 0")
        check("the third two north of it", (sites[3][1] - sites[1][1]) .. " " .. (sites[3][3] - sites[1][3]), "0 -2")
    end
    CM.EndCommandMode(true)
    check("ending the build mode clears the template (the script's call)", next(GetActiveBuildTemplate()), nil)

    -- the small ones
    check("the game has voices in English", HasLocalizedVO("us"), true)
    check("and none in a language it does not have", HasLocalizedVO("xx"), false)
    local handle = AddConsoleOutputReciever(function(text) end)
    check("a console output receiver gets a handle", handle ~= nil, true)
    RemoveConsoleOutputReciever(handle)
    local pgen = GetSelectedUnits()[1]
    AddToSessionExtraSelectList(pgen)
    RemoveFromSessionExtraSelectList(pgen)
    ClearSessionExtraSelectList()
    FlushEvents()
    check("the extra selection list and FlushEvents are there", true, true)

    -- A factory's infinite build: the construction panel's button tells the simulation with
    -- unit:ProcessInfo('SetRepeatQueue', ...) and reads it back with IsRepeatQueue; it must
    -- stay on when a unit is then put in the queue (the panel is laid out anew by that).
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {x - 30, c[2], z - 4}, bpId = "ueb0101", count = 1, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("FACTORY", false, false, false, false)
    local factory = (GetSelectedUnits() or {})[1]
    check("a factory stands and is selected", factory ~= nil and factory:GetBlueprint().BlueprintId, "ueb0101")
    if factory then
        local button = import("/lua/ui/game/construction.lua").controls.extraBtn1
        check("its queue does not repeat at first", factory:IsRepeatQueue(), false)
        check("and the panel's infinite-build button is off", button:IsChecked(), false)
        button:SetCheck(true)  -- (as a click on it: the button's OnCheck tells the simulation)
        WaitSeconds(0.5)
        check("the button's switch reaches the simulation", factory:IsRepeatQueue(), true)
        IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0201", 1)
        WaitSeconds(0.8)
        check("a unit is queued", table.getn(factory:GetCommandQueue() or {}) > 0, true)
        check("and the switch is still on", factory:IsRepeatQueue(), true)
        SelectUnits({cdr})
        WaitSeconds(0.3)
        SelectUnits({factory})  -- (the panel is laid out anew and reads the switch back)
        WaitSeconds(0.3)
        button = import("/lua/ui/game/construction.lua").controls.extraBtn1
        check("the button still shows it after the panel is laid out anew", button:IsChecked(), true)
        -- and the factory does build its queue again: a second tank comes with one ordered
        local tanks, waited = 0, 0
        while tanks < 2 and waited < 60 do
            WaitSeconds(1)
            waited = waited + 1
            UISelectionByCategory("uel0201", false, false, false, false)
            tanks = table.getn(GetSelectedUnits() or {})
        end
        check("with one tank ordered a second one is built", tanks >= 2, true)
        check("and the order is still in the queue", table.getn(factory:GetCommandQueue() or {}), 1)
        SelectUnits({factory})
        factory:ProcessInfo("SetRepeatQueue", "false")
        WaitSeconds(0.5)
        check("and it goes off again", factory:IsRepeatQueue(), false)
    end

    -- The camera shake (the simulation's Entity:ShakeCamera calls): a Monkeylord of ours
    -- destroyed with Ctrl+K near what the camera looks at shakes it; with the game's option
    -- "Shake intensity" at 0 (the console's cam_ShakeMult 0) the same blast moves nothing.
    local function blast(mult)
        ConExecute("cam_ShakeMult " .. mult)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {x + 25, c[2], z - 10}, bpId = "url0402", count = 1, yaw = 0}})
        WaitSeconds(1.5)
        UISelectionByCategory("url0402", false, false, false, false)
        local ml = (GetSelectedUnits() or {})[1]
        if not ml then return nil end
        local cam = GetCamera("WorldCamera")
        local s = cam:SaveSettings()
        s.Focus = ml:GetPosition()
        s.Zoom = 80
        cam:RestoreSettings(s)
        moho64ui_Key("k", "Ctrl")
        local most, shook = 0, nil
        for i = 1, 48 do
            WaitSeconds(0.25)
            local now = moho64ui_CameraShake()
            if now.offset > most then most = now.offset end
            if now.time < now.seconds then shook = now end
        end
        return most, shook, ml:IsDead()
    end
    local most, shook, dead = blast(1)
    check("a Monkeylord destroyed near the camera", dead, true)
    check("shakes the camera as its blueprint says (5 at the middle, reaching 50)", shook and (shook.max .. " " .. shook.radius), "5 50")
    check("and the view moves", (most or 0) > 0.1, true)
    most, shook, dead = blast(0)
    check("with Shake intensity at 0 the next blast is still there", shook ~= nil, true)
    check("but the view does not move", most, 0)
    ConExecute("cam_ShakeMult 1")
    LOG("TEST done: " .. failed .. " failed")
end)
