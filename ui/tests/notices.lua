-- The main menu's notice line (ours; ui/user/notices.h, ui/data/hook/lua/ui/menus/main.lua): what failed or fell back
-- is said on the menu in one box with the log's name, until the player clicks it or leaves the menu.
--   moho64ui --stage frontend --lua ui/tests/notices.lua --lua-again --frames 4000 --log out.log
-- The file runs twice: in the first front end (the list, the box, the click, then a game launched that cannot load),
-- and again in the front end that comes back after it (--lua-again): the menu itself shows why.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local main = import("/lua/ui/menus/main.lua")
local function has(list, part)
    for _, text in list or {} do
        if string.find(text, part, 1, true) then return true end
    end
    return false
end
if not GetPreference('notices_test_second') then
    check("nothing to say at a start without trouble", moho64ui_Notices(), nil)
    check("and the menu shows no box", main.RemasterMenuNotice.box, false)
    moho64ui_AddNoticeForTests("The game could not start: a reason made up for this test.")
    moho64ui_AddNoticeForTests("The game could not start: a reason made up for this test.")
    moho64ui_AddNoticeForTests("A second sentence, long enough that it must be wrapped into more than one line of the box: "
        .. "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen.")
    local notices = moho64ui_Notices()
    check("the same sentence twice is kept once", notices and table.getn(notices), 2)
    check("the run's log is named", type(notices and notices.log), 'string')
    check("the menu's box is made from them", main.RemasterShowNotices(GetFrame(0)), true)
    local box = main.RemasterMenuNotice.box
    check("it is there", box ~= false and not IsDestroyed(box), true)
    check("with the first sentence", has(main.RemasterMenuNotice.texts, "a reason made up for this test"), true)
    check("the long one wrapped (more rows than sentences and the log's two)", table.getn(main.RemasterMenuNotice.texts) > 4, true)
    check("and the log's name under them", has(main.RemasterMenuNotice.texts, notices.log), true)
    check("the box is as wide as its widest row, inside the head", box.Width() > 100 and box.Right() < GetFrame(0).Right(), true)
    box:HandleEvent({ Type = 'ButtonPress' })
    check("a click hides it", main.RemasterMenuNotice.box, false)
    check("and the sentences are gone", moho64ui_Notices(), nil)
    -- the box goes with its parent, as with the menu when the player moves on
    local Group = import("/lua/maui/group.lua").Group
    local screen = Group(GetFrame(0))
    import("/lua/maui/layouthelpers.lua").FillParent(screen, GetFrame(0))
    moho64ui_AddNoticeForTests("A sentence for a box whose parent goes.")
    check("a box in a group of its own", main.RemasterShowNotices(screen) and main.RemasterMenuNotice.box:GetParent() == screen, true)
    screen:Destroy()
    check("the group gone: the box and the sentences with it", tostring(main.RemasterMenuNotice.box) .. " " .. tostring(moho64ui_Notices()), "false nil")
    -- a game that cannot load: its map file is not there
    SetPreference('notices_test_second', true)
    local scenario = import("/lua/ui/maputil.lua").LoadScenario("/maps/X1CA_001/X1CA_001_scenario.lua")
    local info = import("/lua/singleplayerlaunch.lua").SetupCampaignSession(scenario, 2, 1,
        {opKey = "X1CA_001", campaignID = "uef", difficulty = 2}, false)
    info.scenarioInfo.map = "/maps/X1CA_001/no_such_map.scmap"
    local ok, err = pcall(LaunchSinglePlayerSession, info)
    LOG("TEST note the launch: " .. tostring(ok) .. " " .. tostring(err))
    if failed > 0 then LOG("TEST done: " .. failed .. " failed (in the first front end)") end
else
    local notices = moho64ui_Notices()
    LOG("TEST note back in the front end: " .. tostring(notices and notices[1]))
    check("back in the front end: the host says why the game did not start", has(notices, "The game could not start: "), true)
    local box = main.RemasterMenuNotice.box
    check("and the menu shows it without being asked", box ~= false and not IsDestroyed(box) and has(main.RemasterMenuNotice.texts, "The game could not start: "), true)
    check("the box is the menu's own child (it goes with the menu)", box and box:GetParent() ~= GetFrame(0), true)
    -- a notice that comes while the menu is up is shown by itself, with the one already there
    ForkThread(function()
        moho64ui_AddNoticeForTests("A second thing went wrong while the menu was up.")
        local waited = 0
        while waited < 8 and not has(main.RemasterMenuNotice.texts, "A second thing went wrong") do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        check("a notice that comes while the menu is up is shown by itself", has(main.RemasterMenuNotice.texts, "A second thing went wrong"), true)
        check("with the one already there", has(main.RemasterMenuNotice.texts, "The game could not start: "), true)
        -- (the box is left up: the run's --screenshot shows it)
        LOG("TEST done: " .. failed .. " failed")
    end)
end
