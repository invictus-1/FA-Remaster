-- The campaign from its own screens: the operation briefing (FAF's operationbriefing.lua with our
-- hook) opened as the operation select opens it (no parent: the original's briefing), its Launch
-- button pressed; the operation starts with the briefing's choices. The same file serves both states:
--   moho64ui --stage frontend --lua ui/tests/briefing.lua --game-lua ui/tests/briefing.lua --frames 1500 ...
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- catch the briefing's Launch button as it is made (as our hook does)
    local UIUtil = import("/lua/ui/uiutil.lua")
    local make = UIUtil.CreateButtonStd
    local launch = nil
    UIUtil.CreateButtonStd = function(p, f, label, a, b, c, d, e, g)
        local button = make(p, f, label, a, b, c, d, e, g)
        if label == "<LOC opbrief_0003>Launch" then launch = button end
        return button
    end
    local ok, err = pcall(import("/lua/ui/campaign/operationbriefing.lua").CreateUI, nil, "/maps/X1CA_001/X1CA_001_scenario.lua")
    UIUtil.CreateButtonStd = make
    if not ok then LOG("TEST note: " .. tostring(err)) end
    check("the operation briefing opens", ok, true)
    check("it has a Launch button", launch ~= nil, true)
    check("and the button is on", launch ~= nil and not launch._isDisabled, true)
    ForkThread(function()
        WaitSeconds(1)
        if launch then launch.OnClick(launch) end
        if failed > 0 then LOG("TEST done: " .. failed .. " failed (in the front end)") end
    end)
else
    ForkThread(function()
        check("Launch started a session", SessionIsActive(), true)
        local scenario = SessionGetScenarioInfo()
        check("the briefing's operation", scenario.campaignInfo and scenario.campaignInfo.opKey, "X1CA_001")
        check("at the briefing's difficulty", scenario.Options.Difficulty, import("/lua/user/prefs.lua").GetFromCurrentProfile("campaign.difficulty") or 2)
        LOG("TEST note: campaign faction " .. tostring(scenario.Options.FACampaignFaction))
        check("with a campaign faction", type(scenario.Options.FACampaignFaction), "string")
        check("the player's army first", GetFocusArmy(), 1)
        LOG("TEST done: " .. failed .. " failed")
    end)
end
