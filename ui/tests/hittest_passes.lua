-- Hit testing by a picture's transparency and the render passes, as in the original (FA exe 2026-10-19).
-- A bitmap with UseAlphaHitTest(true) (CMauiBitmap::HitTest 0x77ff70) is hit only where its texture's pixel at
-- (x - Left, y - Top), cut toward zero, plus the texture's border, has an alpha that is not 0: the place in the
-- control's own pixels, also when the bitmap is stretched; the frame shown now; a solid colour by its alpha. A click
-- on a transparent part goes to what lies under it. The render passes (a set of bits; FAF's uiutil.lua: 1
-- UIRP_UnderWorld, 4 UIRP_Glow, 8 UIRP_PostGlow): the root frame's is 8 and a new control takes its parent's; pass 1
-- draws first, then the world, then pass 4, then pass 8, each by Depth (WRenViewport::Render 0x7f90d0, DoRender
-- 0x7870f0), so a later pass draws over an earlier one whatever their Depths; a control of none of those passes is
-- not drawn, one of several draws in each; hit testing goes by Depth alone (0x7872e0).
-- In a game: the passes FAF's scripts give (the main view 1 + 8, the minimap and its window's middle 1) and the
-- minimap's window middle under the world (not over the minimap); FAF's construction tab (construction.lua
-- CreateTab asks UseAlphaHitTest(true)) clicked on its round picture and beside it. In the front end also the picture
-- formats D3DX reads that FAF and the frozen mods use besides DDS and PNG: a .tga, a .bmp, a .bmp named .dds.
--   front end: moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/hittest_passes.lua --frames 8
--   a game:    moho64ui --init <init.lua> --stage setup --size 1920x1080 --session <session_ai_cheats.lua> --seed 7
--              --game-lua ui/tests/hittest_passes.lua --frames 900 (with the world's picture)
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Group = import("/lua/maui/group.lua").Group

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function name(c) return c and (c:GetName() or "?") or "nil" end

-- FAF's construction tab picture (UEF), 32x32 with a border of 1: transparent all round a ring; its file pixel
-- (16, 16) is opaque, (1, 1) and (12, 16) (the hole inside the ring) are transparent, (16, 8) opaque, (31, 16) not.
local tab = "/textures/ui/uef/game/construct-tech_btn/t1_btn_up.dds"
local opaque = "/textures/ui/common/widgets/bar_sbr.dds"  -- 32x32, alpha 255 everywhere
local root = GetFrame(0)

local function solid(color, l, t, w, h, depth, label)
    local b = Bitmap(root)
    b:SetSolidColor(color)
    b.Left:Set(l) b.Top:Set(t) b.Width:Set(w) b.Height:Set(h)
    b.Depth:Set(depth)
    b:SetName(label)
    return b
end

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    ForkThread(function()
        WaitFrames(2)
        -- 1. hit testing by transparency
        local under = solid("ff00ff00", 280, 280, 400, 120, 10, "under")
        local top = Bitmap(root, tab)
        top:SetName("tab")
        top.Left:Set(300) top.Top:Set(300) top.Width:Set(30) top.Height:Set(30) top.Depth:Set(20)
        check("without the alpha test the whole rectangle is the bitmap's", name(moho64ui_ControlAt(300.5, 300.5)), "tab")
        top:UseAlphaHitTest(true)
        check("with it: a transparent corner goes to what is under it", name(moho64ui_ControlAt(300.5, 300.5)), "under")
        check("an opaque pixel is the bitmap's", name(moho64ui_ControlAt(315.5, 315.5)), "tab")
        check("the place is cut toward zero (315.9 is pixel 15)", name(moho64ui_ControlAt(315.9, 315.9)), "tab")
        check("the transparent hole inside the ring goes under too", name(moho64ui_ControlAt(311.5, 315.5)), "under")
        check("the script's HitTest asks the picture too: transparent", top:HitTest(300.5, 300.5), false)
        check("and opaque", top:HitTest(315.5, 315.5), true)
        check("outside the rectangle no hit at all", top:HitTest(299.5, 315.5), false)
        -- stretched to twice its size: the place stays in the control's pixels (not scaled to the texture)
        local wide = Bitmap(root, tab)
        wide:SetName("wide")
        wide.Left:Set(400) wide.Top:Set(300) wide.Width:Set(60) wide.Height:Set(60) wide.Depth:Set(20)
        wide:UseAlphaHitTest(true)
        check("stretched: pixel (15, 15) of the control asks the texture's (16, 16): a hit", name(moho64ui_ControlAt(415.5, 315.5)), "wide")
        check("stretched: (30, 15) asks (31, 16), transparent (scaled it would be opaque)", name(moho64ui_ControlAt(430.5, 315.5)), "under")
        -- the frame shown now: the ring's picture, then an opaque one
        local anim = Bitmap(root)
        anim:SetTexture({tab, opaque})
        anim:SetName("anim")
        anim.Left:Set(500) anim.Top:Set(300) anim.Width:Set(30) anim.Height:Set(30) anim.Depth:Set(20)
        anim:UseAlphaHitTest(true)
        check("an animation's first frame: its corner is transparent", name(moho64ui_ControlAt(500.5, 300.5)), "under")
        anim:SetFrame(1)
        check("the second frame is opaque there", name(moho64ui_ControlAt(500.5, 300.5)), "anim")
        -- a solid colour by its alpha
        local clear = solid("00ffffff", 600, 300, 30, 30, 20, "clear")
        check("a clear solid colour without the test is hit", name(moho64ui_ControlAt(610, 310)), "clear")
        clear:UseAlphaHitTest(true)
        check("with it, its alpha 0 is a miss", name(moho64ui_ControlAt(610, 310)), "under")
        clear:SetSolidColor("01ffffff")
        check("any alpha above 0 is a hit", name(moho64ui_ControlAt(610, 310)), "clear")
        -- a click on the transparent part goes to what is under it; on the picture to the bitmap
        local pressed = {}
        under.HandleEvent = function(self, e) if e.Type == "ButtonPress" then table.insert(pressed, "under") end return true end
        top.HandleEvent = function(self, e) if e.Type == "ButtonPress" then table.insert(pressed, "tab") end return true end
        moho64ui_MouseButton("Left", true, 300.5, 300.5) moho64ui_MouseButton("Left", false, 300.5, 300.5)
        moho64ui_MouseButton("Left", true, 315.5, 315.5) moho64ui_MouseButton("Left", false, 315.5, 315.5)
        check("a press on the corner reaches the control under it, one on the picture the bitmap", table.concat(pressed, " "), "under tab")
        top:UseAlphaHitTest(false)
        check("switched off again: the whole rectangle", name(moho64ui_ControlAt(300.5, 300.5)), "tab")
        for _, c in {under, top, wide, anim, clear} do c:Destroy() end
        moho64ui_MouseMove(5, 5)

        -- 2. render passes
        check("the root frame draws in pass 8 (UIRP_PostGlow)", root:GetRenderPass(), 8)
        local g = Group(root)
        check("a new control takes its parent's pass", g:GetRenderPass(), 8)
        local before = Group(g)
        g:SetRenderPass(1)
        local after = Group(g)
        check("a child made after its parent's pass changed takes the new one", after:GetRenderPass(), 1)
        check("one made before keeps its own", before:GetRenderPass(), 8)
        g:Destroy()
        local a = solid("ffff0000", 1000, 500, 40, 40, 100, "late")    -- pass 8, Depth 100
        local b = solid("ff00ff00", 1000, 500, 40, 40, 200, "early")   -- pass 1, Depth 200
        b:SetRenderPass(1)
        WaitFrames(1)
        check("pass 8 draws over pass 1 whatever the Depths", moho64ui_PixelAt(1020, 520), "ffff0000")
        check("hit testing goes by Depth alone: the pass 1 control is the one under the mouse", name(moho64ui_ControlAt(1020, 520)), "early")
        a:Hide()
        check("pass 1 draws where nothing of a later pass lies over it", moho64ui_PixelAt(1020, 520), "ff00ff00")
        local c = solid("ff0000ff", 1000, 500, 40, 40, 50, "glow")     -- pass 4, Depth 50
        c:SetRenderPass(4)
        check("pass 4 draws over pass 1 (Depth 50 over 200)", moho64ui_PixelAt(1020, 520), "ff0000ff")
        a:Show()
        check("and under pass 8 (Depth 100 over 50)", moho64ui_PixelAt(1020, 520), "ffff0000")
        a:Hide()
        c:SetRenderPass(0)
        check("a control of no pass is not drawn", moho64ui_PixelAt(1020, 520), "ff00ff00")
        c:SetRenderPass(2)
        check("nor one of pass 2 alone (UIRP_World: no pass draws it)", moho64ui_PixelAt(1020, 520), "ff00ff00")
        c:SetRenderPass(5)
        check("one of passes 1 and 4 draws in both: blue over green", moho64ui_PixelAt(1020, 520), "ff0000ff")
        -- a half transparent colour of passes 1 and 8 draws twice: denser than the same of pass 8 alone
        local once = solid("80ff8000", 1100, 500, 20, 20, 10, "once")
        local twice = solid("80ff8000", 1140, 500, 20, 20, 10, "twice")
        twice:SetRenderPass(9)
        local p1, p2 = moho64ui_PixelAt(1110, 510), moho64ui_PixelAt(1150, 510)
        check("drawn twice (passes 1 and 8) is not drawn once (" .. tostring(p1) .. ", " .. tostring(p2) .. ")", p1 ~= p2 and p1 ~= "00000000" and p2 ~= "00000000", true)
        for _, x in {a, b, c, once, twice} do x:Destroy() end

        -- 3. texture formats the original reads through D3DX (by content, not name): FAF's one .tga (a cursor),
        -- a mod's .bmp icon, a mod's build icon that is a .bmp named .dds; drawn one texel a pixel, border 1
        local function picture(file, l, t, w, h)
            local p = Bitmap(root, file)
            p.Left:Set(l) p.Top:Set(t) p.Width:Set(w) p.Height:Set(h) p.Depth:Set(10)
            return p
        end
        local tga = picture("/textures/ui/common/game/cursors/reclaim-disabled.tga", 700, 600, 30, 30)
        local bmp = picture("/mods/BlackOpsFAF-Unleashed/icon.bmp", 750, 600, 62, 62)
        local named = picture("/mods/Antares Unit Pack/icons/units/GMAL307_icon.dds", 830, 600, 62, 62)
        check("a .tga: its pixel (16, 16)", moho64ui_PixelAt(715, 615), "ff6b6b6b")
        check("its rows from the bottom: (16, 10)", moho64ui_PixelAt(715, 609), "ff292929")
        check("its alpha: (1, 1) transparent", moho64ui_PixelAt(700, 600), "00000000")
        check("a .bmp: its pixel (11, 11)", moho64ui_PixelAt(760, 610), "ff636558")
        check("and (32, 32)", moho64ui_PixelAt(781, 631), "ffced8bd")
        check("a .bmp named .dds: its pixel (11, 11)", moho64ui_PixelAt(840, 610), "ff9e835e")
        tga:UseAlphaHitTest(true)
        check("the .tga's alpha in the hit test too: (1, 1) is a miss", moho64ui_ControlAt(700.5, 600.5) == tga, false)
        check("(16, 16) a hit", moho64ui_ControlAt(715.5, 615.5) == tga, true)
        for _, x in {tga, bmp, named} do x:Destroy() end
        LOG("TEST done: " .. failed .. " failed")
    end)
else
    ForkThread(function()
        WaitSeconds(4)
        -- 1. the passes FAF's scripts give
        check("in a game the root frame draws in pass 8", root:GetRenderPass(), 8)
        local wv = import("/lua/ui/game/worldview.lua")
        check("FAF's main world view: passes 1 and 8 (worldview.lua: UnderWorld + PostGlow)", wv.viewLeft and wv.viewLeft:GetRenderPass(), 9)
        local minimap = import("/lua/ui/game/minimap.lua")
        local shownAtFirst = minimap.controls.displayGroup and not minimap.controls.displayGroup:IsHidden()
        if not shownAtFirst then minimap.ToggleMinimap() end
        WaitFrames(3)
        local mini, middle = minimap.controls.miniMap, minimap.controls.displayGroup.window_m
        check("the minimap: pass 1", mini:GetRenderPass(), 1)
        check("its window's middle: pass 1", middle:GetRenderPass(), 1)
        check("the window's other parts keep the frame's pass 8", minimap.controls.displayGroup.window_tl:GetRenderPass(), 8)
        -- the window's middle (half transparent) lies under the world: over the minimap it changes nothing
        local x = math.floor((math.max(middle.Left(), mini.Left()) + math.min(middle.Right(), mini.Right())) / 2)
        local y = math.floor((math.max(middle.Top(), mini.Top()) + math.min(middle.Bottom(), mini.Bottom())) / 2)
        check("a point inside the minimap and the window's middle", x > mini.Left() and x < mini.Right() and y > middle.Top() and y < middle.Bottom(), true)
        local shown = moho64ui_PixelAt(x, y)
        middle:Hide()
        local hidden = moho64ui_PixelAt(x, y)
        check("the window's middle (pass 1) does not show over the minimap (" .. tostring(shown) .. ")", shown, hidden)
        middle:Show()
        middle:SetRenderPass(8)
        local over = moho64ui_PixelAt(x, y)
        check("of pass 8 it would lie over it (" .. tostring(over) .. ")", over ~= hidden, true)
        middle:SetRenderPass(1)
        if not shownAtFirst then minimap.ToggleMinimap() end

        -- 2. FAF's construction tab: CreateTab asks UseAlphaHitTest(true) (its layout switches it off again later)
        local construction = import("/lua/ui/game/construction.lua")
        local under = solid("ff00ff00", 590, 390, 60, 60, 1000, "under")
        local t = construction.CreateTab(root, nil, nil)
        t:SetName("construction tab")
        t:SetNewTextures(tab, "/textures/ui/uef/game/construct-tech_btn/t1_btn_selected.dds",
            "/textures/ui/uef/game/construct-tech_btn/t1_btn_over.dds", "/textures/ui/uef/game/construct-tech_btn/t1_btn_down.dds",
            "/textures/ui/uef/game/construct-tech_btn/t1_btn_dis.dds", "/textures/ui/uef/game/construct-tech_btn/t1_btn_dis.dds")
        t.Left:Set(600) t.Top:Set(400) t.Width:Set(30) t.Height:Set(30) t.Depth:Set(1010)
        WaitFrames(1)
        check("the tab's transparent corner goes to what is under it", name(moho64ui_ControlAt(600.5, 400.5)), "under")
        check("its picture is the tab", name(moho64ui_ControlAt(615.5, 415.5)), "construction tab")
        local pressed = 0
        under.HandleEvent = function(self, e) if e.Type == "ButtonPress" then pressed = pressed + 1 end return true end
        moho64ui_MouseButton("Left", true, 600.5, 400.5) moho64ui_MouseButton("Left", false, 600.5, 400.5)
        check("a click on the corner: the tab stays unchecked", t:IsChecked(), false)
        check("and the control under it hears the press", pressed, 1)
        moho64ui_MouseMove(615.5, 415.5)
        moho64ui_MouseButton("Left", true, 615.5, 415.5) moho64ui_MouseButton("Left", false, 615.5, 415.5)
        check("a click on its picture checks it (FAF's OnClick)", t:IsChecked(), true)
        check("not heard under it", pressed, 1)
        t:UseAlphaHitTest(false)  -- (what FAF's construction layouts do in SetupTab)
        check("with the test off (FAF's layout) the corner is the tab's again", name(moho64ui_ControlAt(600.5, 400.5)), "construction tab")
        moho64ui_MouseMove(5, 300)
        t:Destroy() under:Destroy()
        LOG("TEST done: " .. failed .. " failed")
    end)
end
