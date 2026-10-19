-- Pings and markers, the game's own (FAF's /lua/ui/game/ping.lua and what it calls, run unchanged): each kind sent the
-- player's way, its key (F5 alert, F6 move, F7 attack, F8 marker: FAF's key map) or its button in the multifunction
-- panel (the script's command mode "ping": a left click in the world ends it not cancelled, FA exe action kind 6, and
-- a right click as well, kind 7, so either pings at the mouse); through the simulation (SimCallback SpawnPing,
-- SimPing.lua) and back with the Sync (UserSync.lua -> ping.DisplayPing); shown in the main world view and the minimap
-- (the script's rings, which the main view hides while zoomed in nearer than 40% of the camera's range, and its arrow;
-- the ping's mesh in the world, an entity of the simulation's scripts as moho64ui_ScriptEntities reports what the
-- picture is given), with its sound; gone after its time. A ping from over the minimap goes to the minimap's place
-- (GetMouseWorldPos: the view that last heard the mouse). A right click in an order mode ends the mode and gives no
-- order. A marker: placed with a name typed into its dialog, shown with its name while Shift is held, renamed
-- (ping.UpdateMarker, FAF's way), moved by dragging it, deleted with Ctrl and a right click on it; in the main view only
-- (FAF's minimap.lua registers the minimap with its markers off; the marker's rings show there). At the end a ping and
-- a marker stand in view with Shift held, the run ending while the ping lasts (for --screenshot). Run it like
-- orderlines.lua (a launched game, session_ai_cheats.lua, seed 7, with the world's picture), with --frames 2000.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function xyz(v) return v and string.format("%.1f,%.1f,%.1f", v[1], v[2], v[3]) or "nil" end
local function near(a, b, within)
    return a ~= nil and b ~= nil and math.abs(a[1] - b[1]) <= within and math.abs(a[3] - b[3]) <= within
end

-- What the Sync hands the interface (UserSync.lua calls ping.DisplayPing with Sync.Ping): kept here, then passed on.
local Ping = import("/lua/ui/game/ping.lua")
local synced = {}
local display = Ping.DisplayPing
Ping.DisplayPing = function(data)
    for _, p in data do table.insert(synced, p) end
    return display(data)
end
local function lastSynced(kind)
    for i = table.getn(synced), 1, -1 do
        if synced[i].Type == kind and not synced[i].Action then return synced[i] end
    end
end

local view, mini
-- the rings a ping made in a view (FAF's PingRing: a Bitmap of the view with the ping's data), and its arrow (the
-- view's CreateCameraIndicator: an off-screen indicator with the ping's place as its target)
local function rings(v, ping)
    local found = {}
    v:ApplyFunction(function(c)
        if c ~= v and c.data == ping and c.Time ~= nil then table.insert(found, c) end
    end)
    return found
end
local function arrows(v, ping)
    local found = {}
    v:ApplyFunction(function(c)
        if c ~= v and c.Target == ping.Location and c.WorldView == v then table.insert(found, c) end
    end)
    return found
end
-- the ring's middle against where the view puts the place (Project: from the view's top left corner)
local function ringAt(v, ring, place)
    local p = v:Project(Vector(place[1], place[2], place[3]))
    local dx = ring.Left() + ring.Width() / 2 - (v.Left() + p.x)
    local dy = ring.Top() + ring.Height() / 2 - (v.Top() + p.y)
    return math.abs(dx) <= 2 and math.abs(dy) <= 2
end
-- the simulation's script entity with a ping's mesh near a place (the mesh is 2 above the place clicked)
local function meshAt(mesh, place)
    for _, e in moho64ui_ScriptEntities() or {} do
        if e.mesh == "/meshes/game/ping_" .. mesh and math.abs(e.x - place[1]) < 0.5 and math.abs(e.z - place[3]) < 0.5 then return e end
    end
end
local function cues() return moho64ui_SoundInfo().played end
local function point(v, place)
    local p = v:Project(Vector(place[1], place[2], place[3]))
    return v.Left() + p.x, v.Top() + p.y
end
local function mouseAt(place)
    local x, y = point(view, place)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.1)
    return x, y
