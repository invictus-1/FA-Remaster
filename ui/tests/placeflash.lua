-- A building placed with the build mode: per frame, from 5 frames before each click to 30 after, what the world view
-- hands the picture at the site (Chris's re-test 2026-11-06, item 3: "when placing a structure for a queue, the
-- complete building shows for a split second before the green ghost"). The flash was the order's feedback blip: FAF's
-- OnBuildMobileIssued (commandmode.lua) asks AddCommandFeedbackBlip for the building's mesh by its BlueprintID with
-- flag02d_albedo.dds and the CommandFeedback shader, and ours drew it with the building's own albedo and the "Unit"
-- shader, a finished building, for the blip's 0.7 s (user/world_meshes.cpp, FA exe 0x857be0).
-- Checked, each frame: nothing the script placed stands at the site as an ordinary model (only the flat feedback
-- shaders); and the site always has a ghost handed over (the cursor's or the ordered site's), or the building being
-- built there. Also: the cursor's ghost from the first frame of the build mode; the blip as the original makes it.
-- Two generators queued with Shift, one placed without. Units made by the game; run it like orderlines.lua:
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --quiet
--            --session <session_ai_cheats.lua> --seed 7 --game-lua ui/tests/placeflash.lua --frames 2400 --log out.log
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
local flat = {commandfeedback = true, commandfeedback2 = true, commandfeedback3 = true, commandfeedback4 = true,
              rallypoint = true, fakerings = true}

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local CM = import("/lua/ui/game/commandmode.lua")
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local function onScreen(at)
        local p = {at[1], at[2], at[3]}
        for i = 1, 4 do
            local g = UnProject(view, view:Project(p))
            if g and g[2] then p[2] = g[2] end
        end
        return view:Project(p)
    end
    UIZoomTo({cdr}, 0)
    WaitSeconds(1)
    for i = 1, 2 do moho64ui_MouseWheel(800, 300, -1) WaitSeconds(0.1) end
    WaitSeconds(1.5)
    local function at(site, x, z) return math.abs(x - site[1]) < 0.6 and math.abs(z - site[3]) < 0.6 end
    -- what is handed over at the site this frame: "ghost" (cursor / ordered), "built" (the commander's work there,
    -- unfinished), and the first script mesh there drawn as an ordinary model
    local function look(site)
        local ghosts = moho64ui_HandedGhosts()
        local ghost
        for _, g in ghosts do
            if g.blueprint == "ueb1101" and at(site, g.x, g.z) then ghost = g.placing and "cursor" or "ordered" end
        end
        local building
        local f = cdr:GetFocus()
        local b = f and moho64ui_UnitBuilt(f:GetEntityId())
        if b and b.beingBuilt and b.fraction < 1 and at(site, f:GetPosition()[1], f:GetPosition()[3]) then building = b end
        local solid
        for _, m in moho64ui_WorldMeshes() do
            if at(site, m.x, m.z) and not flat[string.lower(m.shader)] then solid = solid or (m.mesh .. " " .. m.shader) end
        end
        return ghost, building, solid
    end
    -- per frame `frames` frames; counts the frames with a solid mesh at the site and those with nothing at all
    local function watch(site, tag, frames)
        local solid, bare = 0, 0
        for i = 1, frames do
            WaitFrames(1)
            local ghost, building, s = look(site)
            if s then solid = solid + 1 end
            if not ghost and not building then bare = bare + 1 end
            if s or (not ghost and not building) then
                LOG(string.format("TEST info: %s +%d: ghost %s, being built %s, script mesh %s", tag, i, tostring(ghost),
                    building and string.format("%.3f", building.fraction) or "-", tostring(s)))
            end
        end
        return solid, bare
    end
    local function click(site, shift, tag)
        local s = onScreen(site)
        moho64ui_MouseMove(s.x, s.y)
        local solid, bare = watch(site, tag .. " before", 5)
        moho64ui_MouseButton("Left", true, s.x, s.y, shift and "Shift" or nil)
        moho64ui_MouseButton("Left", false, s.x, s.y, shift and "Shift" or nil)
        WaitFrames(1)
        -- the blip of the order, as the original's AddCommandFeedbackBlip makes it of FAF's description
        local blip
        for _, m in moho64ui_WorldMeshes() do
            if at(site, m.x, m.z) then blip = m end
        end
        check(tag .. ": the order's blip is the building's mesh",
            blip and string.lower(blip.mesh), "/units/ueb1101/ueb1101_lod0.scm")
        check(tag .. ": with the description's texture and shader (FAF: flag02d, CommandFeedback)",
            blip and (blip.texture .. " " .. blip.shader), "/meshes/game/flag02d_albedo.dds CommandFeedback")
        check(tag .. ": at the blueprint's own scale", blip and blip.scale, __blueprints["ueb1101"].Display.UniformScale)
        local solid2, bare2 = watch(site, tag .. " after", 29)
        check(tag .. ": no frame round the click shows a script mesh there as an ordinary model", solid + solid2, 0)
        check(tag .. ": every frame round the click has its ghost there or the building begun", bare + bare2, 0)
    end
    SelectUnits({cdr})
    WaitSeconds(0.3)
    local A = {math.floor(c[1] - 8) + 0.5, c[2], math.floor(c[3] + 6) + 0.5}
    local B = {math.floor(c[1] - 11) + 0.5, c[2], math.floor(c[3] + 6) + 0.5}
    local C = {math.floor(c[1] + 8) + 0.5, c[2], math.floor(c[3] - 6) + 0.5}
    local s = onScreen(A)
    moho64ui_MouseMove(s.x, s.y)
    WaitFrames(2)
    CM.StartCommandMode("build", {name = "ueb1101"})
    WaitFrames(1)
    local ghost = look(A)
    check("the cursor's ghost from the build mode's first frame", ghost, "cursor")
    moho64ui_Key(306, "Shift", "down")
    WaitSeconds(0.3)
    click(A, true, "queued with Shift (the first)")
    click(B, true, "queued with Shift (the second)")
    moho64ui_Key(306, "", "up")
    WaitSeconds(0.5)
    CM.StartCommandMode("build", {name = "ueb1101"})
    click(C, false, "placed without Shift")
    -- the blip goes after its 0.7 s
    WaitSeconds(1)
    local left = 0
    for _, m in moho64ui_WorldMeshes() do
        if at(C, m.x, m.z) then left = left + 1 end
    end
    check("the blip is gone after its time", left, 0)
    CM.EndCommandMode(true)
    LOG("TEST done: " .. failed .. " failed")
end)
