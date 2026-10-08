"""Compare the rules phase (blueprint loading) of our log with the original game's log.
usage: compare_rules_log.py original.log ours.log"""
import difflib, sys
def section(path):
    lines = open(path, encoding='latin1').read().replace('\r\n', '\n').split('\n')
    s = lines.index('info: Active game mods for blueprint loading:')
    e = next(i for i in range(s, len(lines)) if 'Blueprints Loading... completed:' in lines[i] and 'projectiles' in lines[i])
    # engine/OS noise that is not part of the rules state
    return [l for l in lines[s:e + 1] if l not in ('info: Minimized true', 'info: Minimized false')]
a, b = section(sys.argv[1]), section(sys.argv[2])
d = list(difflib.unified_diff(a, b, 'original', 'ours', n=0, lineterm=''))
print(f'original {len(a)} lines, ours {len(b)} lines, {sum(1 for l in d if l[:1] in "+-" and l[:3] not in ("+++","---"))} differing lines')
print('\n'.join(d[:60]))
sys.exit(1 if d else 0)
