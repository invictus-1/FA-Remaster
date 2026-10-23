-- FA Remastered: Colour Vision and the marker pings (this file is a hook: the program reads it after the game's
-- /lua/ui/game/ping.lua, which stays as it is). A marker ping carries its army's colour (DoPing takes it from
-- GetArmiesTable) into the simulation, which keeps it, hands it to every player and so writes it in the replay. With
-- Colour Vision on (ours, the Interface tab; ui/data/hook/lua/options/options.lua) GetArmiesTable gives the colour the
-- player sees; what goes to the simulation must stay the army's own, so that the replay and the other players' screens
-- are the same with the option on or off. So: a ping's colour going out is turned back into the army's own, and a
-- ping's colour coming in (the army's own) is shown as Colour Vision's. Any other colour is left as it is.

-- The colour the simulation should get for `data` (a ping's table: Owner, the army from 0, and Color); nil to leave it.
-- Any kind's colour for the army is turned back, not only the one chosen now: FAF's GetArmiesTable is a copy renewed
-- every few seconds (/lua/ui/override/ArmiesTable.lua), so just after a change it still holds the kind before.
function RemasterPingColorForSim(data)
    if type(data) ~= 'table' or type(data.Color) ~= 'string' or type(data.Owner) ~= 'number' or not moho64ui_ArmyColors then
        return nil
    end
    local color = string.lower(data.Color)
    for kind = 1, 4 do
        local given, seen = moho64ui_ArmyColors(data.Owner + 1, kind)
        if given and seen and given ~= seen and color == string.lower(seen) then return given end
    end
    return nil
end
-- The colour to show for a ping the simulation handed over; nil to leave it.
function RemasterPingColorShown(data)
    if type(data) ~= 'table' or type(data.Color) ~= 'string' or type(data.Owner) ~= 'number' or not moho64ui_ArmyColors then
        return nil
    end
    local given, seen = moho64ui_ArmyColors(data.Owner + 1)
    if given and seen and given ~= seen and string.lower(data.Color) == string.lower(given) then return seen end
    return nil
end

-- (The module's own SimCallback, which DoPing and UpdateMarker call: the global one, with the colour turned back.)
local RemasterSimCallback = SimCallback
SimCallback = function(callback, withUnits)
    if type(callback) == 'table' and (callback.Func == 'SpawnPing' or callback.Func == 'UpdateMarker') then
        local color = RemasterPingColorForSim(callback.Args)
        if color then
            local args = {}
            for k, v in callback.Args do args[k] = v end
            args.Color = color
            callback = { Func = callback.Func, Args = args }
        end
    end
    return RemasterSimCallback(callback, withUnits)
end

local RemasterDisplayPing = DisplayPing
function DisplayPing(data)
    for _, ping in data or {} do
        local color = RemasterPingColorShown(ping)
        if color then ping.Color = color end
    end
    return RemasterDisplayPing(data)
end
