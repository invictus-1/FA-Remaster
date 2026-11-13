# Options menu: reorganising the remastered options

For the ui chat to decide. Chris, 2026-11-08: "eventually we will need to organize the video menu with how many
features we have since its getting kinda big ... keep the same theme/look ... drop down menus that open up different
remaster options. Or putting everything in remaster into its own tab or something I'll let you decide."

**Changed 2026-11-11 (the review's 4.2 item 11, the ui chat's call):** the section Preset (Remastered Preset, Look) is the FIRST section, before Display & Performance: the one control that sets the rest stood ten rows down. Frame Rate offers 60, 90, 120, 144, 165, 240 beside Vertical Sync and Uncapped. Left as they are, on purpose: Ghost Look and Cloak Look stay On / Off (On shows the ghost / the cloak, which is also the original's way: the tip says so); Battle Effects stays one switch (the renderer's thirteen effects have one cost and one look; a split waits for a wish); Go to Section is the tab's first row and scrolls away with it (the grid cannot hold a row in place).

**As built (2026-11-09, recommendation (a)), measured where this paper estimated:** the textures are 698 (panel)
and 178 (tab button) wide, not 700 and 180: the game's four tabs end 6 short of the panel's edge, five get **145**
each (not 146); "Remastered" (105 px in the title font at 16) fits with room to spare, no fallback. FAF's Video tab has
17 items, so **14** rows after the three we leave out (not 12); with our header and the Show button 16. The hover of an
option row needs the row in view (the grid places only the rows it shows). Tested by `ui/tests/options_tabs.lua`.

Read only. Nothing was built or changed. Sources: `fa-remaster-ui/ui/data/hook/lua/options/options.lua`, FAF's
`lua/options/options.lua`, `lua/options/optionsLogic.lua`, `lua/ui/dialogs/options.lua` (lua.nx2), the dialog's
textures (read-only, Steam `textures.scd`), `ui/data/lua/palette.lua`, `ui/user/keymap.cpp`, and the tests named below.

---

## 1. What is there today

### 1.1 Our options: 47, 42 of them in the Video tab

| Tab | FAF's own rows | Ours | Rows in all |
|---|---|---|---|
| Gameplay | FAF's (with FAF's headers) | 1: Simulation Order for New Games | +1 |
| Interface | FAF's (with FAF's headers) | 4: Colour Vision, Path Preview, Score Screen Extras, Key Bindings "Import from FAF..." (a `button`) | +4 |
| **Video** | **12** (15, minus Shadow Resolution, Shadow render distance and Render Sky, which we take out) | **42** | **54** |
| Sound | FAF's | none | |

The grid shows about 10.5 rows (area 604 x 380, rows 36 high), so Video is five pages of scrolling, and our 42 come
after FAF's 12 in the order they were added, with no grouping.

Two placement faults we have today, by the way: our Interface rows sit after FAF's last header "Casting tools" and our
Gameplay row after "Cursor features", so on screen they look as if they belonged to those sections.

The 42 Video options (type, default; "preset" = one of the 30 keys `remasterPresets` sets):

