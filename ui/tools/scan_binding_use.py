"""scan_binding_use.py - how often the game's scripts call each engine function of the user state.

    py ui/tools/scan_binding_use.py <gamedata folder> [--all]

Reads the names from ui/user/user_binding_names.inc and counts, in every .lua file of the game's
archives (*.nx2, *.scd: zip files), the files that call each one. A global counts when its name is
followed by "(", a method when ".Name(" or ":Name(" appears (methods of different classes that share
a name are counted together). Default: user-side scripts only (/lua/ui, /lua/maui, /lua/user,
/lua/keymap, /lua/options, /lua/lazyvar.lua, the top-level user files); --all scans every script.
Output: one line per function, most used first, then the functions no script calls.
"""
import os, re, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
USER_SIDE = re.compile(r"^lua/(ui|maui|user|keymap|options|multiplayer|modules/ui)/|^lua/[^/]+\.lua$", re.I)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gamedata, scan_all = sys.argv[1], "--all" in sys.argv
    names = []
    for line in open(os.path.join(HERE, "..", "user", "user_binding_names.inc")):
        m = re.match(r'\{"(\w+)", "(\w*)", "(\w+)"\},', line)
        if m and not m.group(3).startswith("__"):
            names.append((m.group(2), m.group(3)))
    wanted = {n for _, n in names}
    call = re.compile(rb"([.:]?)\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    files_of = {}  # (is method, name) -> set of files
    for arc in sorted(os.listdir(gamedata)):
        if not arc.lower().endswith((".nx2", ".scd")):
            continue
        z = zipfile.ZipFile(os.path.join(gamedata, arc))
        for n in z.namelist():
            if not n.lower().endswith(".lua") or not (scan_all or USER_SIDE.search(n)):
                continue
            for m in call.finditer(z.read(n)):
                name = m.group(2).decode()
                if name in wanted:
                    files_of.setdefault((bool(m.group(1)), name), set()).add(n)
    rows = []
    for cls, name in names:
        rows.append((len(files_of.get((bool(cls), name), ())), (cls + ":" if cls else "") + name))
    rows.sort(key=lambda r: (-r[0], r[1]))
    for count, name in rows:
        print("%4d  %s" % (count, name))
    print("%d functions, %d not called by any scanned script" % (len(rows), sum(1 for r in rows if r[0] == 0)))


if __name__ == "__main__":
    main()
