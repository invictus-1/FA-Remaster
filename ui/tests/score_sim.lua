-- Run in the simulation by ui/tests/score_tabs.lua (ExecLuaInSim('dofile', 'ui/tests/score_sim.lua'), which the
-- simulation carries out only with cheats on): the newest entry of the score history the simulation's
-- scripts keep (FAF's /lua/sim/score.lua, scoreData.history: one copy of every army's score each 30 seconds,
-- never changed afterwards), printed for the interface to compare with what it is handed at the end.
local scoreData = import("/lua/sim/score.lua").scoreData
local h = table.getn(scoreData.history)
local paths = {
    {"general", "score"}, {"general", "kills", "count"}, {"general", "built", "count"}, {"general", "lost", "count"},
    {"general", "kills", "mass"}, {"general", "currentunits"}, {"resources", "massin", "total"},
    {"resources", "energyin", "total"}, {"resources", "massout", "excess"}, {"units", "land", "built"},
    {"units", "land", "lost"}, {"units", "structures", "kills"}, {"units", "air", "built"},
}
for army, entry in scoreData.history[h] or {} do
    for _, path in paths do
        local v = entry
        for _, key in path do v = type(v) == "table" and v[key] or nil end
        print("SCORESIM " .. h .. " " .. army .. " " .. table.concat(path, ".") .. " " .. tostring(v))
    end
end
print("SCORESIM done " .. h)
