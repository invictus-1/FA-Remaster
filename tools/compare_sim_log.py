"""Compare the sim start-up of our log with the original game's log.
Section: from the map loader line to the end of our log (or --until <text> in the original).
usage: compare_sim_log.py original.log ours.log [--context N]"""
import difflib, sys
NOISE = ('info: Minimized true', 'info: Minimized false')
def lines(path):
    return open(path, encoding='latin1').read().replace('\r\n', '\n').split('\n')
def section(ls, ours):
    s = next(i for i, l in enumerate(ls) if 'Background task "Map loader' in l and 'finished' in l)
    out = []
    for l in ls[s:]:
        if l in NOISE or l.startswith('debug: MEM:') or 'moho64:' in l: continue
        if l.startswith('debug: Gametime') or l.startswith('debug: Session time'): break  # timed engine stats
        out.append(l)
    return out
a, b = section(lines(sys.argv[1]), False), section(lines(sys.argv[2]), True)
while b and not b[-1]: b.pop()
a = a[:len(b) + 50]
d = list(difflib.unified_diff(a, b, 'original', 'ours', n=0, lineterm=''))
diff = sum(1 for l in d if l[:1] in '+-' and l[:3] not in ('+++', '---'))
print(f'original {len(a)} lines (cut at ours+50), ours {len(b)} lines, {diff} differing lines')
print('\n'.join(d[:int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 80]))
