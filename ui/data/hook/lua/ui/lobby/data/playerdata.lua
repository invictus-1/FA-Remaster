-- FA Remastered: our AI's per-slot options in every slot's data (this file is a hook: the program reads it after the
-- game's /lua/ui/lobby/data/playerdata.lua, which stays as it is). A PlayerData has a fixed set of keys, the ones it
-- is made with (WatchedValueTable: setting any other is an error), so the four are added to every new one at their
-- defaults; the lobby (our hook of lobby.lua) sets them for a slot with our AI. They travel with the slot's other
-- options to the other players and into the launch, where the simulation reads them as ScenarioInfo.ArmySetup[army]
-- (docs/ai-architecture.md 2.7, 2.8); a human slot carries them unused.
do
    local defaults = {
        RemasterPersonality = 'random',
        RemasterAggression = 0,
        RemasterEconomy = 0,
        RemasterTeacher = 'off',
    }
    local Base = PlayerData
    PlayerData = ClassUI(Base) {
        __init = function(self, initialMapping)
            initialMapping = initialMapping or {}
            for key, value in defaults do
                if initialMapping[key] == nil then initialMapping[key] = value end
            end
            Base.__init(self, initialMapping)
        end
    }
end
