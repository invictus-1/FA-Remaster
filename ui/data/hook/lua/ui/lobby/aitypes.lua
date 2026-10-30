-- FA Remastered: our own AI in the lobby's list of AIs (this file is a hook: the program reads it after the game's
-- /lua/ui/lobby/aitypes.lua, which stays as it is). Its four skills (Chris, docs/ai-design-goals.md: Recruit,
-- Veteran, Commander, Supreme Commander) and each one's cheating twin, by the keys the engine's AI knows
-- (data/ai/hook/lua/aibrains/index.lua: remastered_<skill> and remastered_<skill>_cheat; the simulation mounts its
-- folder only in a game where an army plays one of them). First in the list, the game's own and the mods' AIs after
-- them as before. Personality, aggression, economy and teacher mode are per-slot options (our lobby.lua hook).
do
    local skills = {
        { key = 'recruit', name = 'Recruit' },
        { key = 'veteran', name = 'Veteran' },
        { key = 'commander', name = 'Commander' },
        { key = 'supreme', name = 'Supreme Commander' },
    }
    local RemasterGameGetAItypes = GetAItypes
    function GetAItypes()
        local game = RemasterGameGetAItypes()
        local list = {}
        for _, skill in skills do
            table.insert(list, { key = 'remastered_' .. skill.key, name = 'Remastered AI: ' .. skill.name })
        end
        for _, skill in skills do
            table.insert(list, { key = 'remastered_' .. skill.key .. '_cheat', name = 'Remastered AIx: ' .. skill.name })
        end
        for _, entry in game do table.insert(list, entry) end
        return list
    end
end
