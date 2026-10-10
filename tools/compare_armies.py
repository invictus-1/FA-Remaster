#!/usr/bin/env python3
"""Compare the whole game per army (probe v19 'PROBE ar' lines) between the original and moho64.
usage: compare_armies.py original.log moho64.log [--every 300]
Per army, at every N ticks (nearest logged tick): army value (mass of finished units), unit counts and mass income,
original vs ours; then the mean relative difference of the army value over the game."""
import sys, argparse, collections

FIELDS = ['mi', 'ei', 'ms', 'es', 'n', 'eng', 'land', 'air', 'naval', 'struct', 'fac', 'val', 'cx', 'cz']

def load(path):
    d = collections.defaultdict(dict)
    for line in open(path, encoding='latin-1'):
        i = line.find('PROBE ar ')
        if i < 0:
            continue
        f = line[i + 9:].split()
        if len(f) < 16:
            continue
        t, a = int(f[0]), int(f[1])
        row = {}
        for k, v in zip(FIELDS, f[2:]):
            row[k] = None if v == '-' else float(v)
        d[a][t] = row
    return d

def near(rows, t):
    ks = sorted(rows)
    return min(ks, key=lambda k: abs(k - t)) if ks else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('orig'); ap.add_argument('ours'); ap.add_argument('--every', type=int, default=300)
    a = ap.parse_args()
    O, M = load(a.orig), load(a.ours)
    for army in sorted(O):
        o, m = O[army], M.get(army, {})
        if not m:
            print(f'army {army}: no data in ours'); continue
        last = max(o)
        print(f'== army {army}   (value / units / land / air / struct / mass income: orig | ours)')
        rel = []
        for t in range(a.every, last + 1, a.every):
            to, tm = near(o, t), near(m, t)
            ro, rm = o[to], m[tm]
            print(f'  {t:5d}  {ro["val"]:8.0f} {ro["n"]:4.0f} {ro["land"]:4.0f} {ro["air"]:3.0f} {ro["struct"]:4.0f} {ro["mi"]:6.2f}'
                  f'  |  {rm["val"]:8.0f} {rm["n"]:4.0f} {rm["land"]:4.0f} {rm["air"]:3.0f} {rm["struct"]:4.0f} {rm["mi"]:6.2f}')
        for t in sorted(o):
            tm = near(m, t)
            if o[t]['val'] and tm is not None:
                rel.append(abs(m[tm]['val'] - o[t]['val']) / o[t]['val'])
        if rel:
            print(f'  army value: mean relative difference {100 * sum(rel) / len(rel):.1f}%')

if __name__ == '__main__':
    main()
