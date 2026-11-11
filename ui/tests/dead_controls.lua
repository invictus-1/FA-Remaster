-- A control destroyed earlier in the same frame is still there until the frame's end, and the original lets scripts
-- go on using it (FA exe: Destroy 0x786ef0, the constructor 0x7867b0, SetParent 0x786ad0, MAUI_SetKeyboardFocus
-- 0x79cc10, the capture stack 0x7a4540; "dead", +0xec, is tested only where controls are drawn and given frame steps):
-- it can be destroyed again, be made a parent, be moved, take the keyboard focus and the input capture. What must hold
-- is that nothing points at it once it is deleted (the review's toolkit H4): its late children go with it, a control
-- moved under another parent stays, and the focus and the capture are nobody's after the frame.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/dead_controls.lua --frames 20 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local Edit = import("/lua/maui/edit.lua").Edit

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
-- a group (laid out under the mouse of the events below) that counts its OnDestroy calls in `.destroys`
local function counted(parent, name)
    local g = Group(parent, name)
    g.Left:Set(800) g.Top:Set(400) g.Width:Set(200) g.Height:Set(200)
    g.destroys = 0
    g.OnDestroy = function(self) self.destroys = self.destroys + 1 end
    return g
end
local function gone(...)
    local s = ""
    for i = 1, arg.n do s = s .. (IsDestroyed(arg[i]) and "y" or "n") end
    return s
end

local root = GetFrame(0)
ForkThread(function()
    -- destroyed twice in one frame
    local twice = counted(root, "twice")
    twice:Destroy()
    check("Destroy: OnDestroy", twice.destroys, 1)
    twice:Destroy()
    check("destroyed again in the same frame: OnDestroy again, as the original's", twice.destroys, 2)

    -- made under a destroyed parent
    local parent = counted(root, "deadparent")
    parent:Destroy()
    local made, child = pcall(counted, parent, "lateChild")
    check("a control can be made under a parent destroyed in this frame", made, true)
    local grandchild = counted(child, "lateGrandchild")
    check("it has that parent", child:GetParent() == parent, true)
    check("and all three are still there", gone(parent, child, grandchild), "nnn")

    -- moved: a destroyed control under a living parent, a living control under a destroyed parent
    local home = counted(root, "home")
    local moved = counted(root, "moved")
    moved:Destroy()
    moved:SetParent(home)
    check("a destroyed control moved under another parent has that parent", moved:GetParent() == home, true)
    local doomed = counted(root, "doomed")
    local guest = counted(root, "guest")
    local guestChild = counted(guest, "guestChild")
    doomed:Destroy()
    guest:SetParent(doomed)
    check("a control moved under a destroyed parent has that parent", guest:GetParent() == doomed, true)

    -- the keyboard focus and the input capture taken after Destroy
    local keeper = counted(root, "keeper")
    keeper:AcquireKeyboardFocus(false)
    local changes = 0
    keeper.OnKeyboardFocusChange = function(self) changes = changes + 1 end
    local ghost = counted(root, "ghost")
    ghost:Destroy()
    ghost:AcquireKeyboardFocus(false)
    check("a destroyed control takes the keyboard focus for the rest of the frame", root:GetCurrentFocusControl() == ghost, true)
    check("the control that had it hears of it", changes, 1)
    AddInputCapture(ghost)
    AddInputCapture(ghost)
    check("and the input capture", GetInputCapture() == ghost, true)
    -- (a key, Home, and the mouse while it has them)
    moho64ui_Key(315)
    moho64ui_MouseMove(900, 500)

    WaitFrames(2)

    check("after the frame the twice destroyed control is gone", gone(twice), "y")
    check("the destroyed parent is gone, and the child made under it and its child with it", gone(parent, child, grandchild), "yyy")
    check("deleted with the parent, not destroyed: no OnDestroy for them", child.destroys + grandchild.destroys, 0)
    check("Destroy on such a child is no error", pcall(child.Destroy, child), true)
    check("other methods on it are", pcall(child.Hide, child), false)
    check("the destroyed control moved under a living parent is still there", gone(home, moved), "nn")
    check("with its parent", moved:GetParent() == home, true)
    check("the control moved under a destroyed parent is gone with it, and its child", gone(doomed, guest, guestChild), "yyy")
    check("again with no OnDestroy", guest.destroys + guestChild.destroys, 0)
    check("the deleted control has the keyboard focus no more: nobody has", root:GetCurrentFocusControl(), nil)
    check("nor the input capture", tostring(GetInputCapture()) .. " " .. tostring(AnyInputCapture()), "nil false")
    -- (keys and the mouse now: with a pointer to the deleted control left behind these would use freed memory)
    moho64ui_Key(315)
    moho64ui_MouseMove(910, 510)
    moho64ui_MouseButton("Left", true, 910, 510)
    moho64ui_MouseButton("Left", false, 910, 510)

    -- the moved control goes when its new parent's children are destroyed
    home:Destroy()
    check("its new parent destroyed: OnDestroy a second time for the moved control", moved.destroys, 2)
    -- an edit field: AcquireFocus after Destroy, and a late child of a late child destroyed by hand
    local edit = Edit(root, "deadEdit")
    edit:Destroy()
    edit:AcquireFocus()
    check("a destroyed edit field takes the focus for the rest of the frame", root:GetCurrentFocusControl() == edit, true)
    local late = counted(edit, "late")
    local later = counted(late, "later")
    later:Destroy()
    check("a late child destroyed by hand: OnDestroy", later.destroys, 1)
    keeper:Destroy()

    WaitFrames(2)

    check("after the next frame: the parent and the moved control gone", gone(home, moved), "yy")
    check("the edit field and its late children gone", gone(edit, late, later), "yyy")
    check("the one destroyed by hand heard it once, the other not", later.destroys .. " " .. late.destroys, "1 0")
    check("and nobody has the focus", root:GetCurrentFocusControl(), nil)
    moho64ui_Key(315)
    moho64ui_MouseMove(920, 520)
    LOG("TEST done: " .. failed .. " failed")
end)
