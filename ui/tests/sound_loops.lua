-- A unit's running sound ends with the unit, however the unit goes (the review of 2026-11-09, row C).
-- A running sound (a loop: a generator's hum, an engine) is started and ended by the unit's script; a unit that is
-- killed ends its own in its death thread. A unit that goes without being killed ends none: a structure replaced by
-- its upgrade, a unit given to another player, a reclaimed one, one a script destroys. For those the simulation says
-- "this entity's running sound ends" with no army's name on it, and until 2026-11-10 the host took that for another
-- army's sound and let its own play on: at the place, whenever the camera came near, for the rest of the game.
-- Here each of the four ways is gone through with a structure whose hum runs (ui/tests/sound_loops_sim.lua, in the
-- simulation), and the host's own check (GameSession::CheckLoopEntities, every tenth beat:
-- moho64ui_SoundInfo().goneLoops) must count no running sound whose entity is gone.
-- Run it like game.lua with a cheats session: --session session_ai_cheats.lua --game-lua ui/tests/sound_loops.lua
-- --frames 3600.
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
    local heard = {}
    AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
    -- the simulation's last "SOUNDLOOPS state" line says that this one is gone
    local function gone(name)
        for i = table.getn(heard), 1, -1 do
            if string.find(heard[i], "SOUNDLOOPS state", 1, true) then
                return string.find(heard[i], name .. "=gone", 1, true) ~= nil
            end
        end
        return false
    end
    local c = GetArmyAvatars()[1]:GetPosition()
    GetCamera("WorldCamera"):SnapTo(c, nil, 80)
    WaitSeconds(0.5)
    local before = moho64ui_SoundInfo()
    ExecLuaInSim("dofile", "ui/tests/sound_loops_sim.lua")
    ExecLuaInSim("SoundLoopsSetup", {c[1], c[3]})
    WaitSeconds(2.5)
    local info = moho64ui_SoundInfo()
    LOG("TEST note: running sounds kept: " .. before.loops .. " before, " .. info.loops .. " with the four structures and the engineer")
    check("the four structures' hums run (their scripts' ActiveLoop)", info.loops - before.loops >= 4, true)
    check("no running sound without its entity before the cases", info.goneLoops, 0)
    for _, name in {"destroy", "upgrade", "give", "reclaim"} do
        local had = moho64ui_SoundInfo().loops
        ExecLuaInSim("SoundLoopsCase", name)
        local waited = 0
        repeat
            WaitSeconds(0.5)
            waited = waited + 0.5
            ExecLuaInSim("SoundLoopsState", name)
            WaitSeconds(0.3)
        until gone(name) or waited > 25
        check(name .. ": the structure is gone", gone(name), true)
        WaitSeconds(2.2)  -- (the check runs every tenth beat, a second)
        info = moho64ui_SoundInfo()
        LOG("TEST note: " .. name .. ": gone after " .. waited .. " s; running sounds kept " .. had .. " -> " .. info.loops .. "; without an entity: " .. info.goneLoops)
        check(name .. ": no running sound is left without its entity", info.goneLoops, 0)
    end
    check("and none at any check of the run", moho64ui_SoundInfo().goneLoopsMost, 0)
    LOG("TEST done: " .. failed .. " failed")
end)
