-- The settings as a file (ours; /moho64ui/lua/settingsfile.lua, the rows "Save Settings" and "Load Settings" of
-- Options > Interface; Chris, 2026-11-27, the ideas' number 9). The text made and read back; what a file may and may
-- not bring (an unknown key, a value the option cannot take, the PC's own keys, a line that is no setting); a save
-- onto the Desktop (in tests: MOHO64UI_REPORT_FOLDER) and the list of files there; a load into the Options dialog:
-- its rows and its working set change, the profile does not until OK, and Cancel leaves everything.
-- The lab's runner (suite/settingsfile.sh) reads the file written and plants a second one for the load.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/settingsfile.lua --frames 600
-- `-- /pic saved | choose | loaded` leaves that box up at the end (for a --screenshot).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Settings = import("/moho64ui/lua/settingsfile.lua")
local logic = import("/lua/options/optionslogic.lua")
local ESC, ENTER = 27, 13
local pic = (GetCommandLineArg("/pic", 1) or {})[1] or "none"
local function up(control) return control and not IsDestroyed(control) and true or false end
local function boxText()
    local lines = {}
    for _, t in (up(Settings.Controls.box) and Settings.Controls.box.texts) or {} do table.insert(lines, t:GetText()) end
    return (string.gsub(table.concat(lines, " "), "%s+", " "))
end

ForkThread(function()
    WaitSeconds(1)
    local current = logic.GetCurrent()
    -- the text
    local text, count = Settings.Text(current)
    check("the text names many options", count > 60, true)
    LOG("TEST note the text: " .. count .. " options, " .. string.len(text) .. " bytes")
    check("it begins with its heading", string.sub(text, 1, 24), "# FA Remastered settings")
    check("it has the Remastered Preset's line", string.find(text, "\nremaster_preset = " .. tostring(current.remaster_preset) .. "\r\n", 1, true) ~= nil, true)
    check("the screen's resolution is not in it", string.find(text, "primary_adapter", 1, true), nil)
    check("nor this program's own notes", string.find(text, "remaster_first_start", 1, true), nil)
    check("nor a button's row", string.find(text, "remaster_settings_save", 1, true), nil)
    -- read back: the same values
    local values, bad = Settings.Parse(text)
    local same, n = true, 0
    for key, value in values do
        n = n + 1
        if current[key] ~= value then
            same = false
            LOG("TEST note read back otherwise: " .. key .. " = " .. tostring(value) .. " (" .. type(value) .. "), set " .. tostring(current[key]) .. " (" .. type(current[key]) .. ")")
        end
    end
    check("read back: every line a setting, each as it was written", tostring(bad) .. " " .. n .. " " .. tostring(same), "0 " .. count .. " true")
    for _, tab in logic.GetOptionsData() do
        for _, item in tab.items do
            if item.key and values[item.key] ~= nil and not Settings.Fits(item, values[item.key]) then
                LOG("TEST note its own value does not fit: " .. item.key .. " = " .. tostring(values[item.key]) .. " (" .. tostring(item.type) .. ")")
            end
        end
    end
    local take, counts = Settings.Sort(values, current)
    check("against the same settings: nothing to take", counts.taken .. " " .. counts.same .. " " .. counts.unknown .. " " .. counts.invalid, "0 " .. count .. " 0 0")

    -- what a file may bring
    local made = table.concat({
        "# a file written by hand",
        "remaster_preset = 2",
        "texture_level = 1",
        "  shadow_quality=1  ",
        "remaster_preset_that_is_not = 3",
        "primary_adapter = \"640,480,60\"",
        "remaster_first_start_advice = 1",
        "texture_level_text = \"a \\\"quoted\\\" word\"",
        "remaster_crash_reports = 7",
        "fidelity = high",
        "this line is no setting",
        "os.execute(\"calc\")",
        "",
    }, "\n")
    values, bad = Settings.Parse(made)
    check("a file by hand: numbers, a trimmed line, a quoted text", tostring(values.remaster_preset) .. " " .. tostring(values.shadow_quality) .. " " .. tostring(values.texture_level_text), '2 1 a "quoted" word')
    check("three lines are no setting (a bare word as a value, a sentence, a call)", bad, 3)
    check("nothing of it was run: no such key", values.os, nil)
    local base = { remaster_preset = 4, texture_level = 0, shadow_quality = 3, remaster_crash_reports = 1 }
    for key, value in current do if base[key] == nil then base[key] = value end end
    take, counts = Settings.Sort(values, base)
    check("taken: the three that are options with values they can take", counts.taken .. " " .. tostring(take.remaster_preset) .. " " .. tostring(take.texture_level) .. " " .. tostring(take.shadow_quality), "3 2 1 1")
    check("unknown: the key that is no option, the PC's, this program's note, the text key", counts.unknown, 4)
    check("invalid: a value the option cannot take", counts.invalid .. " " .. tostring(take.remaster_crash_reports), "1 nil")

    -- the rows in Options > Interface
    local save, load
    for _, item in logic.GetOptionsData().ui.items do
        if item.key == 'remaster_settings_save' then save = item end
        if item.key == 'remaster_settings_load' then load = item end
    end
    check("Options > Interface has the two rows, buttons", tostring(save and save.type) .. " " .. tostring(load and load.type), "button button")
    check("their words", tostring(save and save.title) .. ": " .. tostring(save and save.custom.text) .. " | " .. tostring(load and load.title) .. ": " .. tostring(load and load.custom.text),
        "Save Settings: To Desktop... | Load Settings: From Desktop...")

    -- in the Options dialog: save what it shows, then load the runner's file into it
    local dialogs = import("/lua/ui/dialogs/options.lua")
    check("no working set while the dialog is not up", dialogs.RemasterWorkingSet(), nil)
    local closed = false
    dialogs.CreateDialog(GetFrame(0), function() closed = true end)
    WaitFrames(5)
    local set = dialogs.RemasterWorkingSet()
    check("the dialog is up: its working set is there", type(set), "table")
    if save then save.update(nil, 0) end
    WaitFrames(2)
    check("Save: a file was written", type(Settings.State.last) == 'string' and string.find(Settings.State.last, "FA_Remastered_settings_", 1, true) ~= nil, true)
    LOG("TEST note saved file: " .. tostring(Settings.State.last) .. (Settings.State.why and (" (" .. tostring(Settings.State.why) .. ")") or ""))
    check("a box says where", string.find(boxText(), "Your settings are saved on your Desktop:", 1, true) ~= nil, true)
    local box = Settings.Controls.box
    if up(box) then
        local frame = GetFrame(0)
        check("the box lies inside the screen", box.Left() >= 0 and box.Top() >= 0 and box.Right() <= frame.Right() and box.Bottom() <= frame.Bottom(), true)
        check("the file's name on one line inside it", box.nameText and box.nameText.Left() >= box.content.Left() and box.nameText.Right() <= box.content.Right(), true)
    end
    if pic == "saved" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    moho64ui_Key(ENTER)
    WaitFrames(3)
    check("Enter closes the box", up(box), false)

    -- the list: the file just saved, and the runner's planted one (named so that it sorts as the older)
    if load then load.update(nil, 0) end
    WaitFrames(2)
    local files = Settings.State.files or {}
    check("Load: the list has the file just saved first", files[1] and files[1].name, string.gsub(Settings.State.last or "", "^.*[/\\]", ""))
    local planted
    for _, f in files do
        if f.name == "FA_Remastered_settings_planted.txt" then planted = f end
    end
    check("and the file the runner planted", planted ~= nil, true)
    box = Settings.Controls.box
    check("a box lists them to click", up(box) and table.getn(box.links) == table.getn(files), true)
    if pic == "choose" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    -- a click on the planted one
    local before = logic.GetCurrent()
    for i, f in files do
        if f.name == "FA_Remastered_settings_planted.txt" and up(box) then box.links[i]:HandleEvent({ Type = 'ButtonPress' }) end
    end
    WaitFrames(3)
    local loaded = Settings.State.loaded or {}
    LOG("TEST note loaded: " .. tostring(loaded.taken) .. " taken, " .. tostring(loaded.same) .. " same, " .. tostring(loaded.unknown) .. " unknown, " .. tostring(loaded.invalid) .. " invalid, " .. tostring(loaded.unread) .. " unread")
    check("the planted file: three taken, one unknown, one invalid, one line not read", tostring(loaded.taken) .. " " .. tostring(loaded.unknown) .. " " .. tostring(loaded.invalid) .. " " .. tostring(loaded.unread), "3 1 1 1")
    set = dialogs.RemasterWorkingSet() or {}
    check("the dialog's working set has them", tostring(set.remaster_crash_reports) .. " " .. tostring(set.texture_level) .. " " .. tostring(set.shadow_quality), "0 2 1")
    local now = logic.GetCurrent()
    check("the profile is as before: nothing kept yet", tostring(now.remaster_crash_reports) .. " " .. tostring(now.texture_level) .. " " .. tostring(now.shadow_quality),
        tostring(before.remaster_crash_reports) .. " " .. tostring(before.texture_level) .. " " .. tostring(before.shadow_quality))
    check("a box says to press Apply or OK, and what was left out",
        string.find(boxText(), "3 settings are changed in the Options window. Press Apply or OK to use them", 1, true) ~= nil
        and string.find(boxText(), "Left out: 1 not known to this build, 1 with a value its option cannot take, 1 line that could not be read.", 1, true) ~= nil, true)
    -- (the row shown on the Interface tab took its value: Crash Reports)
    local crash = logic.FindOptionItem('remaster_crash_reports')
    LOG("TEST note the box: " .. boxText())
    if pic == "loaded" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    moho64ui_Key(ESC)
    WaitFrames(3)
    check("Escape closes the box, the dialog stays", tostring(up(Settings.Controls.box)) .. " " .. tostring(closed), "false false")
    LOG("TEST done: " .. failed .. " failed")
end)