end
-- a ping just arrived from the simulation: its data, place, ring texture, sound, mesh and rings
local function arrived(kind, place, ring, mesh, before, what)
    local p = lastSynced(kind)
    check(what .. ": the simulation hands it back through the Sync", p ~= nil, true)
    if not p then return end
    check(what .. ": from the player's army (0-based)", p.Owner, Ping.OriginalFocusArmy - 1)
    check(what .. ": at the place", near(p.Location, place, 1), true)
    check(what .. ": its ring", p.Ring, ring)
    check(what .. ": its sound played", cues() - before, 1)
    LOG("TEST note: " .. what .. " at " .. xyz(p.Location) .. ", its sound's wave " .. moho64ui_SoundInfo().wave)
    if mesh then
        local e = meshAt(mesh, p.Location)
        check(what .. ": its mesh stands in the world, 2 above the place", e ~= nil and math.abs(e.y - p.Location[2]) < 0.01, true)
        check(what .. ": seen by the player and allies, not by enemies",
            e and (e.focus .. " " .. e.allies .. " " .. e.enemies .. " " .. e.neutrals), "Always Always Never Never")
        -- (the simulation takes a number Owner as the 0-based army, as the original's _c_CreateEntity 0x692330:
        -- engine 61536c5, ui REQUESTS 43; the hook counts armies from 1)
        check(what .. ": its mesh is the player's army's", e and e.army, GetFocusArmy())
    end
    check(what .. ": a ring in the main view", table.getn(rings(view, p)) >= 1, true)
    check(what .. ": a ring on the minimap", table.getn(rings(mini, p)) >= 1, true)
    return p
end

