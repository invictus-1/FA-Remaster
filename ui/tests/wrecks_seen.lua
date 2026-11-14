-- "Wrecks appear only once seen" (ours, Chris 2026-11-13; Options, Remastered tab, off at first = the original's way):
-- the interface's half. With the option on a unit's wreck that the player's army and its allies have not seen is not
-- under the reclaim cursor and has no reclaim label, until one of them has had its place in sight; off, or with no
-- focus army, everything is as the original. The fact is the engine's (PropSeenBy, ReclaimableAt's armyMask); the
-- picture's half is the renderer's (WorldPicture::SetWrecksOnceSeen, his own check). In a game with fog of war and
-- cheats (the wrecks are made by FAF's CheatBoxSpawnProp, a unit's wreck blueprint, well after the game's start):
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --no-world
--            --session <session_ai_cheats.lua> --seed 7 --game-lua ui/tests/wrecks_seen.lua --frames 1500 --log out.log
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
local function Upvalue(fn, name)
    if type(fn) ~= 'function' then return nil end
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then return nil end
        if n == name then return v end
    end
end
local WRECK = '/props/defaultwreckage/defaultwreckage_prop.bp'
local R = import('/lua/ui/game/reclaim.lua')
-- whether the game's own tables hold a label's point for that id (its locals, made anew when the playable area is set)
local function Labelled(id)
    local fn = R.RemasterGameUpdateReclaim()
    local inside, outside = Upvalue(fn, 'Reclaim'), Upvalue(fn, 'OutsidePlayableAreaReclaim')
    return (inside and inside[id] ~= nil) or (outside and outside[id] ~= nil) or false
end
local function Held(id) return R.RemasterReclaimHeld()[id] ~= nil end

ForkThread(function()
    WaitSeconds(5)
    UISelectionByCategory("COMMAND", false, false, false, false)
    local acu = GetSelectedUnits()[1]
    if not acu then
        LOG("TEST FAIL no commander to select")
        LOG("TEST done: 1 failed")
        return
    end
    local p = acu:GetPosition()
    local size = SessionGetScenarioInfo().size
    check("the option is off at first (the original's way)", tostring(moho64ui_Looks().wrecksOnceSeen) .. " " .. tostring(moho64ui_WrecksOnceSeen()), "false false")
    local function spawn(x, z)
        SimCallback({Func = 'CheatBoxSpawnProp', Args = {bpId = WRECK, pos = {x, 0, z}, count = 1, yaw = 0}})
    end
    local corners = {{30, 30}, {size[1] - 30, 30}, {30, size[2] - 30}, {size[1] - 30, size[2] - 30}}
    for _, c in corners do spawn(c[1], c[2]) end
    local nx, nz = p[1] + 8, p[3] + 8
    spawn(nx, nz)
    WaitSeconds(2)
    local function wreckAt(x, z)
        local w = moho64ui_ReclaimableAt(x, z, 3)
        if w and string.find(string.lower(w.blueprint), 'wreckage', 1, true) then return w end
    end
    local near = wreckAt(nx, nz)
    check("a wreck stands beside the commander", near ~= nil, true)
    local far = {}
    for _, c in corners do
        local w = wreckAt(c[1], c[2])
        if w then table.insert(far, {id = w.idText, x = c[1], z = c[2]}) end
    end
    check("and one at each corner of the map", table.getn(far), 4)
    if not near or table.getn(far) < 4 then
        LOG("TEST done: " .. failed .. " failed")
        return
    end

    -- off: every wreck is the view's, as in the original
    local seenOff, cursorOff = 0, 0
    for _, w in far do
        if moho64ui_PropSeen(w.id) then seenOff = seenOff + 1 end
        if moho64ui_CanReclaimAt(w.x, w.z, 3) then cursorOff = cursorOff + 1 end
    end
    check("off: all four corner wrecks are the view's and take the reclaim cursor (the commander selected)", seenOff .. " " .. cursorOff, "4 4")

    -- on
    ConExecute("remaster_wrecks_once_seen 1")
    check("on: the program holds wrecks back for the focus army", tostring(moho64ui_Looks().wrecksOnceSeen) .. " " .. tostring(moho64ui_WrecksOnceSeen()), "true true")
    local unseen = {}
    for _, w in far do
        if not moho64ui_PropSeen(w.id) then table.insert(unseen, w) end
    end
    LOG("TEST note: " .. table.getn(unseen) .. " of the 4 corner wrecks are out of the army's sight")
    for _, w in far do LOG("TEST note: the wreck " .. w.id .. " at " .. w.x .. "," .. w.z .. ": seen by armies " .. moho64ui_PropSeenBy(w.id)) end
    LOG("TEST note: the wreck beside the commander " .. near.idText .. " (" .. near.blueprint .. "): seen by armies " .. moho64ui_PropSeenBy(near.idText))
    LOG("TEST note: the map " .. size[1] .. "x" .. size[2] .. ", the commander at " .. math.floor(p[1]) .. "," .. math.floor(p[3]) .. ", focus army " .. GetFocusArmy())
    check("at least two corner wrecks have not been seen by the army or its ally", table.getn(unseen) >= 2, true)
    -- (ids are texts, as the scripts have them: a prop's id does not fit a Lua number)
    check("the wreck beside the commander has been seen", moho64ui_PropSeen(near.idText), true)
    check("an id that is no wreck counts as seen", moho64ui_PropSeen(acu:GetEntityId()), true)
    if table.getn(unseen) < 2 then
        ConExecute("remaster_wrecks_once_seen 0")
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local A, B = unseen[1], unseen[2]
    local idA, idB, idNear = A.id, B.id, near.idText
    check("the reclaim cursor: not over an unseen wreck, still over the seen one", tostring(moho64ui_CanReclaimAt(A.x, A.z, 3)) .. " " ..
        tostring(moho64ui_CanReclaimAt(nx, nz, 3)), "false true")
    check("the simulation still has the unseen wreck there (nothing of the game changes)", wreckAt(A.x, A.z) ~= nil, true)

    -- the labels: the simulation tells of both; the unseen one's is held back
    R.RemasterReclaimBeat()  -- (the hook takes up the option at the next beat: here at once)
    R.UpdateReclaim({
        [idA] = {mass = 250, position = {A.x, 0, A.z}},
        [idNear] = {mass = 250, position = {nx, 0, nz}},
    })
    check("the seen wreck's label reaches the game's labels, the unseen one's is held back",
        tostring(Labelled(idNear)) .. " " .. tostring(Labelled(idA)) .. " " .. tostring(Held(idA)), "true false true")

    -- a scout of ours beside the unseen wreck: seen from then on
    SimCallback({Func = 'CheatSpawnUnit', Args = {army = GetFocusArmy(), pos = {A.x + 4, 0, A.z + 4}, bpId = 'uel0101', count = 1, yaw = 0}})
    WaitSeconds(2)
    check("a scout of ours beside it: the wreck has been seen", moho64ui_PropSeen(A.id), true)
    check("it takes the reclaim cursor", moho64ui_CanReclaimAt(A.x, A.z, 3), true)
    check("and its label was handed on by the beat's own step", tostring(Labelled(idA)) .. " " .. tostring(Held(idA)), "true false")

    -- switched off with a label held back: it shows; switched on again in the middle of the game: it goes
    R.UpdateReclaim({[idB] = {mass = 250, position = {B.x, 0, B.z}}})
    check("another unseen wreck's label is held back", tostring(Labelled(idB)) .. " " .. tostring(Held(idB)), "false true")
    ConExecute("remaster_wrecks_once_seen 0")
    WaitSeconds(1)
    check("the option switched off: the held label shows, the cursor takes the wreck",
        tostring(Labelled(idB)) .. " " .. tostring(Held(idB)) .. " " .. tostring(moho64ui_CanReclaimAt(B.x, B.z, 3)), "true false true")
    ConExecute("remaster_wrecks_once_seen 1")
    WaitSeconds(1)
    check("switched on in the middle of the game: the unseen wreck's label goes again, the seen ones stay",
        tostring(Labelled(idB)) .. " " .. tostring(Held(idB)) .. " " .. tostring(Labelled(idA)) .. " " .. tostring(Labelled(idNear)), "false true true true")
    R.UpdateReclaim({[idB] = false})
    check("the simulation says the held wreck is gone: nothing is kept of it", tostring(Held(idB)) .. " " .. tostring(Labelled(idB)), "false false")
    ConExecute("remaster_wrecks_once_seen 0")
    WaitSeconds(0.5)
    LOG("TEST done: " .. failed .. " failed")
end)
