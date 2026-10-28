-- FA Remastered: the radial ping menu (Chris, 2026-10-28, the AI design talk: pings our own AI understands, given by
-- a radial menu; docs/ai-design-goals.md, docs/ui-answers.md). This program's own, not a hook. Its key action
-- "Radial ping menu" (Y at first, where Y is free: ui/data/hook/lua/keymap) runs Open here. While the key is held a ring
-- of seven pings stands round the place the key went down; the mouse's way from there lights one (a small middle
-- sends nothing); letting go of the key sends the lit one, a left click sends it at once, a right click or Escape
-- sends nothing.
--
-- A ping goes the way FAF's own do (/lua/ui/game/ping.lua DoPing: the sim callback SpawnPing with FAF's ping table,
-- its Type one of FAF's for the look: Attack red, Move blue, Alert yellow), so FAF's meshes, rings, arrows, sounds and
-- its limit of a ping a second hold, and the allies' brains hear it (SimPing.SpawnPing: DoPingCallbacks / DoAIPing).
-- Two fields are ours: Intent (the AI's word for it) and Target (the entity id of the unit under the cursor when the
-- key went down, if there was one; the simulation is to check it). The place is where the key went down. Not in a
-- replay, nor for an observer, nor for another team (as DoPing).

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap

-- The pings, clockwise from the top: the intent, its label, and the FAF ping type it looks like.
Pings = {
    { intent = 'attack', label = 'Attack', look = 'attack' },
    { intent = 'attack_acu', label = 'Attack ACU', look = 'attack' },
    { intent = 'experimental', label = 'Experimental here', look = 'alert' },
    { intent = 'expand', label = 'Expand here', look = 'alert' },
    { intent = 'retreat', label = 'Retreat', look = 'alert' },
    { intent = 'defend', label = 'Defend', look = 'move' },
    { intent = 'assist', label = 'Assist', look = 'move' },
}
local lookColor = { attack = 'ffff5a4a', move = 'ff5aa0ff', alert = 'ffffd040' }
local wedgeColor, wedgeLit, wedgeEdge = 'cc101c22', 'ee26424c', 'ff8fb0b8'
local radius, deadZone, wedgeW, wedgeH = 118, 32, 156, 34

-- How things stand, for the tests: open; lit (its index in Pings, or nil); sent (the last ping table sent, or nil);
-- why (why the last open sent nothing: 'replay', 'observer', 'opponent', 'nothing lit', 'cancelled', 'no place').
State = { open = false, lit = nil, sent = nil, why = nil }
-- The menu's controls (cover, wedges[i] = { group, bg, text }), for the tests.
Controls = {}

