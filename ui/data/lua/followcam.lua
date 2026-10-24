-- FA Remastered: the follow camera (Chris's approval of 2026-10-23: a small picture-in-picture view in a corner that
-- follows one unit, an experimental, a transport, the commander; a click on it jumps the main camera there). This
-- program's own, not a hook: the game has none. The key "Follow camera" (Ctrl-Alt-V at first) or the console's
-- remaster_follow_cam [on|off] opens it on the first unit of the selection (nothing selected: it says so and nothing
-- opens) and closes it again. Its picture is the renderer's (an inset view of the one world picture, drawn every 2nd
-- frame: ui/maui/worldview.cpp, FollowCamFrame and GatherViews); here its frame, of the game's own look: the minimap
-- window's border (/game/mini-map-brd/, as FAF's minimap.lua makes its Window) with the unit's name in its title bar
-- and the window's close button. 480 x 270 at the interface's scale 1 (LayoutHelpers.ScaleNumber), at the bottom
-- right of the screen above the game's bottom panels (the construction panel, the unit view, the orders: they come and
-- grow with a selection). A left click on the picture moves the main camera to the unit (moho64ui_FollowCamJump), a
-- right click or the X closes it. When the unit dies the picture stays on the wreck for about 3 s and closes (the
-- engine's). It goes away while the game's panels are put away (the game's own HideGameUI: the photo mode, the
-- player's key) and comes back with them.

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Button = import("/lua/maui/button.lua").Button
local Tooltip = import("/lua/ui/game/tooltip.lua")

-- How things stand, for the tests: open, unit (its entity id), name (the title's text), visible (on the screen now),
-- said (the last thing it said: why nothing opened), rect (the picture's place in the window: {x, y, w, h}).
State = { open = false, unit = false, name = "", visible = false, said = false, rect = false }
-- The controls by name (frame, picture, title, close), for the tests.
Controls = {}

local S = LayoutHelpers.ScaleNumber
-- Measures at the interface's scale 1: the picture; the frame round it as the game's Window lays its client area (5
-- at the sides and the bottom, its title bar of 30 above); the gap to the screen's edge and to the panels below.
local K = { width = 480, height = 270, side = 5, title = 30, edge = 16, above = 8 }
local my = { frame = false, ticker = false }

local function Say(text)
    State.said = text
    LOG("FA Remastered: the follow camera: " .. text)
    import("/lua/ui/game/textdisplay.lua").PrintToScreen({ text = text, size = 16, color = 'ffffd200', duration = 2, location = 'center' })
end

-- A unit's name as the game's panels give it: its own name if the player gave it one, else its blueprint's.
local function NameOf(unit)
    local custom = unit.GetCustomName and unit:GetCustomName()
    if custom and custom ~= "" then return custom end
    local bp = unit:GetBlueprint()
    local general = bp and bp.General
    if general and general.UnitName and general.UnitName ~= "" then return LOC(general.UnitName) end
    return LOC(bp and bp.Description or unit:GetUnitId())
end

-- The top of the game's bottom panels under the picture's place (as the replay timeline finds it: the highest shown
-- one of construction.lua's, unitview.lua's and orders.lua's controls lying in the lower part of the screen and
-- reaching into the picture's width); the screen's bottom without one.
local panelTables = { "/lua/ui/game/construction.lua", "/lua/ui/game/unitview.lua", "/lua/ui/game/orders.lua" }
local function Shows(c)
    local root = GetFrame(0)
    while c and c ~= root do
        if c:IsHidden() or (c.GetAlpha and c:GetAlpha() <= 0) then return false end
        c = c:GetParent()
    end
    return true
end
local function PanelsTop(left, right)
    local root = GetFrame(0)
    local top = root.Bottom()
    local lower = root.Top() + root.Height() * 0.4
    for _, file in panelTables do
        local ok, module = pcall(import, file)
        local controls = ok and module and module.controls
        if type(controls) == 'table' then
            for _, c in controls do
                if type(c) == 'table' and c.IsHidden and c.GetParent and c ~= root and c.Top and type(c.Top) ~= 'number' then
                    local fine, t, b, l, r = pcall(function() return c.Top(), c.Bottom(), c.Left(), c.Right() end)
                    if fine and t > lower and t < top and r > left and l < right and b > t and Shows(c) then top = t end
                end
            end
        end
    end
    return math.floor(top)
end

-- The picture's place: at the bottom right, its frame clear of the screen's edge and of the panels below it.
local function Place()
    local root = GetFrame(0)
    local w, h = S(K.width), S(K.height)
    local right = math.floor(root.Right() - S(K.edge) - S(K.side))
    local left = right - w
    local bottom = math.min(root.Bottom() - S(K.edge), PanelsTop(left - S(K.side), right + S(K.side)) - S(K.above)) - S(K.side)
    local top = math.max(root.Top() + S(K.title), bottom - h)
    return left, math.floor(top), w, math.floor(bottom) - math.floor(top)
end

local function Layout()
    local x, y, w, h = Place()
    local p = Controls.picture
    if not State.rect or State.rect[1] ~= x or State.rect[2] ~= y or State.rect[3] ~= w or State.rect[4] ~= h then
        State.rect = { x, y, w, h }
        p.Left:Set(x)
        p.Top:Set(y)
        p.Right:Set(x + w)
        p.Bottom:Set(y + h)
    end
end

local function Frame()
    if not my.frame then return end
    local st = moho64ui_FollowCamState()
    if not st.on then  -- (the unit is gone and its wreck shown long enough: the engine closed it)
        Close()
        return
    end
    Layout()
    local panelsAway = import("/lua/ui/game/gamemain.lua").gameUIHidden
    local visible = not panelsAway
    if visible ~= State.visible then
        State.visible = visible
        if visible then my.frame:Show() else my.frame:Hide() end
    end
    local name = my.baseName .. (st.lost and " (destroyed)" or "")
    if name ~= State.name then
        State.name = name
        Controls.title:SetText(name)
    end
    local r = State.rect
    moho64ui_FollowCamPlace(r[1], r[2], r[3], r[4], visible)
end

-- The frame: the minimap window's border pieces laid as the game's Window lays them, round the picture (whose middle
-- stays clear: the renderer's picture shows through the interface there).
local function MakeFrame()
    local root = GetFrame(0)
    -- the frame's outer edges round the picture
    local frame = Group(root, "followCamera")
    frame.Depth:Set(root:GetTopmostDepth() + 1)
    frame:DisableHitTest()
    my.frame = frame
    Controls.frame = frame
    local picture = Group(frame, "followCameraPicture")
    picture:DisableHitTest()
    Controls.picture = picture
    State.rect = false
    Layout()
    frame.Left:Set(function() return picture.Left() - S(K.side) end)
    frame.Right:Set(function() return picture.Right() + S(K.side) end)
    frame.Top:Set(function() return picture.Top() - S(K.title) end)
    frame.Bottom:Set(function() return picture.Bottom() + S(K.side) end)
    local outer = frame
    local function Piece(name)
        local b = Bitmap(frame, UIUtil.UIFile('/game/mini-map-brd/mini-map_brd_' .. name .. '.dds'))
        b:DisableHitTest()
        b.Depth:Set(function() return frame.Depth() + 1 end)
        return b
    end
    local tl, tr, bl, br = Piece('ul'), Piece('ur'), Piece('ll'), Piece('lr')
    local tm, bm, ml, mr = Piece('horz_um'), Piece('lm'), Piece('vert_l'), Piece('vert_r')
    tl.Top:Set(outer.Top) tl.Left:Set(outer.Left)
    tr.Top:Set(outer.Top) tr.Right:Set(outer.Right)
    bl.Bottom:Set(outer.Bottom) bl.Left:Set(outer.Left)
    br.Bottom:Set(outer.Bottom) br.Right:Set(outer.Right)
    tm.Left:Set(tl.Right) tm.Right:Set(tr.Left) tm.Top:Set(tl.Top)
    bm.Left:Set(bl.Right) bm.Right:Set(br.Left) bm.Top:Set(bl.Top)
    ml.Left:Set(tl.Left) ml.Top:Set(tl.Bottom) ml.Bottom:Set(bl.Top)
    mr.Right:Set(tr.Right) mr.Top:Set(tr.Bottom) mr.Bottom:Set(br.Top)
    -- the unit's name in the title bar, as the Window's title (its font, colour and place)
    local title = UIUtil.CreateText(frame, "", 14, UIUtil.titleFont)
    title:SetColor(UIUtil.fontColor)
    title:DisableHitTest()
    title.Depth:Set(function() return frame.Depth() + 3 end)
    LayoutHelpers.AtLeftTopIn(title, outer, 20, 7)
    Controls.title = title
    -- the picture's mouse: left jumps the main camera there, right closes (a surface over it that lets the world show)
    local hit = Bitmap(frame)
    hit:SetSolidColor('00000000')
    LayoutHelpers.FillParent(hit, picture)
    hit.Depth:Set(function() return frame.Depth() + 2 end)
    hit.HandleEvent = function(self, event)
        if event.Type == 'ButtonPress' or event.Type == 'ButtonDClick' then
            if event.Modifiers.Right then
                Close()
            elseif event.Modifiers.Left then
                State.jumped = moho64ui_FollowCamJump()
            end
            return true
        end
        return false
    end
    Controls.hit = hit
    -- the Window's close button in the title bar
    local close = Button(frame,
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_up.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_down.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_over.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_dis.dds'),
        "UI_Tab_Click_01", "UI_Tab_Rollover_01")
    LayoutHelpers.AtRightTopIn(close, outer, 10, 5)
    close.Depth:Set(function() return frame.Depth() + 4 end)
    close.OnClick = function(self) Close() end
    Tooltip.AddControlTooltipManual(close, "Close the follow camera", "A left click on the picture moves the camera there; a right click closes it too.", 0)
    Controls.close = close
    -- each frame (on a control that is never hidden: the frame's own updates would stop with it)
    my.ticker = Group(root, "followCameraTicker")
    LayoutHelpers.SetDimensions(my.ticker, 1, 1)
    LayoutHelpers.AtLeftTopIn(my.ticker, root, 0, 0)
    my.ticker:DisableHitTest()
    my.ticker:SetNeedsFrameUpdate(true)
    my.ticker.OnFrame = function(self, delta) Frame() end
end

-- Open on the first unit of the selection; false (and it says why) when nothing opens.
function Open()
    if State.open then Close() end
    local selected = GetSelectedUnits()
    local unit = selected and selected[1]
    if not unit then
        Say("Select a unit to follow first")
        return false
    end
    local ok, why = moho64ui_FollowCamSet(unit:GetEntityId())
    if not ok then
        Say("It cannot follow that unit: " .. tostring(why))
        return false
    end
    State.open = true
    State.unit = unit:GetEntityId()
    State.said = false
    State.visible = true
    State.jumped = nil
    my.baseName = NameOf(unit)
    State.name = false
    MakeFrame()
    Frame()
    return true
end

function Close()
    if moho64ui_FollowCamSet then moho64ui_FollowCamSet(nil) end
    if my.ticker then my.ticker:Destroy() end
    if my.frame then my.frame:Destroy() end
    my.ticker, my.frame = false, false
    Controls = {}
    State.open = false
    State.visible = false
    State.rect = false
end

function Toggle()
    if State.open then Close() else Open() end
end

-- remaster_follow_cam [on|off]: no word turns it over (on again: on the selection as it is now).
function Command(words)
    local word = string.upper(string.gsub(words or "", "%s", ""))
    if word == "ON" or word == "1" or word == "TRUE" then
        Open()
    elseif word == "OFF" or word == "0" or word == "FALSE" then
        Close()
    else
        Toggle()
    end
end
