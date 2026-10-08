"""Compare the full blueprint dumps of two probe logs (ours, original): fields the original
adds, removes or changes. usage: compare_probe_bps.py ours.log original.log"""
import re, sys, collections
def load(path):
    bps = collections.defaultdict(dict)
    for l in open(path, encoding='latin1'):
        l = l.rstrip('\r\n')
        m = re.match(r'info: PROBE bp (\S+?)\.(\S+) = (.*)$', l)
        if not m:
            m2 = re.match(r'info: PROBE bp (\S+) missing', l)
            continue
        bid, path, val = m.groups()
        # ids containing dots (projectile paths) -> split at '.bp.' or mesh
        if bid.endswith('.bp') is False and '/' in bid and '.bp' in l.split(' = ')[0]:
            k = l.split(' = ')[0][len('info: PROBE bp '):]
            i = k.index('.bp.') + 3
            bid, path = k[:i], k[i+1:]
        bps[bid][path] = val
    return bps
ours, orig = load(sys.argv[1]), load(sys.argv[2])
added = collections.defaultdict(list); removed = collections.defaultdict(list); changed = collections.defaultdict(list)
for bid in orig:
    o, m = orig[bid], ours.get(bid, {})
    for p, v in o.items():
        g = re.sub(r'\.\d+(?=\.|$)', '.[]', p)
        if p not in m: added[g].append((bid, v))
        elif m[p] != v: changed[g].append((bid, m[p], v))
    for p, v in m.items():
        g = re.sub(r'\.\d+(?=\.|$)', '.[]', p)
        if p not in o: removed[g].append((bid, v))
print('bps', len(orig), 'ours', len(ours))
for name, d in (('ADDED by engine', added), ('REMOVED', removed), ('CHANGED', changed)):
    print('=====', name, len(d))
    for g in sorted(d):
        vals = d[g]
        uniq = collections.Counter(x[-1] for x in vals)
        print(f'{g}  n={len(vals)}  ' + ('; '.join(f'{v} x{c}' for v, c in uniq.most_common(4))) + (f'  ex={vals[0][0]}' if name=='CHANGED' else ''))
