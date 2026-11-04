-- bp_table.lua: the unit blueprints as the game's rules made them, one log line each, for tools/oraclestats.py
-- (which tells the units of an oracle recording apart by them). Run it after a game's first tick, with the game's
-- own mods, so that the table is that game's:
--   moho64 --init offline/bin/init_lab.lua --sim <replay> --ticks 1 --sim-lua fa-remaster/tools/bp_table.lua --log <file>
-- Each line: "BPT <id> <MaxHealth> <BuildCostMass> <categories...>" (the categories oraclestats.py reads; a line per
-- unit blueprint, in id order). It only reads the blueprints.
local want = {'UEF', 'AEON', 'CYBRAN', 'SERAPHIM', 'NOMADS', 'COMMAND', 'SUBCOMMANDER', 'ENGINEER', 'MOBILE',
    'STRUCTURE', 'MASSEXTRACTION', 'MASSFABRICATION', 'ENERGYPRODUCTION', 'HYDROCARBON', 'FACTORY', 'RESEARCH',
    'LAND', 'AIR', 'NAVAL', 'HOVER', 'AMPHIBIOUS', 'TECH1', 'TECH2', 'TECH3', 'EXPERIMENTAL'}
local ids = {}
for id, bp in __blueprints do
    if type(id) == 'string' and type(bp) == 'table' and bp.Defense and bp.Economy and bp.General and bp.Categories
        and bp.BlueprintId and string.lower(bp.BlueprintId) == id then
        table.insert(ids, id)
    end
end
table.sort(ids)
for _, id in ids do
    local bp = __blueprints[id]
    local has = {}
    for _, c in bp.Categories do has[c] = true end
    local cats = {}
    for _, c in want do if has[c] then table.insert(cats, c) end end
    LOG(string.format('BPT %s %.3f %.3f %s', id, bp.Defense.MaxHealth or 0, bp.Economy.BuildCostMass or 0,
        table.concat(cats, ' ')))
end
LOG('BPT end ' .. table.getn(ids))
