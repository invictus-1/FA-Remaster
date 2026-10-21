-- What Chris's tour log of 2026-10-19 showed, as the original does it:
-- - FAF's notify overlay (notifyoverlay.lua createEnhancementOverlay: a bitmap on the root frame with only a size,
--   laid out in its first OnFrame), made when the commander starts an enhancement, had its Left and Top read before
--   that OnFrame: "Evaluating LazyVar failed ... circular dependency [of the control bitmap < frame]" twice each time.
--   The reader was the unit panel's OnFrame asking GetRolloverInfo, which here tested every control against the mouse;
--   the original reads what the world view the mouse is over found in its frame update (worldview.cpp MouseInView), its
--   controls' frames come after the script threads and the picture (UserState::UpdateControls), and its draw does not
--   look at a bitmap with nothing to show (bitmap.cpp Draw).
-- - The order table commandmode.OnCommandIssued hears (FA exe 0x83e150): an order without a target, as the
--   enhancement's Script order, has Target.Type "None" and the place 0, 0, 0 (so FAF's default blips there, the
--   original's too), and Blueprint is "" when the order has none.
-- - The console variables gamemain.lua sets at a game's start, kept (console.lua has their words); cam_SetLOD on a
--   camera; cam_Free's turn with Space at any zoom.
-- - The interface's blueprints and the simulation's: the same count and records (moho64ui_BlueprintFacts).
-- - The reclaim cursor's TGA picture is read.
-- Run it like orderlines.lua (a launched game with CheatsEnabled = "true"), with --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
-- a LazyVar's cached value (its [1]): nil while nothing has read it; a read that failed leaves 0 there
local function cached(var) return rawget(var, 1) end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[1]
    local cx, cy = view.Left() + view.Width() / 2, view.Top() + view.Height() / 2

    -- 1. a bitmap with nothing to show is not drawn: its edges are not read (0x780872..0x7808ca)
    local plain = Bitmap(GetFrame(0))
    LayoutHelpers.SetDimensions(plain, 100, 50)
    moho64ui_PixelAt(5, 5)  -- (the head's controls drawn now)
    check("a bitmap on the root frame with only a size: the draw reads neither Left nor Top",
        tostring(cached(plain.Left)) .. " " .. tostring(cached(plain.Top)), "nil nil")
    plain:Destroy()

    -- 2. the mouse over the world view (a mouse event: the view's MouseEnter), then a bitmap made by a thread and laid
    -- out in its first OnFrame, as the notify overlay: nothing reads it before then, though the unit panel asks
    -- GetRolloverInfo each frame
    moho64ui_MouseMove(cx, cy)
    WaitSeconds(0.3)
    check("GetRolloverInfo answers with the mouse over the view", pcall(GetRolloverInfo), true)
    local seen
    local made
    ForkThread(function()
        made = Bitmap(GetFrame(0))
        LayoutHelpers.SetDimensions(made, 100, 50)
        made:SetNeedsFrameUpdate(true)
        made.OnFrame = function(self, delta)
            if not seen then seen = tostring(cached(self.Left)) .. " " .. tostring(cached(self.Top)) end
            LayoutHelpers.AtLeftTopIn(self, GetFrame(0), 100, 100)
        end
    end)
    WaitSeconds(0.5)
    check("a bitmap a thread made is first read by its own OnFrame", seen, "nil nil")
    check("and laid out there", made and math.floor(made.Left() + 0.5) == LayoutHelpers.ScaleNumber(100), true)
    if made then made:Destroy() end

    -- 3. the commander's enhancement, as Chris ordered it: the order table and FAF's notify overlay
    local commandmode = import("/lua/ui/game/commandmode.lua")
    local heard = {}
    local oldIssued = commandmode.OnCommandIssued
    commandmode.OnCommandIssued = function(command)
        table.insert(heard, command)
        return oldIssued(command)
    end
    local notify = import("/lua/ui/notify/notifyoverlay.lua")
    local overlaySeen
    local oldCreate = notify.createEnhancementOverlay
    notify.createEnhancementOverlay = function(args)
        local o = oldCreate(args)
        local frame = o.OnFrame
        o.OnFrame = function(self, delta)
            if not overlaySeen then overlaySeen = tostring(cached(self.Left)) .. " " .. tostring(cached(self.Top)) end
            return frame(self, delta)
        end
        return o
    end
    SelectUnits({cdr})
    WaitSeconds(0.5)
    IssueCommand("UNITCOMMAND_Script", {TaskName = "EnhanceTask", Enhancement = "LeftPod"}, true)
    local c = heard[table.getn(heard)]
    check("the enhancement's order is heard", c and c.CommandType, "Script")
    if c then
        check("its target: None", c.Target.Type, "None")
        check("at 0, 0, 0 (the issue data's start, 0x552550)",
            string.format("%g %g %g", c.Target.Position[1], c.Target.Position[2], c.Target.Position[3]), "0 0 0")
        check("no EntityId", c.Target.EntityId, nil)
        check("Blueprint the empty string (0x83e233)", c.Blueprint, "")
        check("its Lua value", c.LuaParams and c.LuaParams.Enhancement, "LeftPod")
        check("Clear", c.Clear, true)
        check("its units", table.getn(c.Units) == 1 and c.Units[1] == cdr, true)
    end
    -- the overlay comes when the work's per cent comes back through the notify chat
    local waited = 0
    while not overlaySeen and waited < 15 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the notify overlay was made and its first OnFrame came before any read of its layout", overlaySeen, "nil nil")
    IssueCommand("Stop")
    c = heard[table.getn(heard)]
    check("Stop: no target either", c and (c.CommandType .. " " .. c.Target.Type), "Stop None")
    -- a move by a right click on the ground: a place as its target
    moho64ui_MouseMove(cx + 60, cy + 40)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, cx + 60, cy + 40)
    moho64ui_MouseButton("Right", false, cx + 60, cy + 40)
    WaitSeconds(0.2)
    c = heard[table.getn(heard)]
    check("a move: its target is a Position", c and (c.CommandType .. " " .. c.Target.Type), "Move Position")
    check("and its Blueprint the empty string too", c and c.Blueprint, "")
    commandmode.OnCommandIssued = oldIssued
    notify.createEnhancementOverlay = oldCreate
    SelectUnits(nil)

    -- 4. cam_SetLOD on a camera (0x7adfc0): its LOD scale; one word only or no such camera: nothing
    -- (FAF's WorldViewCameraComponent sets 0.70 itself when the profile's Level of Detail is "Extreme", 2.1)
    local extreme = (GetOptions("level_of_detail") or 0) > 2
    check("the world camera's LOD scale as FAF's world view left it (1, cam_DefaultLOD; 0.70 for Extreme)",
        string.format("%.2f", moho64ui_CameraLodScale("WorldCamera")), extreme and "0.70" or "1.00")
    ConExecute("cam_SetLOD WorldCamera 0.70")
    check("cam_SetLOD WorldCamera 0.70", string.format("%.2f", moho64ui_CameraLodScale("WorldCamera")), "0.70")
    ConExecute("cam_SetLOD WorldCamera")
    check("with one word nothing changes", string.format("%.2f", moho64ui_CameraLodScale("WorldCamera")), "0.70")
    ConExecute("cam_SetLOD WorldCamera 1.0")
    check("and back to 1", moho64ui_CameraLodScale("WorldCamera"), 1)

    -- 5. cam_Free: Space and the mouse turn the camera from far out too, and it stays turned (0x870982, 0x870a79)
    local cam = GetCamera("WorldCamera")
    cam:Reset()
    WaitSeconds(0.5)
    local north = string.format("%.2f", cam:SaveSettings().Heading)
    ConExecute("cam_Free on")
    moho64ui_MouseMove(cx, cy)
    moho64ui_Key(32, "", "down")
    moho64ui_MouseMove(cx - 100, cy)
    moho64ui_MouseMove(cx + 100, cy - 60)
    moho64ui_Key(32, "", "up")
    WaitSeconds(0.5)
    local turned = string.format("%.2f", cam:SaveSettings().Heading)
    check("with cam_Free from far out Space and the mouse turn the camera", turned ~= north, true)
    WaitSeconds(0.5)
    check("and it does not swing back", string.format("%.2f", cam:SaveSettings().Heading), turned)
    ConExecute("cam_Free off")
    cam:Reset()
    WaitSeconds(0.5)

    -- 6. the interface's blueprints and the simulation's (one load each today; their records must agree)
    for _, id in {"uel0001", "ueb0101", "uea0102", "url0107"} do
        local f = moho64ui_BlueprintFacts(id)
        check("blueprint " .. id .. ": the same count, ordinal, entity index and caps in both",
            string.format("%s %s %s %s %s %s", tostring(f.interface.count), tostring(f.interface.entities), tostring(f.interface.ordinal),
                tostring(f.interface.entityIndex), tostring(f.interface.commandCaps), tostring(f.interface.toggleCaps)),
            string.format("%s %s %s %s %s %s", tostring(f.sim.count), tostring(f.sim.entities), tostring(f.sim.ordinal),
                tostring(f.sim.entityIndex), tostring(f.sim.commandCaps), tostring(f.sim.toggleCaps)))
    end
    local f = moho64ui_BlueprintFacts("uel0001")
    check("the commander's blueprint is among them", f.interface.ordinal ~= nil and f.interface.count > 1000, true)

    -- 7. the reclaim cursor's picture, a TGA file (the tour log's "texture format not read yet")
    local tga = Bitmap(GetFrame(0), "/textures/ui/common/game/cursors/reclaim-disabled.tga")
    check("the TGA cursor picture is read: 32 x 32 less its border ring", tga.BitmapWidth() .. " x " .. tga.BitmapHeight(), "30 x 30")
    tga:Destroy()

    LOG("TEST done: " .. failed .. " failed")
end)
