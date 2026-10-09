#!/usr/bin/env python3
"""Compare the oracle probe's transport phase ("PROBE tr..." lines) of two logs.

usage: compare_transport.py ORIGINAL.log OURS.log [--every N] [--tags tr,c_tank1,...] [--tol 0.05]

Prints the event lines (trcb / trlayer / trdead) side by side, then for every tagged unit the first
tick its position, layer, queue length or unit states differ (position beyond --tol), and a table
every N ticks.
"""
import argparse, math, re, sys

def load(path):
    ev, tru = [], {}
    for line in open(path, encoding='latin-1'):
        i = line.find('PROBE tr')
        if i < 0:
            continue
        f = line[i + 6:].split()
        kind = f[0]
        if kind == 'tru':
            tick, tag = int(f[1]), f[2]
            x, y, z = float(f[3]), float(f[4]), float(f[5])
            layer, q = f[6], int(f[7])
            rest = f[8:]
            states = ''
            extra = ''
            for r in rest:
                if r.startswith('cargo='):
                    extra = r
                else:
                    states = r
            tru.setdefault(tag, {})[tick] = (x, y, z, layer, q, states, extra)
        elif kind in ('trcb', 'trlayer', 'trdead', 'trorders', 'trspawn'):
            ev.append((int(f[1]), kind, ' '.join(f[2:]) if kind != 'trspawn' else ' '.join(f[2:4])))
    return ev, tru

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('orig'); ap.add_argument('ours')
    ap.add_argument('--every', type=int, default=10)
    ap.add_argument('--tags', default='')
    ap.add_argument('--tol', type=float, default=0.05)
    ap.add_argument('--from', dest='t0', type=int, default=0)
    ap.add_argument('--to', dest='t1', type=int, default=10**9)
    a = ap.parse_args()
    eo, to = load(a.orig)
    eu, tu = load(a.ours)
    print('== events (original | ours)')
    so = set((t, k, s) for t, k, s in eo)
    su = set((t, k, s) for t, k, s in eu)
    for t, k, s in sorted(so | su):
        mark = '  =  ' if (t, k, s) in so and (t, k, s) in su else (' orig' if (t, k, s) in so else ' ours')
        print(f'{mark} {t:5d} {k:8s} {s}')
    tags = a.tags.split(',') if a.tags else sorted(set(to) | set(tu), key=lambda x: (x != 'tr', x))
    print('\n== first differences (tol %.3g)' % a.tol)
    for tag in tags:
        o, u = to.get(tag, {}), tu.get(tag, {})
        first = {}
        for t in sorted(set(o) | set(u)):
            if t < a.t0 or t > a.t1:
                continue
            if t not in o or t not in u:
                first.setdefault('presence', t)
                continue
            po, pu = o[t], u[t]
            d = math.dist(po[:3], pu[:3])
            if d > a.tol: first.setdefault('pos', (t, round(d, 3)))
            if po[3] != pu[3]: first.setdefault('layer', (t, po[3], pu[3]))
            if po[4] != pu[4]: first.setdefault('queue', (t, po[4], pu[4]))
            if po[5] != pu[5]: first.setdefault('states', (t, po[5] or '-', pu[5] or '-'))
            if po[6] != pu[6]: first.setdefault('cargo', (t, po[6], pu[6]))
        print(f'{tag:8s}', first if first else 'identical')
    print('\n== every %d ticks: tick tag | original x y z layer q states | ours ... | dist' % a.every)
    allt = sorted(set(t for d in list(to.values()) + list(tu.values()) for t in d))
    for t in allt:
        if t < a.t0 or t > a.t1 or (t - allt[0]) % a.every:
            continue
        for tag in tags:
            po, pu = to.get(tag, {}).get(t), tu.get(tag, {}).get(t)
            fo = '%.2f %.2f %.2f %s %d %s %s' % (po[0], po[1], po[2], po[3], po[4], po[5] or '-', po[6]) if po else '-'
            fu = '%.2f %.2f %.2f %s %d %s %s' % (pu[0], pu[1], pu[2], pu[3], pu[4], pu[5] or '-', pu[6]) if pu else '-'
            d = '%.2f' % math.dist(po[:3], pu[:3]) if po and pu else ''
            print(f'{t:5d} {tag:8s} | {fo:58s} | {fu:58s} | {d}')

if __name__ == '__main__':
    main()
