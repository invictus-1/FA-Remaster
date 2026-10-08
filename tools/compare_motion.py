#!/usr/bin/env python3
"""Compare the oracle probe's movement scenarios (PROBE mv lines) between the original and moho64.
usage: compare_motion.py original.log moho64.log [--tag NAME] [--ticks a-b]
Per test unit: when it starts moving, when its commands end, where it ends, and the position
error per tick (horizontal and height) and the heading error."""
import sys, math, collections, argparse

def load(path):
    rows = collections.defaultdict(dict)
    for line in open(path, encoding='latin-1'):
        i = line.find('PROBE mv ')
        if i < 0:
            continue
        f = line[i + 9:].split()
        if len(f) < 15:
            continue
        t = int(f[0])
        rows[f[1]][t] = dict(p=tuple(map(float, f[2:5])), q=tuple(map(float, f[5:9])),
                             v=tuple(float(x) if x != 'nil' else 0.0 for x in f[9:12]),
                             layer=f[12], moving=int(f[13]), ncmd=int(f[14]))
    return rows

def heading(q):
    x, y, z, w = q
    return math.atan2(2 * (w * y + x * z), 1 - 2 * (y * y + x * x))

def summary(r):
    ticks = sorted(r)
    first = next((t for t in ticks if t > 40 and abs(r[t]['v'][0]) + abs(r[t]['v'][2]) > 0), None)
    done = [t for t in ticks[1:] if r[t]['ncmd'] < r[ticks[ticks.index(t) - 1]]['ncmd']]
    return first, done, r[ticks[-1]]['p']

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('orig'); ap.add_argument('ours')
    ap.add_argument('--tag'); ap.add_argument('--ticks')
    a = ap.parse_args()
    A, B = load(a.orig), load(a.ours)
    if a.tag:
        lo, hi = map(int, a.ticks.split('-')) if a.ticks else (0, 10 ** 9)
        ra, rb = A[a.tag], B[a.tag]
        for t in sorted(set(ra) | set(rb)):
            if not lo <= t <= hi: continue
            x, y = ra.get(t), rb.get(t)
            def fm(d):
                if not d: return '-' * 40
                return '%9.4f %8.4f %9.4f h%7.2f v%7.4f,%7.4f m%d c%d' % (d['p'] + (math.degrees(heading(d['q'])), d['v'][0], d['v'][2], d['moving'], d['ncmd']))
            err = ''
            if x and y:
                err = 'dxz %.4f dy %.4f' % (math.hypot(x['p'][0] - y['p'][0], x['p'][2] - y['p'][2]), y['p'][1] - x['p'][1])
            print(t, '|', fm(x), '|', fm(y), '|', err)
        return
    print('%-14s %-12s %-22s %-24s %-24s %s' % ('unit', 'first move', 'commands end', 'final (orig)', 'final (ours)', 'xz err: mean/max   y err max   heading err max'))
    for tag in A:
        if tag not in B: print(tag, 'missing in ours'); continue
        fa, da, pa = summary(A[tag]); fb, db, pb = summary(B[tag])
        errs, yerr, herr = [], [], []
        for t in A[tag]:
            if t in B[tag] and t >= 21:
                x, y = A[tag][t], B[tag][t]
                errs.append(math.hypot(x['p'][0] - y['p'][0], x['p'][2] - y['p'][2]))
                yerr.append(abs(x['p'][1] - y['p'][1]))
                d = abs(heading(x['q']) - heading(y['q'])); d = min(d, 2 * math.pi - d)
                herr.append(math.degrees(d))
        m = sum(errs) / len(errs) if errs else 0
        print('%-14s %4s / %-5s  %-10s/ %-10s  %8.2f,%8.2f      %8.2f,%8.2f      %6.3f / %6.3f   %6.3f   %6.2f' % (
            tag, fa, fb, ','.join(map(str, da)), ','.join(map(str, db)), pa[0], pa[2], pb[0], pb[2], m, max(errs or [0]), max(yerr or [0]), max(herr or [0])))

main()
