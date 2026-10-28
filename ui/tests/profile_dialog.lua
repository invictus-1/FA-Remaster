-- The game's profile dialog (FAF's /lua/ui/dialogs/profile.lua) worked by the mouse and the keys, as the player does:
-- a profile created by typing its name, a second with the same name refused, one chosen from the list, one deleted
-- (the dialog's "Are you sure", then "No" to the recycle bin), the dialog closed with OK. Also RemoveProfileDirectories
-- with names that are no profile's folder (nothing is removed). Runs on a copy of the preferences (the suite's).
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/profile_dialog.lua --frames 400 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local UIUtil = import("/lua/ui/uiutil.lua")
local Prefs = import("/lua/user/prefs.lua")

-- the buttons the dialogs make, by their label's key (a click goes to the newest one still there)
-- (UIUtil.QuickDialog's are CreateButtonWithDropshadow's: the module's functions call each other through its table)
local buttons = {}
local function Recording(make)
    return function(parent, filename, label, ...)
        local b = make(parent, filename, label, unpack(arg))
        if type(label) == 'string' then
            local key = string.gsub(label, "^<LOC ([^>]*)>.*$", "%1")
            buttons[key] = buttons[key] or {}
            table.insert(buttons[key], b)
        end
        return b
    end
end
local GameCreateButtonStd, GameCreateButtonWithDropshadow = UIUtil.CreateButtonStd, UIUtil.CreateButtonWithDropshadow
UIUtil.CreateButtonStd = Recording(GameCreateButtonStd)
UIUtil.CreateButtonWithDropshadow = Recording(GameCreateButtonWithDropshadow)

local function ClickAt(x, y)
    moho64ui_MouseMove(x, y)
    WaitFrames(1)
    moho64ui_MouseButton("Left", true, x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(2)
end
local function Click(key)
    local b
    for _, one in buttons[key] or {} do
        if not IsDestroyed(one) then b = one end
    end
    if not b then
        failed = failed + 1
        LOG("TEST FAIL no button " .. key)
        return
    end
    ClickAt((b.Left() + b.Right()) / 2, (b.Top() + b.Bottom()) / 2)
end
local function Names()
    local names = {}
    for _, p in GetPreference("profile.profiles") or {} do table.insert(names, p.Name) end
    return names
end

ForkThread(function()
    WaitFrames(10)
    local before = Names()
    local n0 = table.getn(before)
    LOG("TEST note: " .. n0 .. " profile(s) at the start, current " .. tostring(GetPreference("profile.current")))
    local closed = false
    local panel = import("/lua/ui/dialogs/profile.lua").CreateDialog(function() closed = true end)
    WaitFrames(3)
    check("the dialog is up", panel ~= nil and not IsDestroyed(panel), true)

    -- a new profile: Create, its name typed into the field (which has the keys at once), Enter
    Click("PROFILE_0004")
    WaitFrames(2)
    moho64ui_Type("ProfTestA")
    moho64ui_Key(13)
    WaitFrames(3)
    local names = Names()
    check("Create, a name typed and Enter make a profile", table.getn(names), n0 + 1)
    check("by the name typed", names[n0 + 1], "ProfTestA")
    check("the profile dialog is shown again", not IsDestroyed(panel) and not panel:IsHidden(), true)

    -- the same name again (other case): refused with the game's message, nothing made
    Click("PROFILE_0004")
    WaitFrames(2)
    moho64ui_Type("proftesta")
    moho64ui_Key(13)
    WaitFrames(3)
    check("a name in use is refused", table.getn(Names()), n0 + 1)
    Click("PROFILE_0011")  -- the message's OK
    WaitFrames(2)
    moho64ui_Key(27)       -- and the name's dialog left with Escape
    WaitFrames(3)
    check("Escape leaves the name's dialog", not panel:IsHidden(), true)
    check("still nothing more made", table.getn(Names()), n0 + 1)

    -- chosen from the list: a click on its row
    local list = moho64ui_ControlAt(panel.Left() + 60, panel.Top() + 80)
    check("the list is under the panel's top left", list and list.GetRowHeight ~= nil, true)
    if list and list.GetRowHeight then
        local h = list:GetRowHeight()
        if n0 >= 1 then
            ClickAt(list.Left() + 40, list.Top() + (n0 - 1) * h + h / 2)
            check("a click on a row makes it current", GetPreference("profile.current"), n0)
        end
        ClickAt(list.Left() + 40, list.Top() + n0 * h + h / 2)
        check("a click on the new one's row makes it current", GetPreference("profile.current"), n0 + 1)
        check("and the list shows it chosen", list:GetSelection(), n0)
    end

    -- deleted: Delete, "Are you sure?" Yes, then No to the recycle bin
    Click("PROFILE_0006")
    WaitFrames(3)
    Click("_Yes")
    WaitFrames(3)
    names = Names()
    check("Delete and Yes remove the current profile", table.getn(names), n0)
    local gone = true
    for _, n in names do if n == "ProfTestA" then gone = false end end
    check("the one removed is ProfTestA", gone, true)
    check("the first profile is current then", GetPreference("profile.current"), 1)
    Click("_No")
    WaitFrames(3)

    -- closed with OK
    Click("PROFILE_0007")
    WaitFrames(3)
    check("OK closes the dialog", IsDestroyed(panel), true)
    check("and calls its exit", closed, true)
    check("the profiles are those of the start", table.concat(Names(), "|"), table.concat(before, "|"))

    -- RemoveProfileDirectories with names that are no profile's folder: nothing removed, no error
    check("an empty name is refused quietly", pcall(RemoveProfileDirectories, ""), true)
    check("a name with .. is refused quietly", pcall(RemoveProfileDirectories, ".."), true)
    check("a name with a separator is refused quietly", pcall(RemoveProfileDirectories, "a/b"), true)
    check("a profile without folders: nothing to do", pcall(RemoveProfileDirectories, "NoSuchProfileHere"), true)
    check("its arguments are checked", pcall(RemoveProfileDirectories), false)

    UIUtil.CreateButtonStd, UIUtil.CreateButtonWithDropshadow = GameCreateButtonStd, GameCreateButtonWithDropshadow
    LOG("TEST done: " .. failed .. " failed")
end)
