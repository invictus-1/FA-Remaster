"""symbolize_hang.py LOG EXE: the viewer's hang dump (MOHO64_HANG_DUMP=<seconds>, hang_dump.h) with moho64view's own frames by
function and line, by llvm-symbolizer (on the PATH: Visual Studio's LLVM tools). The exe names its symbols
moho64view.new.pdb (build.py links to that name, then renames it): a copy of moho64view.pdb by that name beside the exe."""
import re
import subprocess
import sys

log, exe = sys.argv[1], sys.argv[2]
text = open(log, encoding="utf-8", errors="replace").read()
for block in text.split("render: hang dump: thread ")[1:]:
    offs = re.findall(r"\[moho64view\.exe\+0x([0-9a-f]+)\]", block)
    if len(offs) < 3:
        continue
    print("thread", block.split("\n")[0], len(offs), "frames of ours")
    r = subprocess.run(["llvm-symbolizer", "--obj=" + exe, "--relative-address", "--functions=short", "--no-inlines"],
                       input="\n".join("0x" + o for o in offs[:40]), capture_output=True, text=True)
    lines = [l for l in r.stdout.split("\n") if l.strip()]
    for i in range(0, len(lines) - 1, 2):
        where = lines[i + 1].split("fa-remaster-render")[-1]
        print("   ", lines[i], " ", where)
