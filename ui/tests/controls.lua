-- The control tree, checked from the script side: creation, layout through LazyVars, hide and
-- show, hit testing, frame updates, the order of destruction.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/controls.lua --frames 8 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Text = import("/lua/maui/text.lua").Text
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local root = GetFrame(0)
check("root frame covers the head", root.Left() .. "," .. root.Top() .. " " .. root.Right() .. "," .. root.Bottom(), "0,0 1920,1080")
check("root depth", root.Depth(), 0)
check("one root frame", GetNumRootFrames(), 1)

local g = Group(root, "testgroup")
g.Left:Set(100) g.Top:Set(50) g.Width:Set(400) g.Height:Set(300)
check("right and bottom follow left/top + size", g.Right() .. "," .. g.Bottom(), "500,350")
check("depth is the parent's + 1", g.Depth(), 1)
check("name", g:GetName(), "testgroup")
check("parent", g:GetParent() == root, true)
check("root frame of a control", g:GetRootFrame() == root, true)

-- a texture's size for the interface is the file's size less a 1 pixel border on each side
local texture = "/textures/ui/common/scx_menu/large-no-bracket-btn/large_btn_up.dds"
local tw, th = GetTextureDimensions(texture)
check("texture dimensions", tostring(tw) .. "x" .. tostring(th), "358x70")
local bw = GetTextureDimensions(texture, 0)
check("texture dimensions without border", bw, 360)
check("missing texture gives nil", GetTextureDimensions("/textures/ui/no_such_file.dds"), nil)

local log = {}
local b = Bitmap(g, texture, "testbitmap")
b.OnHide = function(self, hidden) table.insert(log, "bitmap hide " .. tostring(hidden)) end
b.OnDestroy = function(self) table.insert(log, "bitmap destroy") Bitmap.OnDestroy(self) end
LayoutHelpers.AtCenterIn(b, g)
check("bitmap takes its texture's size", b.Width() .. "x" .. b.Height(), "358x70")
check("bitmap centred in the group", b.Left() .. "," .. b.Top(), "121,165")

local t = Text(b, "testtext")
t:SetFont("Arial", 14)
t:SetText("Hello")
t.OnDestroy = function(self) table.insert(log, "text destroy") Text.OnDestroy(self) end
check("text", t:GetText(), "Hello")
check("text width is its advance", t.Width(), t.TextAdvance())
check("text height is ascent + descent", t.Height(), math.floor(t.FontAscent() + t.FontDescent()))

-- hiding: OnHide first, then the control and its children
g.OnHide = function(self, hidden) table.insert(log, "group hide " .. tostring(hidden)) end
g:Hide()
check("hide reaches the children", tostring(g:IsHidden()) .. tostring(b:IsHidden()) .. tostring(t:IsHidden()), "truetruetrue")
g:Show()
check("show reaches the children", tostring(g:IsHidden()) .. tostring(b:IsHidden()) .. tostring(t:IsHidden()), "falsefalsefalse")
-- a control whose OnHide returns true keeps its state, and so does everything below it
b.OnHide = function(self, hidden) table.insert(log, "bitmap refuses") return true end
g:Hide()
check("OnHide returning true stops the change", tostring(g:IsHidden()) .. tostring(b:IsHidden()) .. tostring(t:IsHidden()), "truefalsefalse")
g:Show()
check("a new control takes its parent's hidden state", (function()
    g:Hide()
    local late = Group(g)
    local h = late:IsHidden()
    g:Show()
    late:Destroy()
    return h
end)(), true)

check("hit test: left and top edges are inside", g:HitTest(100, 50), true)
check("hit test: right edge is outside", g:HitTest(500, 50), false)
check("hit test: bottom edge is outside", g:HitTest(499, 350), false)
b:DisableHitTest(true)
check("recursive DisableHitTest", tostring(b:IsHitTestDisabled()) .. tostring(t:IsHitTestDisabled()) .. tostring(g:IsHitTestDisabled()), "truetruefalse")

local applied = 0
g:ApplyFunction(function(control) applied = applied + 1 end)
check("ApplyFunction visits the control and everything below", applied, 3)

