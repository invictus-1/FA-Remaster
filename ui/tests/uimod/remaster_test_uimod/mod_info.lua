-- FA Remastered's own test mod (ui/tests/uimod.lua): a mod of the interface alone, with one hook. It does nothing
-- for a player; the suite mounts its folder with --mods-folder and switches it on to see that a UI mod's hook runs in a game.
name = "FA Remastered test UI mod"
uid = "fa-remastered-test-uimod-0001"
version = 1
copyright = "MIT, as FA Remastered"
description = "A test mod of FA Remastered's interface suite: one hook that sets a value. Not for play."
author = "FA Remastered"
url = ""
selectable = true
enabled = true
exclusive = false
ui_only = true
requires = {}
requiresNames = {}
conflicts = {}
before = {}
after = {}
