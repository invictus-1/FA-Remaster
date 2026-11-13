-- FA Remastered: the Options dialog's Tab Defaults button (ui/data/hook/lua/ui/dialogs/options.lua; Chris 2026-11-13)
Tooltips['options_tab_defaults'] = {
    title = 'Tab Defaults',
    description = 'Puts the options of the tab shown back to their defaults. Nothing is kept until OK or Apply; Cancel leaves them as they were.',
}

-- FA Remastered: the lobby's tooltips for our own AI (this file is a hook: the program reads it after the game's
-- /lua/ui/help/tooltips.lua, which stays as it is). The lobby asks for 'aitype_' .. the AI's key (lobby.lua
-- GetAITypes); without these our AIs showed "No Description". The four skills follow the engine's difficulty knobs
-- (fa-remaster/docs/ai-architecture.md 2.6: all with fair intel; the cheating twins add the lobby's AI cheat options).
do
    local skills = {
        recruit = { 'Recruit', 'Slow to react, gives few orders at a time, uses no micro and makes human-looking mistakes. For learning the game.' },
        veteran = { 'Veteran', 'Reacts in a few seconds, keeps a sound economy and fights with some micro. A relaxed game.' },
        commander = { 'Commander', 'Quick to react, keeps its economy tight and uses most of its micro. A real fight.' },
        supreme = { 'Supreme Commander', 'The fastest reactions, every micro skill and a tight opening: our AI at its best, still with fair intel.' },
    }
    local settings = ' Its personality, aggression, economy and teacher mode: "Remastered AI Settings..." in the slot\'s list.'
    for key, s in skills do
        Tooltips['aitype_remastered_' .. key] = {
            title = 'Remastered AI: ' .. s[1],
            description = s[2] .. ' Sees only what its units see.' .. settings,
        }
        Tooltips['aitype_remastered_' .. key .. '_cheat'] = {
            title = 'Remastered AIx: ' .. s[1],
            description = s[2] .. ' Cheats: the lobby\'s AI options give it more resources and faster building.' .. settings,
        }
    end
end
