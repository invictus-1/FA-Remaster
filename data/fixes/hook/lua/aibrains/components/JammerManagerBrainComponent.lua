-- FA Remastered, the engine's script fixes (docs/deviations.md 8, "The jammer manager's walk"). Joined
-- to the end of /lua/aibrains/components/JammerManagerBrainComponent.lua when Sim::fixes is on; never in the playback
-- of the original's replays.
--
-- FAF's JammingToggleThread walks the brain's jammers (a weak table keyed by EntityId) and forks a follow-up thread
-- for each one due, in the table's own order, which depends on when the Lua collector last ran. The threads then
-- switch the jammers' intel off and on in that order. And a destroyed jammer stays in the table until the collector
-- clears it, so whether it still got a (useless) follow-up thread depended on the collector too. Here the walk goes
-- in EntityId order and passes over a destroyed jammer, as FAF's does once the collector has cleared it; the rest is
-- FAF's code unchanged, copied from FAF's file:
--
-- ******************************************************************************************************
-- ** Copyright (c) 2024 Willem 'Jip' Wijnia
-- **
-- ** Permission is hereby granted, free of charge, to any person obtaining a copy
-- ** of this software and associated documentation files (the "Software"), to deal
-- ** in the Software without restriction, including without limitation the rights
-- ** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- ** copies of the Software, and to permit persons to whom the Software is
-- ** furnished to do so, subject to the following conditions:
-- **
-- ** The above copyright notice and this permission notice shall be included in all
-- ** copies or substantial portions of the Software.
-- **
-- ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- ** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- ** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- ** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- ** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
-- ** SOFTWARE.
-- ******************************************************************************************************
do
    local function KeyLess(a, b)
        -- EntityIds are digit strings: the shorter is the smaller, then by text, which is by value. Not by
        -- tonumber: the sim's Lua numbers are single floats, and ids above 2^24 (0x40100000, ...) share one.
        local sa, sb = tostring(a), tostring(b)
        local la, lb = string.len(sa), string.len(sb)
        if la ~= lb then
            return la < lb
        end
        return sa < sb
    end

    JammerManagerBrainComponent.JammingToggleThread = function(self)
        while true do
            local jammers = self.Jammers
            local ids, count = {}, 0
            for id in jammers do
                count = count + 1
                ids[count] = id
            end
            table.sort(ids, KeyLess)
            for i = 1, count do
                local unitWithJammer = jammers[ids[i]]
                if unitWithJammer and not IsDestroyed(unitWithJammer) then
                    if unitWithJammer.ResetJammer == 0 then
                        self.Trash:Add(ForkThread(self.JammingFollowUpThread, self, unitWithJammer))
                        unitWithJammer.ResetJammer = -1
                    else
                        if unitWithJammer.ResetJammer > 0 then
                            unitWithJammer.ResetJammer = unitWithJammer.ResetJammer - 1
                        end
                    end
                end
            end
            WaitSeconds(1)
        end
    end
end