ForkThread(function()
    WaitSeconds(6)
    local start = CurrentTime()
    local WV = import("/lua/ui/game/worldview.lua")
    view = WV.viewLeft
    local minimap = import("/lua/ui/game/minimap.lua")
    if minimap.controls.displayGroup:IsHidden() then minimap.ToggleMinimap() end
    -- (the window small and out of the way, at the right: the preferences' copy has it over most of the screen)
    local win = minimap.controls.displayGroup
    win.Left:Set(1300)
    win.Top:Set(150)
    win.Right:Set(1600)
    win.Bottom:Set(450)
    mini = minimap.controls.miniMap
    WaitSeconds(0.3)
    LOG(string.format("TEST note: the main view %d %d %d %d, the minimap %d %d %d %d", view.Left(), view.Top(), view.Right(), view.Bottom(),
        mini.Left(), mini.Top(), mini.Right(), mini.Bottom()))
    for _, id in {"alert", "move", "attack", "marker"} do
        local b = import("/lua/ui/game/multifunction.lua").GetButton(id)
        if b then LOG(string.format("TEST note: the %s button %d %d %d %d", id, b.Left(), b.Top(), b.Right(), b.Bottom())) end
    end
    local views = 0
    for _, v in WV.GetWorldViews() do views = views + 1 end
    check("pings go to two world views: the main one and the minimap", views, 2)
    check("the player's original army is known to the ping script", Ping.OriginalFocusArmy, GetFocusArmy())
    check("the simulation told the most markers an army may have", Ping.MaxMarkers, 15)
    local cdr = GetArmyAvatars()[1]
    local home = cdr:GetPosition()
    local function spot(dx, dz) return {home[1] + dx, home[2], home[3] + dz} end
    local cam = GetCamera("WorldCamera")
    cam:SnapTo(spot(20, 10), nil, 110)
    WaitSeconds(0.5)
    SelectUnits(nil)

    -- 1. the alert ping by its key, F5, at the mouse; the main view zoomed in: its rings hidden, the mesh shows
    local a = spot(20, 10)
    mouseAt(a)
    check("GetMouseWorldPos: the place under the mouse", near(GetMouseWorldPos(), a, 1), true)
    local before = cues()
    moho64ui_Key(346)
    WaitSeconds(0.4)
    local alert = arrived("Alert", a, "/game/marker/ring_yellow02-blur.dds", "alert_marker", before, "F5, an alert")
    if alert then
        local r = rings(view, alert)[1]
        check("zoomed in, the main view's ring is hidden (FAF's ping script)", r and r:IsHidden(), true)
        local m = rings(mini, alert)[1]
        check("the minimap's ring shows", m and m:IsHidden(), false)
        check("the minimap's ring is centred on the place", m and ringAt(mini, m, alert.Location), true)
        check("its texture", m and moho64ui_Describe(m), "bitmap /textures/ui/common/game/marker/ring_yellow02-blur.dds")
        check("and it fades as it grows", m and m:GetAlpha() < 1 and m:GetAlpha() > 0 and m.Height() > 0, true)
        check("the main view's arrow to it", table.getn(arrows(view, alert)), 1)
        local e1 = meshAt("alert_marker", alert.Location)
        WaitSeconds(0.35)
        local e2 = meshAt("alert_marker", alert.Location)
        check("its mesh grows and shrinks (SimPing.lua AnimatePingMesh)", e1 ~= nil and e2 ~= nil and e1.scale ~= e2.scale
            and e1.scale >= 0.19 and e1.scale <= 0.61 and e2.scale >= 0.19 and e2.scale <= 0.61, true)
    end
    WaitSeconds(0.6)

    -- 2. the attack ping by its key, F7, with the main view zoomed out past 40% of its range: its rings show there
    local b = spot(-25, 15)
    cam:SnapTo(b, nil, cam:GetMinZoom() + (cam:GetMaxZoom() - cam:GetMinZoom()) * 0.6)
    WaitSeconds(0.6)
    mouseAt(b)
    before = cues()
    moho64ui_Key(348)
    WaitSeconds(0.45)
    local attack = arrived("Attack", b, "/game/marker/ring_red02-blur.dds", "attack_marker", before, "F7, an attack")
    if attack then
        local r = rings(view, attack)[1]
        check("zoomed out, the main view's ring shows", r and r:IsHidden(), false)
        check("centred on the place", r and ringAt(view, r, attack.Location), true)
        check("its texture: red", r and moho64ui_Describe(r), "bitmap /textures/ui/common/game/marker/ring_red02-blur.dds")
        -- (the youngest of its rings, a few frames apart: each lives a second, growing to a quarter of the view's
        -- height and fading)
        for _, k in rings(view, attack) do
            if k.Time < r.Time then r = k end
        end
        local h1, a1 = r.Height(), r:GetAlpha()
        WaitFrames(6)
        check("it grows and fades over its second", not IsDestroyed(r) and r.Height() > h1 and r:GetAlpha() < a1, true)
    end
    cam:SnapTo(a, nil, 110)
    WaitSeconds(0.8)

    -- 3. the move ping by its button in the multifunction panel, then a left click in the world
    local MF = import("/lua/ui/game/multifunction.lua")
    local CM = import("/lua/ui/game/commandmode.lua")
    local btn = MF.GetButton("move")
    check("the move ping's button is there", btn ~= nil, true)
    local c = spot(5, -20)
    if btn then
        check("and can be pressed", btn:IsDisabled(), false)
        local bx, by = (btn.Left() + btn.Right()) / 2, (btn.Top() + btn.Bottom()) / 2
        moho64ui_MouseMove(bx, by)
        WaitSeconds(0.1)
        moho64ui_MouseButton("Left", true, bx, by)
        moho64ui_MouseButton("Left", false, bx, by)
        WaitSeconds(0.1)
        local mode = CM.GetCommandMode()
        check("its click starts the script's mode \"ping\"", tostring(mode[1]) .. " " .. tostring(mode[2] and mode[2].pingtype), "ping move")
        local x, y = mouseAt(c)
        before = cues()
        moho64ui_MouseButton("Left", true, x, y)
        check("a left press in the world ends the mode", CM.GetCommandMode()[1], false)
        moho64ui_MouseButton("Left", false, x, y)
        WaitSeconds(0.45)
        arrived("Move", c, "/game/marker/ring_blue02-blur.dds", "move", before, "the Move button and a left click, a move")
    end
    WaitSeconds(0.7)
    -- and by its key, F6
    local c2 = spot(-5, 25)
    mouseAt(c2)
    before = cues()
    moho64ui_Key(347)
    WaitSeconds(0.45)
    arrived("Move", c2, "/game/marker/ring_blue02-blur.dds", "move", before, "F6, a move")
    WaitSeconds(0.7)

    -- 4. the alert ping by its button, then a right click: the mode ends when the button is let go, not cancelled
    local d = spot(-10, -25)
    btn = MF.GetButton("alert")
    if btn then
        local bx, by = (btn.Left() + btn.Right()) / 2, (btn.Top() + btn.Bottom()) / 2
        moho64ui_MouseMove(bx, by)
        WaitSeconds(0.1)
        moho64ui_MouseButton("Left", true, bx, by)
        moho64ui_MouseButton("Left", false, bx, by)
        WaitSeconds(0.1)
        check("the Alert button starts the mode \"ping\"", tostring(CM.GetCommandMode()[1]) .. " " .. tostring(CM.GetCommandMode()[2].pingtype), "ping alert")
        local x, y = mouseAt(d)
        local count = table.getn(synced)
        moho64ui_MouseButton("Right", true, x, y)
        check("a right press keeps the mode", CM.GetCommandMode()[1], "ping")
        moho64ui_MouseButton("Right", false, x, y)
        check("letting the right button go ends it", CM.GetCommandMode()[1], false)
        WaitSeconds(0.45)
        local p = synced[count + 1]
        check("and pings there (the mode ended, not cancelled)", p ~= nil and p.Type == "Alert" and near(p.Location, d, 1), true)
    end
    WaitSeconds(0.7)

    -- 5. F5 with the mouse over the minimap: the ping goes to the minimap's place under the mouse
    local mx, my = mini.Left() + (mini.Right() - mini.Left()) * 0.3, mini.Top() + (mini.Bottom() - mini.Top()) * 0.6
    moho64ui_MouseMove(mx, my)
    WaitSeconds(0.1)
    local onMini = UnProject(mini, Vector2(mx, my))
    local behind = UnProject(view, Vector2(mx, my))
    check("GetMouseWorldPos over the minimap: the minimap's place", near(GetMouseWorldPos(), onMini, 1), true)
    local count = table.getn(synced)
    moho64ui_Key(346)
    WaitSeconds(0.45)
    local p = synced[count + 1]
    check("F5 over the minimap pings at its place", p ~= nil and near(p.Location, onMini, 1), true)
    check("(not at the main view's ground behind it)", p ~= nil and not near(p.Location, behind, 20), true)
    LOG("TEST note: over the minimap at " .. xyz(onMini) .. ", the main view's ground there " .. xyz(behind))

    -- 6. a right click in an order mode ends the mode and orders nothing (kind 7, as for the pings)
    SelectUnits({cdr})
    IssueCommand("UNITCOMMAND_Stop")
    WaitSeconds(0.3)
    CM.StartCommandMode("order", {name = "RULEUCC_Attack"})
    local x, y = mouseAt(spot(12, 0))
    moho64ui_MouseButton("Right", true, x, y)
    moho64ui_MouseButton("Right", false, x, y)
    check("a right click in the Attack order mode ends it", CM.GetCommandMode()[1], false)
    WaitSeconds(0.4)
    check("and gives no order", table.getn(cdr:GetCommandQueue() or {}), 0)
    SelectUnits(nil)

    -- 7. gone after its time: the alert's rings and arrow (its six seconds of rings, then the last one's second) and
    -- its mesh (six seconds, SimPing.lua)
    while CurrentTime() - start < 17 do WaitSeconds(0.2) end
    if alert then
        check("the alert's rings are gone from the main view", table.getn(rings(view, alert)), 0)
        check("and from the minimap", table.getn(rings(mini, alert)), 0)
        check("its arrow is gone", table.getn(arrows(view, alert)), 0)
        check("its mesh is gone from the world", meshAt("alert_marker", alert.Location), nil)
    end
    if attack then check("the attack's rings are gone", table.getn(rings(view, attack)) + table.getn(rings(mini, attack)), 0) end

    -- 8. a marker: F8 opens the marker's dialog, a name typed and Enter
    local e = spot(15, -8)
    mouseAt(e)
    before = cues()
    moho64ui_Key(349)
    WaitSeconds(0.2)
    moho64ui_Type("Base")
    moho64ui_Key(13)
    WaitSeconds(0.5)
    local marker = lastSynced("Marker")
    local owner = Ping.OriginalFocusArmy - 1
    check("F8, a name and Enter: the marker comes back through the Sync", marker ~= nil, true)
    local id = marker and marker.ID
    local group = id and view.Markers and view.Markers[owner] and view.Markers[owner][id]
    local miniGroup = id and mini.Markers and mini.Markers[owner] and mini.Markers[owner][id]
    if marker then
        check("with its name", marker.Name, "Base")
        check("its number from the simulation", id, 1)
        check("in the army's colour", marker.Color, GetArmiesTable().armiesTable[GetFocusArmy()].color)
        check("at the place", near(marker.Location, e, 1), true)
        check("its sound", cues() - before, 1)
        LOG("TEST note: the marker's sound's wave " .. moho64ui_SoundInfo().wave)
        check("no mesh for a marker", meshAt("marker", marker.Location), nil)
        check("the main view holds it with its name", group and group.Name:GetText(), "Base")
        check("the minimap holds no marker (FAF's minimap.lua registers it with its markers off)", miniGroup, nil)
        check("but shows the marker's rings", table.getn(rings(mini, marker)) >= 1, true)
        check("its icon", group and moho64ui_Describe(group.Marker), "bitmap /textures/ui/common/game/ping_marker/ping_marker-01.dds")
        check("the icon shows, its name only while Shift is held", group and (tostring(group.Marker:IsHidden()) .. " " .. tostring(group:IsHidden())), "false true")
    end
    local function shiftKey(down)
        if down then moho64ui_Key(306, "Shift", "down") else moho64ui_Key(306, "", "up") end
        WaitSeconds(0.2)
    end
    local function markerPoint(g)
        return (g.Marker.Left() + g.Marker.Right()) / 2, (g.Marker.Top() + g.Marker.Bottom()) / 2
    end
    if group then
        local gx, gy = markerPoint(group)
        local px, py = point(view, marker.Location)
        check("the icon stands at the place", math.abs(gx - px) <= 2 and math.abs(gy - py) <= 2, true)
        shiftKey(true)
        check("Shift held: its name shows and the icon can be clicked",
            tostring(group:IsHidden()) .. " " .. tostring(group.Marker:IsHitTestDisabled()), "false false")
        shiftKey(false)
        check("Shift let go: the name hides again", group:IsHidden(), true)

        -- 9. renamed, FAF's way (ping.UpdateMarker: the simulation renames it and tells the views)
        Ping.UpdateMarker({Action = "rename", ID = id, Owner = owner, Name = "Front"})
        WaitSeconds(0.5)
        check("renamed: the main view's name", group.Name:GetText(), "Front")

        -- 10. moved: Shift held, the icon dragged with the left button
        shiftKey(true)
        gx, gy = markerPoint(group)
        moho64ui_MouseMove(gx, gy)
        WaitSeconds(0.1)
        moho64ui_MouseButton("Left", true, gx, gy, "Shift")
        for k = 1, 4 do
            moho64ui_MouseMove(gx + 30 * k, gy + 15 * k)
            WaitSeconds(0.05)
        end
        moho64ui_MouseButton("Left", false, gx + 120, gy + 60, "Shift")
        local dropped = UnProject(view, Vector2(gx + 120, gy + 60))
        WaitSeconds(0.6)
        check("dragged: the simulation moves it and the views take its new place", near(group.data.Location, dropped, 1), true)
        gx, gy = markerPoint(group)
        local nx, ny = point(view, dropped)
        check("the icon stands at the new place", math.abs(gx - nx) <= 2 and math.abs(gy - ny) <= 2, true)

        -- 11. deleted: Ctrl and a right click on the icon (Shift held, so that it can be clicked)
        moho64ui_MouseMove(gx, gy)
        WaitSeconds(0.1)
        moho64ui_MouseButton("Right", true, gx, gy, "Shift Ctrl")
        moho64ui_MouseButton("Right", false, gx, gy, "Shift Ctrl")
        WaitSeconds(0.5)
        check("deleted: gone from the main view", view.Markers[owner][id], nil)
        check("its controls are destroyed", IsDestroyed(group) and IsDestroyed(group.Marker), true)
        shiftKey(false)
    end

    -- 12. for the picture: a marker (by its button and a left click this time) and an alert ping side by side in view,
    -- Shift held
    local f, g = spot(28, 4), spot(36, 14)
    cam:SnapTo(spot(32, 9), nil, 70)
    WaitSeconds(0.6)
    btn = MF.GetButton("marker")
    if btn then
        local bx, by = (btn.Left() + btn.Right()) / 2, (btn.Top() + btn.Bottom()) / 2
        moho64ui_MouseMove(bx, by)
        WaitSeconds(0.1)
        moho64ui_MouseButton("Left", true, bx, by)
        moho64ui_MouseButton("Left", false, bx, by)
        WaitSeconds(0.1)
        check("the Marker button starts the mode \"ping\"", tostring(CM.GetCommandMode()[1]) .. " " .. tostring(CM.GetCommandMode()[2].pingtype), "ping marker")
    end
    local fx, fy = mouseAt(f)
    moho64ui_MouseButton("Left", true, fx, fy)
    moho64ui_MouseButton("Left", false, fx, fy)
    WaitSeconds(0.2)
    moho64ui_Type("Rally")
    moho64ui_Key(13)
    WaitSeconds(1.2)
    mouseAt(g)
    moho64ui_Key(346)
    WaitSeconds(0.4)
    local rally = lastSynced("Marker")
    check("for the picture: the marker", rally and rally.Name, "Rally")
    check("and the ping", lastSynced("Alert") ~= nil and near(lastSynced("Alert").Location, g, 1), true)
    moho64ui_MouseMove(view.Left() + 80, view.Top() + 300)
    moho64ui_Key(306, "Shift", "down")
    LOG(string.format("TEST note: done at %.1f s of the run's clock (%d frames)", CurrentTime(), CurrentTime() * 60))
    LOG("TEST done: " .. failed .. " failed")
end)
