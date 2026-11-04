-- A fix for FreyaGroup's "Ultra Shields" mod (v3.5), made for FA Remastered. It holds none of the mod's files: it only
-- changes two of its blueprints after they are loaded (hook/lua/system/Blueprints.lua). README.md says what and why.
name = "Ultra Shields fix (FA Remastered)"
uid = "0c5a9aee-2f06-451c-ab53-5b2e2e906a3e"
version = 1
copyright = "MIT, as FA Remastered's engine. Ultra Shields itself is FreyaGroup's and is not included."
description = "For Ultra Shields 3.5: its two UEF experimental shields (ESD Pulse) are the same shield. Keeps feb4402 (more hull, faster build) and lets T3 engineers and commanders build it as well as upgrade to it; feb4401 is no longer buildable. Needs Ultra Shields."
author = "FA Remastered"
url = ""
selectable = true
enabled = true
exclusive = false
ui_only = false
requires = { "E08710C8-B785-11E4-9CA4-35FF1C5D46B0" }
requiresNames = { ["E08710C8-B785-11E4-9CA4-35FF1C5D46B0"] = "Ultra Shields" }
conflicts = { }
before = { }
after = { "E08710C8-B785-11E4-9CA4-35FF1C5D46B0" }