-- small engine functions, as the original's (read from its exe, 2026-10-16)
check("GetSystemTime: the time since the start as hours:minutes:seconds", string.find(GetSystemTime(), "^%d%d+:%d%d:%d%d$") ~= nil, true)
check("GetUIControlsAlpha starts at 1", GetUIControlsAlpha(), 1)
SetUIControlsAlpha(0.5)
check("SetUIControlsAlpha keeps its factor", GetUIControlsAlpha(), 0.5)
SetUIControlsAlpha(1)
local function opened(url)  -- (what OpenURL would open, without opening it)
    moho64ui_OpenURLDryRun(true)
    OpenURL(url)
    local got = moho64ui_OpenedURL()
    moho64ui_OpenURLDryRun(false)
    return got
end
check("OpenURL opens an http address", opened("http://example.com/x"), "http://example.com/x")
check("not an https one (the original's library knows http, ftp and file only)", opened("https://example.com/x"), "")
check("nor ftp, which the init file's protocols do not name", opened("ftp://example.com/x"), "")
check("nor http without its //", opened("http:example.com"), "")
check("GetArmyScore without a session: the original's error", pcall(GetArmyScore, 1), false)
check("SessionCanRestart without a session: an error", pcall(SessionCanRestart), false)
check("GetSpecialFolder: the replays' folder", string.sub(GetSpecialFolder("SFT_Replay"), -8), "replays/")
check("the saved games', the case not counting", string.sub(GetSpecialFolder("savegame"), -10), "savegames/")
check("another kind is an error", pcall(GetSpecialFolder, "Movie"), false)

local frames = 0
g.OnFrame = function(self, elapsed) frames = frames + 1 end
g:SetNeedsFrameUpdate(true)
g:Hide()  -- hidden controls still get frame updates
log = {}
ForkThread(function()
    WaitFrames(3)
    check("OnFrame runs once per frame, hidden or not", frames, 3)
    check("topmost depth", root:GetTopmostDepth(), 3)
    g:Destroy()
    check("destroy order: the control first, then its children", table.concat(log, ", "), "bitmap destroy, text destroy")
    check("a destroyed control lives until the frame ends", IsDestroyed(g), false)
    check("a destroyed control has left its parent", g:GetParent(), nil)
    g:Destroy()  -- allowed
    WaitFrames(2)
    check("after the frame it is gone", tostring(IsDestroyed(g)) .. tostring(IsDestroyed(b)) .. tostring(IsDestroyed(t)), "truetruetrue")
    check("Destroy on a destroyed control is no error", pcall(g.Destroy, g), true)
    check("other methods on a destroyed control are errors", pcall(g.Hide, g), false)
    check("the root frame cannot be destroyed", pcall(root.Destroy, root), false)

    -- a bitmap laid out turned over (its Top below its Bottom, or its Left right of its Right) still
    -- covers the strip between its edges: the original draws its quad as the corners are given, without
    -- culling (FA exe 0x780850, DRAW_ClippedQuad 0x454e20); FAF's score graph lays its shares out so
    local function strip(l, t, r, bottom)
        local s = Bitmap(root)
        s:SetSolidColor("ff20c040")
        s.Left:Set(l) s.Top:Set(t) s.Right:Set(r) s.Bottom:Set(bottom)
        return s
    end
    local strips = {strip(1500, 980, 1520, 1000), strip(1540, 1000, 1560, 980), strip(1600, 980, 1580, 1000)}
    check("a bitmap draws its colour", moho64ui_PixelAt(1510, 990), "ff20c040")
    check("turned upside down: between its edges", moho64ui_PixelAt(1550, 990), "ff20c040")
    check("turned left for right: between its edges", moho64ui_PixelAt(1590, 990), "ff20c040")
    check("and nothing beside them", moho64ui_PixelAt(1530, 990), "00000000")
    for _, s in strips do s:Destroy() end

    -- options: the current profile's, whole or one by its key (the second form is FAF's)
    local Prefs = import("/lua/user/prefs.lua")
    local profile = GetPreference("profile")
    if profile and profile.current and profile.profiles[profile.current] then
        local saved = Prefs.GetFromCurrentProfile("options")
        Prefs.SetToCurrentProfile("options", {moho64ui_test_option = 7})
        check("GetOptions() gives the profile's options", type(GetOptions()) == "table" and GetOptions().moho64ui_test_option, 7)
        check("GetOptions(key) gives one option", GetOptions("moho64ui_test_option"), 7)
        check("an option that is not set is nil", GetOptions("moho64ui_no_such_option"), nil)
        Prefs.SetToCurrentProfile("options", saved)
    end
    check("ExitApplication is the engine's", type(ExitApplication), "cfunction")
    LOG("TEST done: " .. failed .. " failed")
end)