| # | Title | key | type | default | preset |
|---|---|---|---|---|---|
| 1 | Frame Rate | remaster_frame_rate | toggle (5) | Vertical Sync | no |
| 2 | Low Latency | remaster_low_latency | toggle | On | no |
| 3 | Remastered Preset | remaster_preset | toggle (6) | Ultra | (the preset) |
| 4 | Look | remaster_look | toggle | Remastered | yes |
| 5 | Graphics API | remaster_graphics_api | toggle | Direct3D 12 | no |
| 6 | Upscaling (FSR 1) | remaster_upscale | toggle (5) | Off | no |
| 7 | Temporal Upscaling (FSR 3) | remaster_temporal_upscale | toggle | Off | no |
| 8 | Atmosphere | remaster_atmosphere | toggle | On | yes |
| 9 | Cloud Layout | remaster_cloud_layout | toggle | Mixed | yes |
| 10 | Storm Clouds | remaster_storm_clouds | toggle | Cells | yes |
| 11 | Night Sky | remaster_night_sky | toggle | Starry | yes |
| 12 | Soft Shadows | remaster_soft_shadows | toggle | On | yes |
| 13 | Ray Tracing | remaster_ray_tracing | toggle (1 or 2 states) | On where the card can | yes |
| 14 | Weather Effects | remaster_weather_effects | toggle | On | yes |
| 15 | Water Ripples | remaster_water_ripples | toggle | On | yes |
| 16 | Ground Detail | remaster_ground_detail | toggle | On | yes |
| 17 | Frame Generation | remaster_frame_generation | toggle | Off | no |
| 18 | Cinematic Lens Blur | remaster_lens | toggle | On | no |
| 19 | Remastered Effects | remaster_effects | toggle | On | yes |
| 20 | Effects Quality | remaster_effects_quality | toggle (3) | Full | yes |
| 21 | Shell Tracers | remaster_tracers | toggle | On | yes |
| 22 | Rich Ground on Remastered Maps | remaster_rich_ground | toggle | On | yes |
| 23 | Tracks in Snow | remaster_snow_tracks | toggle | On | yes |
| 24 | Photographic Look | remaster_photo_look | toggle (5) | Off | no |
| 25 | Season | remaster_season | toggle | Summer | no |
| 26 | Battle Effects | remaster_battle_effects | toggle | On | yes |
| 27 | Energy Shields | remaster_energy_shields | toggle | On | yes |
| 28 | Hidden Unit Outlines | remaster_hidden_outlines | toggle | On | yes |
| 29 | Unit Glow Colour | remaster_lamp_colour | toggle | Team colour | no |
| 30 | Scorch Glow | remaster_scorch | toggle | Soft | no |
| 31 | Ocean Glitter | remaster_ocean_glitter | toggle | On | yes |
| 32 | Sea Sparkle | remaster_sea_sparkle | toggle | Off | yes |
| 33 | Close-up Detail | remaster_unit_detail | toggle | On | yes |
| 34 | Shore Soak | remaster_shore_soak | toggle | On | yes |
| 35 | Shield Bending | remaster_shield_bend | toggle (3) | Subtle | yes |
| 36 | Unit Glow at Night | remaster_unit_glow | slider 0-100 | 75 | yes |
| 37 | Sharpen | remaster_sharpen | slider 0-100 | 50 | yes |
| 38 | Remastered Gateway | remaster_gateway | toggle | On | yes |
| 39 | Ghost Look | remaster_ghost_look | toggle | On | yes |
| 40 | Guide over Grass | remaster_guide | toggle | On | yes |
| 41 | Cloak Look | remaster_cloak_look | toggle | On | yes |
| 42 | Stealth Veil | remaster_stealth_veil | toggle | On | yes |

### 1.2 How the options reach the program: the tab does not matter

- Each option's `set` sends a console command (`remaster_*`, listed in `keymap.cpp` `ConsoleNames()`, lines
  310-319); the program never asks which tab an option is in.
- The profile keeps the options as ONE flat table (`optionslogic.GetCurrent`: `curOptions[item.key]`), not per tab.
  `GetCurrent`, `SetCurrent`, `Apply`, `ResetToDefaults`, `FindOptionItem` all loop over every table in `options`,
  so a new `options.remaster` table is picked up by all of them by itself.
- So moving an option between tabs, or adding a tab, needs **no prefs migration**; every player keeps his values.

### 1.3 What FAF's dialog can do (we only hook it)

From `lua/ui/dialogs/options.lua` and `lua/options/options.lua`:

- **Tabs**: one button per entry of `optionsOrder` (`{"gameplay","ui","video","sound"}`), each `options[key] =
  { title, key, items }`. Buttons are `UIUtil.CreateButtonStd(dialog, '/scx_menu/tab_btn/tab', ...)`, 180 px wide
  (texture `tab_btn_up.dds` 180 x 60), the first at x 10, each next one `RightOf(prev, -10)`. The panel
  (`/scx_menu/options/panel_bmp.dds`) is **700** px wide: 10 + 4 x 180 - 3 x 10 = 700, **four tabs fill the bar
  exactly. A fifth would end at 870, 170 px outside the panel.** No limit in code, but a fifth tab needs narrower
  buttons. The chosen tab is drawn as a separate Bitmap `tab_btn_selected.dds` (also 180) centred on the hidden
  button (`SetNewPage`).
