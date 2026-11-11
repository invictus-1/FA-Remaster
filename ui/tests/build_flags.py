"""build_flags.py <worktree> <a built output folder>

The build script's rule "an object compiled with other options is compiled again" (ui/build_ui.py, class Flags;
the review of 2026-11-09, row F). Nothing is compiled and the output folder is not changed:
  1. the record itself, on made-up jobs in a temporary folder: what is kept is found again; a line taken out
     before a compile is not known after; a cut file and another compiler's file count as nothing known;
  2. the real script with --dry-run on the built folder: 0 objects with the options it was built with (the default
     ones), exactly 1 with --mi-debug (mimalloc's one file), all with --no-debug, some but not all with --define.
The folder must be up to date with the worktree (build first): a source changed since counts as one more object.
Exit code: 0 all green, 1 otherwise. About 5 s.
"""
import json, os, re, subprocess, sys, tempfile

wt, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(wt, "ui"))
import build_ui

bad = 0


def check(what, got, want):
    global bad
    if got == want:
        print("TEST ok   " + what)
    else:
        bad += 1
        print(f"TEST FAIL {what}: got {got!r}, want {want!r}")


# 1. the record
with tempfile.TemporaryDirectory() as d:
    os.makedirs(os.path.join(d, "obj"))
    job = lambda name, *flags: (name + ".cpp", os.path.join(d, "obj", name + ".o"), ["python", "-m", "ziglang", "c++", *flags, "-c", name + ".cpp"])
    a, b = job("a", "-O2"), job("b", "-O2")
    f = build_ui.Flags(d)
    check("a folder without the file: nothing known", (f.known, f.differs(a)), (False, True))
    f.keep(a), f.keep(b), f.write()
    f = build_ui.Flags(d)
    check("what was kept is found again", (f.known, f.differs(a), f.differs(b)), (True, False, False))
    check("another option: differs", f.differs(job("a", "-O2", "-DX")), True)
    check("another Python running zig: the same", f.differs((a[0], a[1], ["py3", "-m", "ziglang"] + a[2][3:])), False)
    check("the same folder spelled another way: the same", f.differs((a[0], a[1].replace(os.sep, "/"), a[2] + ["-o", a[1].replace(os.sep, "/")])), False)
    f.forget([a])
    f = build_ui.Flags(d)
    check("a line taken out before its compile is not known after (a build stopped half-way)", (f.differs(a), f.differs(b)), (True, False))
    data = json.load(open(f.path, encoding="utf-8"))
    data["compiler"] = "another compiler"
    json.dump(data, open(f.path, "w", encoding="utf-8"))
    check("another compiler's record: nothing known of any object", build_ui.Flags(d).differs(b), True)
    open(f.path, "w").write('{"compiler": "zig')
    f = build_ui.Flags(d)
    check("a cut file: nothing known, and no error", f.differs(b), True)


# 2. the real script, dry
def dry(*options):
    r = subprocess.run([sys.executable, os.path.join(wt, "ui", "build_ui.py"), "--out", out, "--dry-run", *options], capture_output=True, text=True)
    m = re.search(r"build_ui: (\d+) of (\d+) objects to compile", r.stdout)
    if not m or "nothing compiled" not in r.stdout:
        print("TEST note: the script said: " + r.stdout[-400:] + r.stderr[-400:])
        return (-1, -1)
    return (int(m.group(1)), int(m.group(2)))


before = sorted((n, os.path.getmtime(os.path.join(out, "obj", n))) for n in os.listdir(os.path.join(out, "obj")))
todo, total = dry()
check("the options the folder was built with: no object to compile (it must be built from this worktree as it is: build first)", todo, 0)
check("--mi-debug: one object (mimalloc's)", dry("--mi-debug")[0], 1)
check("--no-debug: every object", dry("--no-debug")[0], total)
n = dry("--define", "MOHO64UI_FLAGS_TEST")[0]
check("--define: the files it reaches, not all and not none (%d of %d)" % (n, total), 0 < n < total, True)
check("the dry runs changed nothing in the folder", sorted((n, os.path.getmtime(os.path.join(out, "obj", n))) for n in os.listdir(os.path.join(out, "obj"))), before)
print(f"TEST done: {bad} failed")
sys.exit(1 if bad else 0)
