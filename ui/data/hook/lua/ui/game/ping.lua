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

-- FA Remastered: a radial ping's meaning under it, for the allies who see it (ours; the ring /moho64ui/lua/radialping.lua
-- sends FAF's ping with an Intent): "Attack ACU", "Expand here" and so on, in the look's colour, below the ping's
-- place in each world view while the ping lasts, fading in its last second. FAF's own pings (no Intent) as before.
-- RemasterPingLabels: the labels showing, for the tests ({ text, control, ping }).
RemasterPingLabels = {}
local function RemasterPingLabel(ping)
    local radial = import("/moho64ui/lua/radialping.lua")
    local label, look
    for _, p in radial.Pings do
        if p.intent == ping.Intent then label, look = p.label, p.look end
    end
    if not label then return end
    local color = ({ attack = 'ffff7060', move = 'ff70b0ff', alert = 'ffffd860' })[look] or 'ffffffff'
    local UIUtil = import("/lua/ui/uiutil.lua")
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    local lifetime = ping.Lifetime or 6
    for name, each in import("/lua/ui/game/worldview.lua").GetWorldViews() or {} do
        local view = each  -- (a local of its own for the label's frame function below: Lua 5.0's loop variable is not)
        if view and name ~= 'MiniMap' and not IsDestroyed(view) then
            local text = UIUtil.CreateText(view, label, 14, UIUtil.bodyFont)
            text:SetColor(color)
            text:SetDropShadow(true)
            text:DisableHitTest()
            local entry = { text = label, control = text, ping = ping }
            table.insert(RemasterPingLabels, entry)
            local time = 0
            local function Place()
                local at = view:Project(Vector(ping.Location[1], ping.Location[2], ping.Location[3]))
                text.Left:Set(view.Left() + at.x - text.Width() / 2)
                text.Top:Set(view.Top() + at.y + LayoutHelpers.ScaleNumber(18))
            end
            Place()
            text:SetNeedsFrameUpdate(true)
            text.OnFrame = function(self, delta)
                time = time + delta
                if time >= lifetime or IsDestroyed(view) then
                    self:SetNeedsFrameUpdate(false)
                    for i, e in RemasterPingLabels do
                        if e == entry then table.remove(RemasterPingLabels, i) break end
                    end
                    self:Destroy()
                    return
                end
                if view:IsHidden() then self:Hide() else self:Show() end
                Place()
                self:SetAlpha(math.min(1, lifetime - time))
            end
        end
    end
end
local RemasterUnlabelled = DisplayPing
function DisplayPing(data)
    local result = RemasterUnlabelled(data)
    for _, ping in data or {} do
        if type(ping.Intent) == 'string' and not ping.Action and not ping.Renew and type(ping.Location) == 'table' then
            RemasterPingLabel(ping)
        end
    end
    return result
end