- **Item types**: exactly `header`, `toggle` (the game's drop-down Combo, 250 wide), `button` (a small standard
  button on a line), `slider`. `header` = the row's box made invisible (`bg:SetAlpha(0)`), only its title shows;
  FAF uses it in Gameplay and Interface ("Camera controls", "HUD", ...). The table of types
  (`controlTypeCreate`) and `CreateOption` are module **locals**: a hook can add a type only through
  `debug.getupvalue` chains.
- **Scrolling**: one Grid of rows (row = `content-box_bmp` 604 x 36), a vertical scrollbar, the wheel scrolls one row.
  Each page is rebuilt from `tabData.items` when its tab is clicked (`SetNewPage`, a **global** set inside
  `CreateDialog`).
- **No collapsible section and no sub-dialog** exist. Both would be ours, built from hooks.
- **Tooltips**: the dialog shows `Tooltips["options_" .. key]` from `/lua/ui/help/tooltips.lua`
  (`Tooltip.CreateMouseoverDisplay(self, "options_" .. key, .5, true)`). FAF's `tip =` field is never read by the
  dialog (FAF's own items' tips live in tooltips.lua).
- **The one hard rule for any layout**: `optionslogic.SetValue` changes an option only when its control exists
  (`if optionItem.change and optionItem.control`), and controls exist only for the page on screen. Our Remastered
  Preset (`SetRemasterPreset`) and FAF's Fidelity Presets work through `SetValue`. **So every key a preset sets must
  be on the same page as the preset, and on screen (not collapsed).** Ours: 30 keys.
- A second working set is dangerous: the dialog keeps `currentOptionsSet` (a copy of all options taken when it
  opened) and OK / Apply save and `set` the WHOLE copy. Anything that changes options beside it while it is open
  (a second dialog with its own copy) is reverted by FAF's OK.

### 1.4 Found while reading: our tips never show (worth fixing in the same change)

Our 38 `tip =` texts are not in `Tooltips`, and FAF's dialog reads only `Tooltips["options_<key>"]`, so hovering
one of our option titles shows the key ("options_remaster_atmosphere") and "No Description" (and logs
`No tooltip in table for key`). Nothing in `ui/` registers them (searched `ui/data`, `ui/user`, the tooltips hook).
Confirm with one hover; fix: a loop at the end of the options hook,
`Tooltips['options_' .. item.key] = { title = item.title, description = item.tip }` for every item of ours with a
`tip` and no entry yet.

### 1.5 Who reads the option lists

- **Command palette** (`ui/data/lua/palette.lua` `Gather`, lines 171-188): walks `optionsOrder`, then each tab's
  `items` (skips `header`), keeps `tabKey`, `row = index` (the row in the grid, headers counted), searches the title,
  the key and the tab's title. `ShowOption` (604-643) finds the tab button by `control.tabData == options[tabKey]`,
  clicks it, finds the row by `_label._tipText == key`, `ScrollSetTop(row - 2)`. Works for any tab in
  `optionsOrder`, as long as rows are not hidden.
- **Tests**: `lobby.lua` 70-73 builds `video[key]` from `options.video.items` and uses it for ~60 checks (every
  remaster key, "the Video tab has ..."); 335 walks all tables. `screens.lua` 37 uses `options.video` only for FAF's
  adapters. `palette.lua` 122 ("vertical sync" is `option Video`, FAF's own Vertical Sync) and 229-237 (opens the
  dialog at a Video option). `colorvision.lua` 50, `pathpreview.lua` 62, `keybind_import.lua` 67 look in
  `options.ui`. `firststart.lua` uses only `RemasterPresetValues` / `optionslogic`. `keys.lua` 234-236 only console
  names. `postgame.lua` 103 only the pref value.

---

## 2. Grouping: 42 picture options in 8 sections

