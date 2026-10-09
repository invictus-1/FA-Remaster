#!/usr/bin/env python3
"""Compare the oracle probe's combat scenarios (probe v4, "PROBE cb..." lines) between the
original game's log and moho64's log.

usage: compare_combat.py <original log> <moho64 log> [--tag TAG]
Per test unit: first tick it has a target, shots (count, first ticks), damage taken (events,
total), death tick; per shot: launch position/orientation of the first shots; impacts by type.
"""
import sys, collections

def load(path):
    d = {'cb': collections.defaultdict(list), 'shot': collections.defaultdict(list), 'dmg': collections.defaultdict(list),
         'dead': {}, 'impact': collections.Counter(), 'proj': collections.defaultdict(list)}
    for line in open(path, errors='replace'):
        i = line.find('PROBE ')
        if i < 0:
            continue
        f = line[i + 6:].split()
        if not f:
            continue
        k = f[0]
        try:
            if k == 'cb':
                d['cb'][f[2]].append((int(f[1]), f[3:]))
            elif k == 'cbshot':
                d['shot'][f[2]].append((int(f[1]), f[3], int(f[4]), [float(x) for x in f[6:13]]))
            elif k == 'cbdmg':
                d['dmg'][f[2]].append((int(f[1]), float(f[3]), f[4], f[5]))
            elif k == 'cbdead':
                d['dead'][f[2]] = int(f[1])
            elif k == 'cbimpact':
                d['impact'][(f[3], f[4])] += 1
            elif k == 'cbproj':
                d['proj'][int(f[2])].append((int(f[1]), [float(x) for x in f[3:6]]))
        except (IndexError, ValueError):
            pass
    return d

def first_target(rows):
    for t, v in rows:
        if v[4] != '-':
            return t, v[4]
    return None, None

def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    tag = sys.argv[sys.argv.index('--tag') + 1] if '--tag' in sys.argv else None
    tags = sorted(set(a['cb']) | set(b['cb']))
    print('%-12s %-22s %-22s %-24s %-24s %s' % ('unit', 'first target o/m', 'shots o/m', 'first shots o', 'first shots m', 'dead o/m'))
    for t in tags:
        if tag and t != tag:
            continue
        fa, fb = first_target(a['cb'][t]), first_target(b['cb'][t])
        sa, sb = a['shot'].get(t, []), b['shot'].get(t, [])
        print('%-12s %-22s %-22s %-24s %-24s %s/%s' % (
            t, '%s/%s' % (fa[0], fb[0]), '%d/%d' % (len(sa), len(sb)),
            ','.join(str(s[0]) for s in sa[:4]), ','.join(str(s[0]) for s in sb[:4]),
            a['dead'].get(t, '-'), b['dead'].get(t, '-')))
        da, db = a['dmg'].get(t, []), b['dmg'].get(t, [])
        if da or db:
            print('%14s damage o: %d events %.1f total (first %s)  m: %d events %.1f total (first %s)' % (
                '', len(da), sum(x[1] for x in da), da[0][0] if da else '-', len(db), sum(x[1] for x in db),
                db[0][0] if db else '-'))
        if tag:
            for sa1, sb1 in zip(sa[:5], sb[:5]):
                print('   shot o', sa1[0], ['%.3f' % x for x in sa1[3]])
                print('   shot m', sb1[0], ['%.3f' % x for x in sb1[3]])
            ra = dict(a['cb'][t]); rb = dict(b['cb'][t])
            for tick in sorted(set(ra) & set(rb))[:60]:
                print('  ', tick, 'o', ' '.join(ra[tick]), '| m', ' '.join(rb[tick]))
    print('impacts o:', dict(a['impact']))
    print('impacts m:', dict(b['impact']))

main()
