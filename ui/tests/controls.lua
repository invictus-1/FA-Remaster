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