Order follows FAF's Video tab (screen first, then the preset, then the details), and goes from the world down to the
finish of the picture:

| Section (header) | Options | n |
|---|---|---|
| Display & Performance | Frame Rate, Low Latency, Graphics API, Upscaling (FSR 1), Temporal Upscaling (FSR 3), Frame Generation | 6 |
| Preset | Remastered Preset, Look | 2 |
| Sky & Light | Atmosphere, Cloud Layout, Storm Clouds, Night Sky, Haze & Mist, Soft Shadows, Ray Tracing | 7 |
| Ground & Weather | Rich Ground on Remastered Maps, Ground Detail, Tracks in Snow, Weather Effects, Season | 5 |
| Water | Water Ripples, Ocean Glitter, Sea Sparkle, Shore Soak | 4 |
| Effects & Battle | Remastered Effects, Effects Quality, Shell Tracers, Battle Effects, Scorch Glow, Energy Shields, Shield Bending, Remastered Gateway | 8 |
| Units & Readability | Close-up Detail, Unit Glow at Night, Unit Glow Colour, Hidden Unit Outlines, Ghost Look, Guide over Grass, Cloak Look, Stealth Veil | 8 |
| Picture & Camera | Photographic Look, Sharpen, Cinematic Lens Blur | 3 |
|  |  | **43** |

The six of "Display & Performance" are exactly the ones no preset level touches by design (the hook's own comment:
"choices of smoothness and screen, not looks"); they sit above the preset, so its tip "Sets the remastered looks
below together" stays true. All 30 preset keys are in the sections below it, on the same tab (the rule of 1.3).

Not moved (they are interface / game features, and FAF players look for them there): Interface keeps Colour Vision,
Path Preview, Score Screen Extras, Key Bindings; Gameplay keeps Simulation Order. Each gets a header
**"FA Remastered"** above it (fixes the "Casting tools" / "Cursor features" look of 1.1).

---

## 3. The four ways compared

