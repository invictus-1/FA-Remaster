-- FA Remastered: "Wrecks appear only once seen" and the reclaim labels (this file is a hook: the program reads it after
-- the game's /lua/ui/game/reclaim.lua, which stays as it is). Chris, 2026-11-13; the option is ours (Options,
-- Remastered tab, off at first: off is the original's and FAF's way, in which every wreck has its label at once).
-- The simulation tells the interface every prop worth a label (Prop.lua: Sync.Reclaim[id] = {mass, position}, or
-- false when it is gone), and the game's UpdateReclaim keeps them for its labels. With the option on, the label of a
-- unit's wreck that the player's army and its allies have not seen is held back here and handed on once they have
-- (moho64ui_PropSeen: the engine's record of which armies have had a wreck's place in sight). Trees, rocks and the
-- map's own wrecks count as seen by everyone. Judged again every beat for the held ones, and for every label when the
-- option is switched or the view goes to another army (a replay). Nothing here changes what the simulation does.
do
    local GameUpdateReclaim = UpdateReclaim
    local all, held = {}, {}    -- every point the simulation has told of, and those held back, by the prop's id
    local viewNow = false       -- the army the labels are judged for; false: nothing is held back
    local beatAdded = false

    local function View()
        local holds = rawget(_G, 'moho64ui_WrecksOnceSeen')
        if holds and holds() then return GetFocusArmy() end
        return false
    end

    local function Beat()
        local view = View()
        local out, any = {}, false
        if view ~= viewNow then
            -- the option switched, or another army's eyes: every label judged anew
            viewNow = view
            for id, point in all do
                local seen = (not view) or moho64ui_PropSeen(id)
                if seen and held[id] then
                    held[id] = nil
                    out[id] = point
                    any = true
                elseif not seen and not held[id] then
                    held[id] = point
                    out[id] = false
                    any = true
                end
            end
        elseif view then
            for id, point in held do
                if moho64ui_PropSeen(id) then
                    held[id] = nil
                    out[id] = point
                    any = true
                end
            end
        end
        if any then GameUpdateReclaim(out) end
    end

    function UpdateReclaim(reclaimPoints)
        if not beatAdded then
            beatAdded = true
            import('/lua/ui/game/gamemain.lua').AddBeatFunction(Beat, false, 'RemasterWrecksOnceSeen')
        end
        if not viewNow then
            -- nothing held back: the game's own way, the points only noted (the option may be switched on later)
            for id, point in reclaimPoints do all[id] = point or nil end
            return GameUpdateReclaim(reclaimPoints)
        end
        local pass = {}
        for id, point in reclaimPoints do
            if not point then
                all[id] = nil
                held[id] = nil
                pass[id] = false
            else
                all[id] = point
                if moho64ui_PropSeen(id) then
                    held[id] = nil
                    pass[id] = point
                else
                    held[id] = point  -- (no label of it is there: a wreck once seen stays seen)
                end
            end
        end
        GameUpdateReclaim(pass)
    end

    -- (for the tests: the labels held back, the game's own function, and the beat's step)
    function RemasterReclaimHeld() return held end
    function RemasterGameUpdateReclaim() return GameUpdateReclaim end
    function RemasterReclaimBeat() Beat() end
end
