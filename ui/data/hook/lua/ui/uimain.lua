-- FA Remastered: Escape for this program's own boxes (this file is a hook: the program reads it after the game's
-- /lua/ui/uimain.lua, which stays as it is). The game's EscapeHandler is what Escape runs when no control took the
-- key: in a game it goes to the stack of escape handlers (/lua/ui/dialogs/eschandler.lua), where a popup has put its
-- own, and the popup closes; on the main menu it goes to the ONE handler the menu has set (its Exit), past that
-- stack, so that Escape with one of our boxes up (the report's, "What's new", the first start's recommendation)
-- asked "Are you sure you'd like to quit?" and left the box standing (found by ui/tests/whatsnew.lua, 2026-11-27).
-- A box of ours therefore names its own Escape here while it is up; the newest named goes first, then the game's way.
local RemasterEscapes = {}

function RemasterEscapePush(handler)
    table.insert(RemasterEscapes, handler)
end

function RemasterEscapeDrop(handler)
    for i = table.getn(RemasterEscapes), 1, -1 do
        if RemasterEscapes[i] == handler then
            table.remove(RemasterEscapes, i)
            return
        end
    end
end

-- (for the tests: how many are named now)
function RemasterEscapeCount()
    return table.getn(RemasterEscapes)
end

local RemasterGameEscapeHandler = EscapeHandler
function EscapeHandler()
    local own = RemasterEscapes[table.getn(RemasterEscapes)]
    if own then
        own()
        return
    end
    RemasterGameEscapeHandler()
end
