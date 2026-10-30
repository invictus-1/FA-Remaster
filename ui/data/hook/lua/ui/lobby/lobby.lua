-- FA Remastered: our AI's per-slot options in the lobby (this file is a hook: the program reads it after the game's
-- /lua/ui/lobby/lobby.lua, which stays as it is). The slot's AI key carries type and skill only (our aitypes hook);
-- personality, aggression, economy and teacher mode are options of the slot (docs/ai-architecture.md 2.7, 2.8),
-- kept in its PlayerOptions (our playerdata hook gives every slot the four keys) and so sent to the other players
-- and into the launch as the slot's other options are.
--   A slot with our AI has "Remastered AI Settings..." first in its name's list: a small dialog of the four, which
--   the host changes and the other players only see. Another of our AIs picked for the same slot keeps them.
--   The slot's row shows the settings that differ from the defaults after the AI's name, as much as fits: in words,
--   else in short ("Rusher A+1 E-2 T"), else "(...)" (the dialog has them).
-- The lobby keeps its controls (GUI) and its connection (lobbyComm) in locals of its module, which a hook does not
-- see: they are read through the game's SetSlotInfo, which uses both. If a later lobby changes that, the entry and
-- the dialog are left out and the slots stay as the game makes them.
do
    local kSettingsKey = 'remaster_ai_settings'
    local kChoices = {
        {
            key = 'RemasterPersonality', label = "Personality",
            values = {
                { key = 'random', text = "Random" },
                { key = 'balanced', text = "Balanced" },
                { key = 'rusher', text = "Rusher" },
                { key = 'turtle', text = "Turtle" },
            },
        },
        {
            key = 'RemasterAggression', label = "Aggression",
            values = {
                { key = -2, text = "-2  Defensive" }, { key = -1, text = "-1" }, { key = 0, text = "0  Normal" },
                { key = 1, text = "+1" }, { key = 2, text = "+2  Aggressive" },
            },
        },
        {
            key = 'RemasterEconomy', label = "Economy",
            values = {
                { key = -2, text = "-2  Army first" }, { key = -1, text = "-1" }, { key = 0, text = "0  Normal" },
                { key = 1, text = "+1" }, { key = 2, text = "+2  Economy first" },
            },
        },
        {
            key = 'RemasterTeacher', label = "Teacher Mode",
            values = { { key = 'off', text = "Off" }, { key = 'on', text = "On" } },
        },
    }

    local function IsOurs(aiKey)
        return type(aiKey) == 'string' and string.sub(aiKey, 1, 10) == 'remastered'
    end
    local function OursIn(playerInfo)
        return playerInfo and not playerInfo.Human and IsOurs(playerInfo.AIPersonality)
    end

    local RemasterGameSetSlotInfo = SetSlotInfo
    local function LobbyLocal(name)
        for i = 1, 200 do
            local n, v = debug.getupvalue(RemasterGameSetSlotInfo, i)
            if not n then return nil end
            if n == name then return v end
        end
    end

    -- the settings that differ from the defaults: "Rusher, A+1, E-2, Teacher", or short "Rusher A+1 E-2 T"
    function RemasterAISettingsText(playerInfo, short)
        local parts = {}
        local personality = playerInfo.RemasterPersonality
        if personality and personality ~= 'random' then
            for _, v in kChoices[1].values do
                if v.key == personality then table.insert(parts, v.text) end
            end
        end
        local function signed(letter, value)
            value = tonumber(value) or 0
            if value > 0 then table.insert(parts, letter .. '+' .. value)
            elseif value < 0 then table.insert(parts, letter .. value) end
        end
        signed('A', playerInfo.RemasterAggression)
        signed('E', playerInfo.RemasterEconomy)
        if playerInfo.RemasterTeacher == 'on' then table.insert(parts, short and "T" or "Teacher") end
        return table.concat(parts, short and " " or ", ")
    end

    -- another of our AIs picked for a slot that has one keeps the slot's settings
    local RemasterGameGetAIPlayerData = GetAIPlayerData
    function GetAIPlayerData(name, AIPersonality, slot)
        local data = RemasterGameGetAIPlayerData(name, AIPersonality, slot)
        local old = GetGameSettings().PlayerOptions[slot]
        if IsOurs(AIPersonality) and OursIn(old) then
            for _, choice in kChoices do data[choice.key] = old[choice.key] end
        end
        return data
    end

    -- the dialog of the four for the AI in `slot`: the host's to change, the other players' to see
    function ShowRemasterAISettings(slot)
        local GUI, lobbyComm = LobbyLocal('GUI'), LobbyLocal('lobbyComm')
        local players = GetGameSettings().PlayerOptions
        if not GUI or not lobbyComm or not OursIn(players[slot]) then return end
        local editable = lobbyComm:IsHost()
        if GUI.remasterAISettings then GUI.remasterAISettings:Close() end
        local UIUtil = import('/lua/ui/uiutil.lua')
        local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
        local Group = import('/lua/maui/group.lua').Group
        local Combo = import('/lua/ui/controls/combo.lua').Combo
        local Popup = import('/lua/ui/controls/popups/popup.lua').Popup

        local content = Group(GUI)
        LayoutHelpers.SetDimensions(content, 420, 80 + 40 * table.getn(kChoices) + 50)
        local dialog = Popup(GUI, content)
        GUI.remasterAISettings = dialog
        local closed = dialog.OnClosed
        dialog.OnClosed = function(self)
            if GUI.remasterAISettings == self then GUI.remasterAISettings = nil end
            closed(self)
        end

        local title = UIUtil.CreateText(content, "Remastered AI, slot " .. slot, 20, UIUtil.titleFont)
        LayoutHelpers.AtHorizontalCenterIn(title, content)
        LayoutHelpers.AtTopIn(title, content, 15)
        local who = UIUtil.CreateText(content, players[slot].PlayerName .. (editable and "" or "  (set by the host)"), 14, UIUtil.bodyFont)
        LayoutHelpers.AtHorizontalCenterIn(who, content)
        LayoutHelpers.AtTopIn(who, content, 45)

        dialog.combos = {}
        for row, choice in kChoices do
            local label = UIUtil.CreateText(content, choice.label, 16, UIUtil.bodyFont)
            LayoutHelpers.AtLeftTopIn(label, content, 25, 40 + 40 * row)
            local combo = Combo(content, 14, table.getn(choice.values), false, nil, "UI_Tab_Rollover_01", "UI_Tab_Click_01")
            LayoutHelpers.SetWidth(combo, 200)
            LayoutHelpers.AtRightTopIn(combo, content, 25, 40 + 40 * row)
            local texts, at = {}, 1
            for i, v in choice.values do
                table.insert(texts, v.text)
                if v.key == players[slot][choice.key] then at = i end
            end
            combo:AddItems(texts, at)
            local key, values = choice.key, choice.values
            combo.OnClick = function(self, index, text)
                local playerInfo = GetGameSettings().PlayerOptions[slot]
                if OursIn(playerInfo) and values[index] then SetPlayerOption(slot, key, values[index].key) end
            end
            if not editable then combo:Disable() end
            dialog.combos[choice.key] = combo
        end

        local ok = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/medium/', LOC("<LOC _Close>Close"))
        LayoutHelpers.AtHorizontalCenterIn(ok, content)
        LayoutHelpers.AtBottomIn(ok, content, 10)
        ok.OnClick = function(self) dialog:Close() end
        dialog.closeButton = ok
        return dialog
    end

    -- the row's text: the name and as much of the settings as fits (measured in the list's font, a little larger than
    -- the row's, so it errs on the short side)
    local function ShownName(combo, playerInfo)
        local name = GetPlayerDisplayName(playerInfo)
        local long = RemasterAISettingsText(playerInfo, false)
        if long == "" then return name end
        local room = combo.Width() - 24
        for _, text in { name .. " (" .. long .. ")", name .. " (" .. RemasterAISettingsText(playerInfo, true) .. ")" } do
            if combo._list:GetStringAdvance(text) <= room then return text end
        end
        return name .. " (...)"
    end

    -- a slot with our AI: the settings first in its list (the host's own entries and their tooltips after it)
    function SetSlotInfo(slotNum, playerInfo)
        RemasterGameSetSlotInfo(slotNum, playerInfo)
        local GUI = LobbyLocal('GUI')
        local slot = GUI and GUI.slots and GUI.slots[slotNum]
        local combo = slot and slot.name
        if not combo or not OursIn(playerInfo) then return end
        if not combo.RemasterSettings then
            combo.RemasterSettings = true
            local gameOnClick = combo.OnClick
            combo.OnClick = function(self, index, text)
                if self.slotKeys and self.slotKeys[index] == kSettingsKey then
                    ShowRemasterAISettings(slotNum)
                else
                    gameOnClick(self, index, text)
                end
            end
        end
        local aiKeys = {}
        for _, entry in import('/lua/ui/lobby/aitypes.lua').GetAItypes() do aiKeys[entry.key] = true end
        local keys, strings, tooltips = { kSettingsKey }, { "Remastered AI Settings..." }, {
            { text = "Remastered AI Settings", body = "This AI's personality, aggression, economy and teacher mode" },
        }
        local gameKeys = combo.slotKeys or {}
        for i = 1, table.getn(gameKeys) do
            table.insert(keys, gameKeys[i])
            table.insert(strings, combo._list:GetItem(i - 1))
            table.insert(tooltips, aiKeys[gameKeys[i]] and ('aitype_' .. gameKeys[i]) or false)
        end
        combo:ClearItems()  -- (also clears the title and its colour: both set again)
        combo.slotKeys = keys
        combo:AddItems(strings)
        combo:Enable()
        combo:SetTitleText(ShownName(combo, playerInfo))
        if combo._titleColor then combo._text:SetColor(combo._titleColor) end
        import('/lua/ui/game/tooltip.lua').AddComboTooltip(combo, tooltips)
    end
end
