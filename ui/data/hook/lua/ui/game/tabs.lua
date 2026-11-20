-- FA Remastered: "Report a Problem" in the game's menu (this file is a hook: the program reads it after the game's
-- /lua/ui/game/tabs.lua, which stays as it is; the two are one chunk, so the game's own tables `menus` and `actions`
-- are in reach here). Chris, 2026-11-20, on the debugger's next step: "I'll let you decide what is the best as in most
-- convenient and gets us the best info". The row stands in every kind of the menu (a game one plays, a replay, a
-- network game) before the rows that leave the game, and opens the report's question (/moho64ui/lua/report.lua: one
-- line of what happened, then the report's zip onto the Desktop). The menu closes first, as for its other rows.
do
    local row = { action = 'RemasterReport', label = 'Report a Problem', tooltip = 'esc_remaster_report' }
    local leaving = { RestartGame = true, RestartReplay = true, EndSPGame = true, EndMPGame = true, EndGame = true,
                      ExitSPGame = true, ExitMPGame = true, ExitGame = true, Return = true }
    for _, list in menus.main do
        local at = table.getn(list) + 1
        for i, item in list do
            if leaving[item.action] then
                at = i
                break
            end
        end
        table.insert(list, at, row)
    end
    actions.RemasterReport = function()
        import("/moho64ui/lua/report.lua").Ask()
    end

    -- (for the tests: the menu's lists and the action's function)
    function RemasterMenus() return menus end
    function RemasterMenuAction(name) return actions[name] end
end