| | (a) one "Remastered" tab, FAF's headers | (b) Remastered tab, collapsible sections | (c) several new tabs | (d) "Remastered..." button, own dialog |
|---|---|---|---|---|
| Game's look kept | Yes: FAF's own tab, rows and headers (FAF's Gameplay tab looks the same). Only change: tab buttons 146 px instead of 180 (art stretched to 81 %). | Partly: no such control in the game; a header that opens would be our new control, or FAF's `button` row ("Show"/"Hide"), which looks like an option, not a header. | No: 8 more tabs at ~65 px each, or two rows of tabs; neither exists in the game. | Yes if built from the same textures; but a dialog over a dialog, two OK/Cancel rows. |
| Findability | All picture options in one place, grouped; the palette unchanged and can add the section name; the old place gets a signpost. | Closed sections hide what exists; the palette must open the section first. | Spread over tabs; palette fine. | One click deeper; the palette needs a second way in (open the dialog, then the sub-dialog). |
| Presets | Work unchanged (all 30 keys on the tab, all controls exist). | **Break** for closed sections (`SetValue` needs a control) unless we write FAF's `currentOptionsSet` through `debug.getupvalue`. | **Break**: the preset's keys spread over tabs; same fix needed. | Need the sub-dialog to share FAF's `currentOptionsSet` (upvalue), or FAF's OK reverts what the sub-dialog applied. |
| FAF's dialog code | Unchanged. One small hook: lay out the tab buttons for 5 tabs. | Unchanged, but a new row type through local upvalue chains (`SetNewPage` -> `CreateOption` -> `controlTypeCreate`), page rebuilt on every click, open/closed state kept somewhere. | Unchanged + the tab-bar problem x8. | Unchanged, but a whole second dialog (grid, scrollbar, buttons, modal input, Escape) re-built by us. |
| Effort | Small: ~120 lines of Lua + tests. | Medium-large. | Medium, and the result is worse. | Large. |
| Risk | Low: the tab label must fit (testable); a later FAF change to the dialog's locals only loses the narrowing. | High (presets, palette rows, state). | High (presets). | Medium-high (two working sets). |
| What moves for a player who knows today's place | The 42 leave the bottom of Video for the next tab; a "Show" button stays where they were; values kept. | Same, plus hidden behind a click. | Spread over 8 tabs. | Behind a button at the bottom of Video. |
| FAF's video settings | Video = exactly FAF's own again (12 rows). | Same. | Same. | Same. |

Chris's "drop down menus that open different remaster options" maps best onto the game's own drop-down (the
`toggle` Combo) used as **"Go to Section"**: it scrolls the Remastered tab to the chosen header. It hides nothing,
so presets and the palette are untouched. It is part of the recommendation as its last, separable step.

---

## 4. Recommendation: (a), a "Remastered" tab after Video, with FAF's headers and a "Go to Section" drop-down

### 4.1 Layout (mock, panel 700 px; header rows = text only, option rows = FAF's box, control at x 338)

```
+---------------------------------------------------------------------------------+
|                                    OPTIONS                                      |
| [GAMEPLAY][INTERFACE][ VIDEO ][REMASTERED][ SOUND ]      (5 tabs x 146 px)      |
|  +---------------------------------------------------------------------------+ |
|  | Go to Section                              [ Display & Performance    v ] |^|
|  DISPLAY & PERFORMANCE                                                        | |
|  | Frame Rate                                 [ Vertical Sync            v ] | |
|  | Low Latency                                [ On                       v ] | |
|  | Graphics API                               [ Direct3D 12              v ] | |
|  | Upscaling (FSR 1)                          [ Off                      v ] | |
|  | Temporal Upscaling (FSR 3)                 [ Off                      v ] | |
|  | Frame Generation                           [ Off                      v ] | |
|  PRESET                                                                       | |
|  | Remastered Preset                          [ Ultra                    v ] |v|
|  +---------------------------------------------------------------------------+ |
|                                      [Reset] [ OK ] [Cancel] [Apply]            |
+---------------------------------------------------------------------------------+
   scrolled on:
  | Look                                       [ Remastered               v ] |
  SKY & LIGHT         Atmosphere, Cloud Layout, Storm Clouds, Night Sky, Soft Shadows, Ray Tracing
  GROUND & WEATHER    Rich Ground on Remastered Maps, Ground Detail, Tracks in Snow, Weather Effects, Season
  WATER               Water Ripples, Ocean Glitter, Sea Sparkle, Shore Soak
  EFFECTS & BATTLE    Remastered Effects, Effects Quality, Shell Tracers, Battle Effects, Scorch Glow,
                      Energy Shields, Shield Bending, Remastered Gateway
  UNITS & READABILITY Close-up Detail, Unit Glow at Night [slider], Unit Glow Colour, Hidden Unit Outlines,
                      Ghost Look, Guide over Grass, Cloak Look, Stealth Veil
  PICTURE & CAMERA    Photographic Look, Sharpen [slider], Cinematic Lens Blur
```

Remastered tab: 1 + 8 headers + 43 options = 52 rows (about 5 pages; "Go to Section", the wheel and the palette
get anywhere in one step).

Video tab: FAF's 12 rows as FAF has them, then

```
  FA REMASTERED
  | Picture options: the Remastered tab        [  Show  ]   |   (FAF's 'button' type; clicks the Remastered tab)
```

Interface tab: FAF's rows, then header "FA REMASTERED" and our 4 rows as now. Gameplay: FAF's rows, then header
"FA REMASTERED" and Simulation Order for New Games.

Why after Video and not at the end: picture options next to picture options; the tab positions all shift anyway
with the narrower buttons. Tab title "Remastered" (as the preset and the Look call it); if the test of 4.3 finds the
label too wide in the game's title font at 146 px, "Remaster", or the five labels at 14 instead of 16.

### 4.2 Code changes

1. `ui/data/hook/lua/options/options.lua`
   - Replace the loop that appends `looks` to `options.video.items` (lines 819-821) with: a `remasterSections` list
     (title + keys, section 2's table), a `byKey` map of `looks`, one `{ title = ..., type = 'header', key = '',
     default = '' }` per section (FAF's header convention) followed by its items, built into
     `options.remaster = { title = "Remastered", key = 'remaster', items = ... }`; any look not named in a section
     appended under a last header "More" (nothing is ever lost; a test keeps that section empty);
     `table.insert(optionsOrder, <index of 'video'> + 1, 'remaster')`.
   - The ray-tracing default metatable loop (811-818) stays as it is (it works on `looks`).
   - After the `leftOut` removal (936-941): append to `options.video.items` the header "FA Remastered" and the
     signpost `{ title = "Picture options: the Remastered tab", key = 'remaster_show_tab', type = 'button',
     default = 0, set = function() end, update = function() RemasterShowOptionsTab('remaster') end,
     custom = { text = "Show" } }`. `RemasterShowOptionsTab(key)`: the tab button whose `tabData ==
     options[key]` and not hidden, `OnClick()` (as palette's `OptionsTab`).
   - Before the 4 `table.insert(options.ui.items, ...)` and the one into `options.gameplay.items`: a header
     "FA Remastered" each.
   - At the end: the tooltip loop of 1.4 (over `options.remaster`, our Interface / Gameplay items and the signpost).
   - The "Go to Section" row as the first item of `options.remaster`: `type = 'toggle'`, `key =
     'remaster_goto_section'`, states = the 8 section titles (keys 1..8), `default = 1`, `set` doing nothing,
     `update(control, value)`: the grid is the row's parent chain (combo -> control group -> row box -> grid);
     `grid:ScrollSetTop('Vert', <row of that header>)` (the index in `options.remaster.items`, as the palette uses
     `row`); `init`: set the combo back to the first section without `update`
     (`item.change(item.control, 1, true)`), so it never shows a stale choice when the tab opens.
   - Comments at the top (lines 1-6) and the README's options paragraph (`ui/README.md` ~218): "Video tab" -> "the
     Remastered tab".
   - Optional, same change or later: each preset key's `update` sets `remaster_preset` to Custom (5) when the value
     leaves the level, as FAF's Fidelity / Shadow Fidelity do for `fidelity_presets` (today changing Atmosphere by
     hand leaves the preset showing "Ultra"). Done 2026-11-09 (helper flight2): `RemasterPresetLookChanged` in the
     options hook, checked by `options_tabs.lua`.

2. New hook `ui/data/hook/lua/ui/dialogs/options.lua` (~40 lines, the pattern of the keybindings hook):
   - Wrap `CreateDialog(over, exitBehavior)`: call FAF's, then `RemasterFitTabs()`: n = number of tab buttons;
     width = min(180, floor((panelWidth - 10 + 10 * (n - 1)) / n)) with panelWidth from
     `GetTextureDimensions(UIUtil.UIFile('/scx_menu/options/panel_bmp.dds'))` (n = 4 gives 180: no change; 5 gives
     146); `LayoutHelpers.SetWidth` each button; the first `AtLeftTopIn(dialog, 10, 64)`, each next
     `RightOf(prev, -10)` (FAF's own placing). The dialog is FAF's module local `dialog`, read with
     `debug.getupvalue(FAF's CreateDialog, 'dialog')`; the buttons are its children with `tabData` and `OnClick`.
   - `SetNewPage` is a global that `CreateDialog` sets each time: after FAF's `CreateDialog`, wrap the new one so
     that, after it, the lit tab's Bitmap (`currentTabBitmap`, an upvalue of `SetNewPage`) gets the button's
     width; and size the one FAF's `CreateDialog` already made for the first tab.
   - If an upvalue is not found (a later FAF changes the dialog), leave the layout as FAF's (a WARN, no error).
   - FAF's `lua/ui/dialogs/options.lua` itself stays untouched; nothing of FAF's code is copied.

3. `ui/data/lua/palette.lua` `Gather` (171-188): while walking a tab's `items`, remember the last `header`'s
   title; put it on the entry (`section`) and into its search words (so "water" finds Shore Soak); the detail line
   (273-275): "Options > Remastered > Water | On". `ShowOption` needs no change.

### 4.3 Test changes

- `ui/tests/lobby.lua` 70-73: build `video` from `options.video.items` AND `options.remaster.items` (one map;
  headers have key `''`); "the Video tab has X" -> "the Remastered tab has X" (the checks stay). New checks:
  - `optionsOrder` is gameplay, ui, video, remaster, sound;
  - `options.video` has no `remaster_*` key except `remaster_show_tab` (Video is FAF's own again);
  - every key of `RemasterPresetValues(4)` and `remaster_preset` are in `options.remaster` (the presets' rule);
  - every key is in exactly one tab; the "More" section does not exist; 8 headers, none empty;
  - every option of ours with a `tip` has `Tooltips['options_' .. key]` with that text.
- `ui/tests/palette.lua`: entries count check (99) still holds; add `query("ocean glitter")` -> `option
  Remastered`, detail with "Water"; `query("water")` lists Ocean Glitter among options (section words); after
  Enter on it, the lit tab is the one with `tabData == options.remaster` and `shownOption ==
  'remaster_ocean_glitter'` (the same steps as 222-238). The "vertical sync" check (122) stays "option Video".
- New `ui/tests/options_tabs.lua` (front end, `--lua`): open the dialog (`CreateDialog(GetFrame(0))`);
  5 tab buttons, each inside the dialog's Left..Right, neighbours overlapping 10 px, each label's width <= its
  button's width - 12; the lit Bitmap as wide as its button; Remastered tab clicked: 52 rows; the preset's combo set
  to Low through its `OnClick` -> `currentOptionsSet` (read as the palette reads controls) has Sharpen 0, Shore Soak 0,
  Atmosphere 0 (the preset works on its new page); "Go to Section" Water -> the grid's top row is the Water header;
  Video tab: its last row's button clicks over to the Remastered tab; Cancel. A picture with `--screenshot` for
  Chris (the stretched tab art, the headers).
- Unchanged: `colorvision.lua`, `pathpreview.lua`, `keybind_import.lua` (Interface items stay), `screens.lua`
  (FAF's adapters stay in Video), `firststart.lua`, `keys.lua`, `postgame.lua`.
- Note: the README's pixel check of the options dialog against the original (README ~558) will differ in the tab
  bar from now on, by design.

### 4.4 If Chris wants literally everything in the tab

Variant: also move our Interface 4 and Gameplay 1 into the Remastered tab as two last sections "Interface" and
"Game". Technically the same (flat prefs, no preset involved); then `colorvision.lua` 50, `pathpreview.lua` 62 and
`keybind_import.lua` 67 read `options.remaster` instead of `options.ui`. Not recommended: a colour-blind or FAF
player looks for Colour Vision and the key import in Interface, and that tab is not crowded.

## 5. "Tab Defaults" (built 2026-11-13; Chris: "lets add them")

The review of 2026-11-09 (section 5, "In the options"): FAF's one Reset puts every option of every tab back at once
(`optionslogic.ResetToDefaults`), the sound's and the gameplay's with the graphics', after a question, and closes the
dialog. Ours adds a fifth button left of it, "Tab Defaults" (`ui/data/hook/lua/ui/dialogs/options.lua`,
`RemasterTabDefaults`): each row of the tab SHOWN is changed to its default the way a hand changes it (the dialog's own
`item.change`: the control, the dialog's working set, and the option's own `update`, a slider's preview). So nothing
is kept until OK or Apply, Cancel leaves the options as they were, and no question is asked. Headers and button rows
have nothing to put back. On the Remastered tab the preset goes to its default (Ultra) and each look to its own.

The game's four buttons fill the row (170 wide each, 6 over the next, 15 from the panel's right edge), so a fifth would
stand 143 outside the panel on the left: the five get 138 each, the textures drawn narrower, as the tab buttons
(section 4). Tooltip: `options_tab_defaults` (ui/data/hook/lua/ui/help/tooltips.lua). Test: `ui/tests/options_tabs.lua`
(every option of the tab at its default in the working set, another tab's option untouched, nothing kept before OK,
Cancel leaves all, every button's label inside its button; at 1920x1080 and 3840x2160).
