-- reclaim_two_games.lua: no state of one game may reach the next game of the same program run (review of 2026-11-09,
-- R7: the reclaim cells' cache of script/fastlib_reclaim.cpp was one map for the process).
--
-- Run in the sim's state at the end of each of two games played one after the other in one process:
--
--   moho64 --init <init.lua> --session tests/reclaim_two_games_a.lua --then-session tests/reclaim_two_games_b.lua
--          --seed 1 --ticks 20 --sim-lua tests/reclaim_two_games.lua --log <log>
--
-- The two session files are the same game but for one number, GameOptions.ReclaimTestLeft (1 and 0.5), which nothing
-- but this file reads; so the second game's Lua state makes the same objects in the same order as the first's, with
-- the same serials and write stamps. This file builds one reclaim cell of the map's first props, gives each prop
-- ReclaimLeft = ReclaimTestLeft with the same number of table writes in both games, and asks the engine's helper
-- Remastered.ReclaimCellTotals for the cell's totals. A cache that outlives the first game answers the FIRST game's
-- totals when the second game's cell also lies at the first's ADDRESS.
--
-- The log must hold "reclaim_two_games: OK" twice and never "reclaim_two_games: STALE".
--
-- What this test is, and is not (2026-11-10): a guard on two games in one process. It did NOT show the fault on the
-- engine as it was: the game's allocator gave the cell another address in each of 16 games of one run (the line
-- prints it), so the old cache was never met and the old exe answers OK too. The fault itself is shown by
-- tests/reclaim_state_cache_test.cpp, which gives Lua an allocator that repeats the addresses.
local left = ScenarioInfo.Options.ReclaimTestLeft or 1
local Native = rawget(_G, 'Remastered')
if not (Native and Native.ReclaimCellTotals) then
    WARN('reclaim_two_games: SKIPPED (no Remastered.ReclaimCellTotals: the script fixes are off)')
    return
end
local size = ScenarioInfo.size
local found = GetReclaimablesInRect(0, 0, size[1], size[2]) or {}
-- the props, by id (digit strings: the shorter is the smaller, then by text)
local props, n = {}, 0
for _, r in found do
    if r.IsProp and not IsDestroyed(r) then
        n = n + 1
        props[n] = r
    end
end
table.sort(props, function(a, b)
    local sa, sb = tostring(a.EntityId), tostring(b.EntityId)
    if string.len(sa) ~= string.len(sb) then return string.len(sa) < string.len(sb) end
    return sa < sb
end)
local count = math.min(n, 32)
local cell = {}
local wantMass, wantEnergy = 0, 0
for i = 1, count do
    local r = props[i]
    r.ReclaimLeft = left
    r.MaxMassReclaim = 10
    r.MaxEnergyReclaim = 3
    cell[tostring(r.EntityId)] = r
    wantMass = wantMass + left * 10
    wantEnergy = wantEnergy + left * 3
end
local gotCount, gotMass, gotEnergy = Native.ReclaimCellTotals(cell)
local verdict = (gotCount == count and gotMass == wantMass and gotEnergy == wantEnergy) and 'OK' or 'STALE'
local line = 'reclaim_two_games: ' .. verdict .. ': ReclaimLeft ' .. tostring(left) .. ', ' .. tostring(count) ..
    ' props of ' .. tostring(n) .. ': totals ' .. tostring(gotCount) .. ' / ' .. tostring(gotMass) .. ' / ' ..
    tostring(gotEnergy) .. ', expected ' .. tostring(count) .. ' / ' .. tostring(wantMass) .. ' / ' ..
    tostring(wantEnergy) .. ' (cell ' .. tostring(cell) .. ')'
if verdict == 'OK' and count > 0 then
    LOG(line)
else
    WARN(count > 0 and line or 'reclaim_two_games: SKIPPED (the map has no props)')
end
