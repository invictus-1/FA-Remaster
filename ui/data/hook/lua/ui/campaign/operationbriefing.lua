-- FA Remastered: the campaign is played the original game's way, from the Campaign menu (Chris,
-- 2026-10-16). FAF's briefing makes its Launch button and switches it off at once (FAF plays the
-- campaign through its co-op lobby); here it works again: the button is caught as the briefing
-- makes it and switched on after. And two of FAF's callers still call the briefing the original's
-- way, with an operation's ID and its data (campaignmanager.LaunchBriefing, the next operation's
-- briefing after a won one; selectcampaign's TimelineFMV, the first operation's after the first
-- visit's film), which FAF's CreateUI(parent, scenarioPath) cannot take: such a call opens that
-- operation's scenario. (This program's own code, a hook: the game's files stay as they are.
-- Listed as a deviation from FAF in ui/README.md.)
local FAFCreateUI = CreateUI
function CreateUI(parent, scenarioPath)
    if type(parent) == 'string' then
        scenarioPath = '/maps/' .. parent .. '/' .. parent .. '_scenario.lua'
        parent = nil
    end
    local UIUtil = import("/lua/ui/uiutil.lua")
    local makeButton = UIUtil.CreateButtonStd
    local launch = nil
    UIUtil.CreateButtonStd = function(buttonParent, filename, label, a, b, c, d, e, f)
        local button = makeButton(buttonParent, filename, label, a, b, c, d, e, f)
        if label == "<LOC opbrief_0003>Launch" then
            launch = button
        end
        return button
    end
    local ok, result = pcall(FAFCreateUI, parent, scenarioPath)
    UIUtil.CreateButtonStd = makeButton
    if not ok then
        error(result, 0)
    end
    if launch then
        launch:Enable()
    end
    return result
end
