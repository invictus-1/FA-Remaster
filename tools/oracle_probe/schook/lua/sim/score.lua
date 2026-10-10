-- SupCom Lab: moho64 oracle probe (offline lab only; see /moho64_probe.lua). Errors are contained.
if rawget(_G, 'moho64_probe') then
    local P = moho64_probe
    local ok, e = pcall(function()
        P.out('score.lua loaded at tick', GetGameTick())
        P.Blueprints()
        P.Threads()
        ForkThread(P.Armies) -- v19
        ForkThread(P.BuildChecks) -- v22
        ForkThread(P.M28Sites) -- v23
        ForkThread(P.PropDump) -- v24
        P.TreeWatch() -- v25
        ForkThread(P.M28Orders) -- v28
        ForkThread(P.M28Zones) -- v29
        ForkThread(function()
            while GetGameTick() < 20 do WaitTicks(1) end
            P.Motion() -- yields (WaitTicks): no pcall around it; its own steps are pcall'd
            while GetGameTick() < 460 do WaitTicks(1) end
            P.Combat()
            P.Transport()
            P.Destroy() -- v7
            ForkThread(P.Staging) -- v11 (beside the ferry)
            ForkThread(P.Factory) -- v13
            ForkThread(P.Carrier) -- v13
            ForkThread(P.Formation) -- v16
            P.Ferry() -- v10
        end)
    end)
    if not ok then LOG('PROBE error ' .. tostring(e)) end
end