local opened = false  -- { x, y (the screen place), place (the world's), target, keyCode }

-- The key the action is bound to now, as IsKeyDown takes it (a letter by its code), or nil.
local function HeldKey()
    local ok, keymapper = pcall(import, "/lua/keymap/keymapper.lua")
    if not ok then return nil end
    for key, action in keymapper.GetCurrentKeyMap() or {} do
        if action == 'remaster_radial_ping' then
            local last = key
            while true do
                local dash = string.find(last, "-", 2, true)
                if not dash then break end
                last = string.sub(last, dash + 1)
            end
            if string.len(last) == 1 then return string.byte(string.upper(last)) end
            return last
        end
    end
    return nil
end

local function Close()
    if opened and opened.escape then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
    if Controls.cover and not IsDestroyed(Controls.cover) then Controls.cover:Destroy() end
    Controls = {}
    opened = false
    State.open, State.lit = false, nil
end

local function Light(index)
    if State.lit == index then return end
    State.lit = index
    for i, w in Controls.wedges or {} do
        w.bg:SetSolidColor(i == index and wedgeLit or wedgeColor)
        w.text:SetColor(i == index and 'ffffffff' or 'ffd8e4e8')
    end
end

-- Which ping the mouse's way from the middle lights (nil inside the middle).
local function PingAt(x, y)
    local dx, dy = x - opened.x, y - opened.y
    local dead = LayoutHelpers.ScaleNumber(deadZone)
    if dx * dx + dy * dy < dead * dead then return nil end
    local n = table.getn(Pings)
    local angle = math.atan2(dx, -dy)  -- 0 at the top, clockwise
    if angle < 0 then angle = angle + 2 * math.pi end
    local step = 2 * math.pi / n
    return math.mod(math.floor(angle / step + 0.5), n) + 1
end

local function Send()
    local ping = State.lit and Pings[State.lit]
    if not ping then
        State.why = 'nothing lit'
        return Close()
    end
    local Ping = import("/lua/ui/game/ping.lua")
    local data = { Owner = Ping.OriginalFocusArmy - 1, Type = ping.look, Location = opened.place }
    data = table.merged(data, Ping.PingTypes[ping.look])
    data.Intent = ping.intent
    data.Target = opened.target
    SimCallback({ Func = 'SpawnPing', Args = data })
    State.sent = data
    State.why = nil
    Close()
end

-- Opens the menu where the mouse is (the key action's call). `keyCode`: the key to watch for its release (by
-- default the one bound to the action now; false: none, the menu waits for a click).
function Open(keyCode)
    if opened then return Close() end
    State.sent = nil
    if SessionIsReplay() or import("/lua/ui/game/gamemain.lua").supressExitDialog then
        State.why = 'replay'
        return
    end
    local focusArmy = GetFocusArmy()
    if focusArmy == -1 then
        State.why = 'observer'
        return
    end
    local Ping = import("/lua/ui/game/ping.lua")
    if not IsAlly(focusArmy, Ping.OriginalFocusArmy) then
        State.why = 'opponent'
        return
    end
    local place = GetMouseWorldPos()
    for _, v in place do
        if v ~= v then  -- (not a number: the mouse over no ground)
            State.why = 'no place'
            return
        end
    end
    local info = GetRolloverInfo()
    local mouse = GetMouseScreenPos()
    if keyCode == nil then keyCode = HeldKey() end
    opened = { x = mouse[1], y = mouse[2], place = place, target = info and info.entityId or nil, keyCode = keyCode }

    local frame = GetFrame(0)
    local cover = Group(frame)
    LayoutHelpers.FillParent(cover, frame)
    cover.Depth:Set(frame:GetTopmostDepth() + 1)
    Controls.cover = cover
    Controls.wedges = {}
    local n = table.getn(Pings)
    for i, ping in Pings do
        local angle = (i - 1) * 2 * math.pi / n
        local cx = opened.x + LayoutHelpers.ScaleNumber(radius) * math.sin(angle)
        local cy = opened.y - LayoutHelpers.ScaleNumber(radius) * math.cos(angle)
        local edge = Bitmap(cover)
        edge:SetSolidColor(wedgeEdge)
        edge.Width:Set(LayoutHelpers.ScaleNumber(wedgeW) + 2)
        edge.Height:Set(LayoutHelpers.ScaleNumber(wedgeH) + 2)
        edge.Left:Set(cx - edge.Width() / 2)
        edge.Top:Set(cy - edge.Height() / 2)
        edge:DisableHitTest()
        local bg = Bitmap(edge)
        bg:SetSolidColor(wedgeColor)
        LayoutHelpers.FillParentFixedBorder(bg, edge, 1)
        bg:DisableHitTest()
        local mark = Bitmap(bg)
        mark:SetSolidColor(lookColor[ping.look])
        mark.Width:Set(LayoutHelpers.ScaleNumber(4))
        LayoutHelpers.AtLeftIn(mark, bg)
        mark.Top:Set(bg.Top)
        mark.Bottom:Set(bg.Bottom)
        mark:DisableHitTest()
        local text = UIUtil.CreateText(bg, ping.label, 15, UIUtil.bodyFont)
        text:SetColor('ffd8e4e8')
        text:SetDropShadow(true)
        LayoutHelpers.AtCenterIn(text, bg)
        text:DisableHitTest()
        Controls.wedges[i] = { group = edge, bg = bg, text = text }
    end
    local middle = Bitmap(cover)
    middle:SetSolidColor(wedgeEdge)
    middle.Width:Set(LayoutHelpers.ScaleNumber(8))
    middle.Height:Set(LayoutHelpers.ScaleNumber(8))
    middle.Left:Set(opened.x - middle.Width() / 2)
    middle.Top:Set(opened.y - middle.Height() / 2)
    middle:DisableHitTest()

    -- (a click acts when its button is let go, so that both halves of it stay with the ring and nothing under it
    -- gets a release without its press)
    local pressed = false
    cover.HandleEvent = function(self, event)
        if event.Type == 'MouseMotion' then
            Light(PingAt(event.MouseX, event.MouseY))
        elseif event.Type == 'ButtonPress' or event.Type == 'ButtonDClick' then
            pressed = event.Modifiers.Left and 'left' or 'other'
        elseif event.Type == 'ButtonRelease' and pressed then
            if pressed == 'left' then
                Light(PingAt(event.MouseX, event.MouseY))
                Send()
            else
                State.why = 'cancelled'
                Close()
            end
        end
        return true
    end
    -- (Escape: nothing is sent; FAF's escape handlers' stack, so a dialog's own handler comes back after)
    import("/lua/ui/dialogs/eschandler.lua").PushEscapeHandler(function()
        State.why = 'cancelled'
        Close()
    end)
    opened.escape = true
    -- (the key let go: the lit ping goes)
    cover:SetNeedsFrameUpdate(true)
    cover.OnFrame = function(self, delta)
        if opened and opened.keyCode and not IsKeyDown(opened.keyCode) then Send() end
    end
    State.open = true
    State.lit = nil
    State.at = { opened.x, opened.y }
end
