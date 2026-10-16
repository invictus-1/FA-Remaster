-- FA Remastered: the campaign is played the original game's way, from the Campaign menu (Chris,
-- 2026-10-16), so an operation's end is kept as the original kept it: the operation is noted in the
-- player's profile (its 'campaign' table: campaign, operation, difficulty -> allPrimary,
-- allSecondary), which makes the next operation selectable, and back in the menu the next
-- operation's briefing opens (the front end's 'NextOpBriefing', which uimain.lua reads). FAF's
-- OperationVictory only shows the end's dialog: it still does that, after this. The operation's
-- names come from the session's campaignInfo (SetupCampaignSession's {opKey, campaignID,
-- difficulty}) when the end's table has none, as FAF's ScenarioFramework sends none. (This
-- program's own code, a hook; listed as a deviation from FAF in ui/README.md.)
local FAFOperationVictory = OperationVictory
function OperationVictory(ovTable, skipDialog)
    local info = (SessionGetScenarioInfo and SessionGetScenarioInfo() or {}).campaignInfo or {}
    local campaignID = ovTable.campaignID or info.campaignID
    local opKey = ovTable.opKey or info.opKey
    local difficulty = ovTable.difficulty or info.difficulty
    if campaignID and opKey and difficulty then
        local Prefs = import("/lua/user/prefs.lua")
        local campaigns = Prefs.GetFromCurrentProfile('campaign') or {}
        campaigns[campaignID] = campaigns[campaignID] or {}
        campaigns[campaignID][opKey] = campaigns[campaignID][opKey] or {{}, {}, {}}
        campaigns[campaignID][opKey][difficulty] = campaigns[campaignID][opKey][difficulty] or {}
        campaigns[campaignID][opKey][difficulty].allPrimary = ovTable.allPrimary
        campaigns[campaignID][opKey][difficulty].allSecondary = ovTable.allSecondary
        Prefs.SetToCurrentProfile('campaign', campaigns)
        SavePreferences()
        -- (Only after a won operation: the original also did it after a lost one, which opened the
        -- next operation's briefing although that operation stayed locked. Our judgement.)
        if not skipDialog and ovTable.success then
            SetFrontEndData('NextOpBriefing', GetNextOperation(campaignID, opKey, difficulty))
        end
    else
        WARN("OperationVictory: the operation is not known (campaign " .. tostring(campaignID) .. ", operation " .. tostring(opKey) .. ")")
    end
    return FAFOperationVictory(ovTable, skipDialog)
end
