"""colorvision_palettes.py - the Colour Vision option's army palettes (ui/user/color_vision.cpp), chosen and checked.

   py -I -X utf8 ui/tools/colorvision_palettes.py            the palettes, their order, and the distances
   py -I -X utf8 ui/tools/colorvision_palettes.py --cpp      the same, as the C++ table color_vision.cpp holds

Every colour comes from a published colour-blind-safe set (below, with its source). For each kind of colour vision
the 16 army slots are picked from those sets one at a time, each the candidate farthest (CIEDE2000) from the slots
already taken, as the eye of that kind sees them AND as normal vision sees them (the smaller of the two counts: a
player's friends look at the same screen, and many colour-blind players are anomalous trichromats, between the two).
So the first two slots are the most distinct pair, the first eight the best eight, and so on. High Contrast takes the
smallest distance over normal vision and all three dichromacies at once: one palette for every kind.

The eye of a dichromat is simulated with Machado, Oliveira and Fernandes 2009 ("A Physiologically-based Model for
Simulation of Color Vision Deficiency", IEEE TVCG 15(6)), their matrices at severity 1.0, on linear sRGB.
Colours darker than CIELAB L* 38 are left out: a team colour is painted on the models' masks and on the strategic
icons over dark ground, where a dark colour reads as black.
"""
import math, sys

# --- The published sets (only colours from these are used) ---------------------------------------------------------
SETS = {
    # Okabe & Ito 2008, "Color Universal Design (CUD): How to make figures and presentations that are friendly to
    # colorblind people", jfly.uni-koeln.de/color (black left out)
    "okabe-ito": ["E69F00", "56B4E9", "009E73", "F0E442", "0072B2", "D55E00", "CC79A7"],
    # Paul Tol 2021, "Colour Schemes", SRON/EPS/TN/09-002 (personal.sron.nl/~pault): bright, vibrant, muted, light,
    # high-contrast qualitative schemes
    "tol-bright": ["4477AA", "EE6677", "228833", "CCBB44", "66CCEE", "AA3377", "BBBBBB"],
    "tol-vibrant": ["EE7733", "0077BB", "33BBEE", "EE3377", "CC3311", "009988", "BBBBBB"],
    "tol-muted": ["CC6677", "332288", "DDCC77", "117733", "88CCEE", "882255", "44AA99", "999933", "AA4499", "DDDDDD"],
    "tol-light": ["77AADD", "EE8866", "EEDD88", "FFAABB", "99DDFF", "44BB99", "BBCC33", "AAAA00", "DDDDDD"],
    "tol-highcontrast": ["004488", "DDAA33", "BB5566", "FFFFFF"],
    # IBM Design Library's colour-blind-safe palette (Nichols, "Coloring for Colorblindness", davidmathlogic.com)
    "ibm": ["648FFF", "785EF0", "DC267F", "FE6100", "FFB000"],
    # Kelly 1965, "Twenty-two colors of maximum contrast", Color Engineering 3(6) (the NBS-ISCC centroids as
    # usually given; black left out)
    "kelly": ["F2F3F4", "F3C300", "875692", "F38400", "A1CAF1", "BE0032", "C2B280", "848482", "008856", "E68FAC",
              "0067A5", "F99379", "604E97", "F6A600", "B3446C", "DCD300", "882D17", "8DB600", "654522", "E25822",
              "2B3D26"],
}

# Machado et al. 2009, Table 1, severity 1.0 (linear RGB -> linear RGB)
MACHADO = {
    "protan": [[0.152286, 1.052583, -0.204868], [0.114503, 0.786281, 0.099216], [-0.003882, -0.048116, 1.051998]],
    "deutan": [[0.367322, 0.860646, -0.227968], [0.280085, 0.672501, 0.047413], [-0.011820, 0.042940, 0.968881]],
    "tritan": [[1.255528, -0.076749, -0.178779], [-0.078411, 0.930809, 0.147602], [0.004733, 0.691367, 0.303900]],
}


def lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hex_rgb(h):
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]


def to_lab(linrgb):
    r, g, b = [min(1.0, max(0.0, c)) for c in linrgb]
    x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    xn, yn, zn = 0.95047, 1.0, 1.08883

    def f(t):
        return t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116

    fx, fy, fz = f(x / xn), f(y / yn), f(z / zn)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def seen(h, kind):
    c = [lin(v) for v in hex_rgb(h)]
    if kind != "normal":
        m = MACHADO[kind]
        c = [sum(m[i][j] * c[j] for j in range(3)) for i in range(3)]
    return to_lab(c)


def de2000(a, b):
    L1, a1, b1 = a
    L2, a2, b2 = b
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    Cb = (C1 + C2) / 2
    G = 0.5 * (1 - math.sqrt(Cb ** 7 / (Cb ** 7 + 25 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360 if C1p else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360 if C2p else 0.0
    dLp, dCp = L2 - L1, C2p - C1p
    if C1p * C2p == 0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180:
        dhp = h2p - h1p
    elif h2p - h1p > 180:
        dhp = h2p - h1p - 360
    else:
        dhp = h2p - h1p + 360
    dHp = 2 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp) / 2)
    Lbp, Cbp = (L1 + L2) / 2, (C1p + C2p) / 2
    if C1p * C2p == 0:
        hbp = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        hbp = (h1p + h2p) / 2
    elif h1p + h2p < 360:
        hbp = (h1p + h2p + 360) / 2
    else:
        hbp = (h1p + h2p - 360) / 2
    T = (1 - 0.17 * math.cos(math.radians(hbp - 30)) + 0.24 * math.cos(math.radians(2 * hbp))
         + 0.32 * math.cos(math.radians(3 * hbp + 6)) - 0.20 * math.cos(math.radians(4 * hbp - 63)))
    dth = 30 * math.exp(-(((hbp - 275) / 25) ** 2))
    Rc = 2 * math.sqrt(Cbp ** 7 / (Cbp ** 7 + 25 ** 7))
    Sl = 1 + 0.015 * (Lbp - 50) ** 2 / math.sqrt(20 + (Lbp - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cbp, 1 + 0.015 * Cbp * T
    Rt = -math.sin(math.radians(2 * dth)) * Rc
    return math.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))


def dist(x, y, views):
    return min(de2000(seen(x, v), seen(y, v)) for v in views)


SLOTS = 16
# the option's kinds: (name, views the distance is the least of, the sets the colours come from, white allowed)
TOL_OI = ["okabe-ito", "tol-bright", "tol-vibrant", "tol-muted", "tol-light", "ibm"]
KINDS = [
    ("Deuteranopia", ["normal", "deutan"], TOL_OI, False),
    ("Protanopia", ["normal", "protan"], TOL_OI, False),
    ("Tritanopia", ["normal", "tritan"], TOL_OI, False),
    ("High Contrast", ["normal", "deutan", "protan", "tritan"], ["kelly", "tol-highcontrast", "okabe-ito", "tol-vibrant", "ibm"], True),
]
# The civilians' colour in every palette: Tol's pale grey (muted / light schemes), apart from the armies and from the
# unknown blips' mid grey.
CIVILIAN = "DDDDDD"
CIVILIAN_GAP = 15


def candidates(sets, white):
    out, seen_ = [], set()
    for s in sets:
        for h in SETS[s]:
            L, a, b = seen(h, "normal")
            grey = math.hypot(a, b) < 10  # (greys are the civilians' and the unknown blips': none for an army)
            if h not in seen_ and L >= 38 and (not grey or (white and L > 95)):
                seen_.add(h)
                out.append(h)
    return out


def pick(views, sets, civilian, white):
    # (none closer than CIVILIAN_GAP to the civilians' grey)
    cands = [c for c in candidates(sets, white) if dist(c, civilian, views) >= CIVILIAN_GAP]
    # the farthest pair first, then greedily the farthest from all taken
    best = max(((a, b) for i, a in enumerate(cands) for b in cands[i + 1:]), key=lambda p: dist(p[0], p[1], views))
    taken = list(best)
    while len(taken) < min(SLOTS, len(cands)):
        rest = [c for c in cands if c not in taken]
        taken.append(max(rest, key=lambda c: min(dist(c, t, views) for t in taken)))
    return taken


def min_dist(pal, views):
    return min(dist(pal[i], pal[j], views) for i in range(len(pal)) for j in range(i + 1, len(pal)))


def source_of(h):
    return ",".join(s for s, cs in SETS.items() if h in cs)


def main():
    cpp = "--cpp" in sys.argv
    allviews = ["normal", "deutan", "protan", "tritan"]
    for name, views, sets, white in KINDS:
        civ = CIVILIAN
        pal = pick(views, sets, civ, white)
        if cpp:
            print("    // %s" % name)
            print("    {{%s}, 0xff%s}," % (", ".join("0xff" + h.lower() for h in pal), civ.lower()))
            continue
        print("%s (least of %s):" % (name, " / ".join(views)))
        for i, h in enumerate(pal, 1):
            print("  %2d  #%s  %s" % (i, h, source_of(h)))
        print("  civilians #%s (%s)" % (civ, source_of(civ)))
        for k in (2, 4, 8, 12, 16):
            line = "  first %2d: min dE2000 %5.1f" % (k, min_dist(pal[:k], views))
            line += "   (" + ", ".join("%s %.1f" % (v, min_dist(pal[:k], [v])) for v in allviews) + ")"
            print(line)
        print("  civilians to the armies: first 8 %.1f, all 16 %.1f" % (min(dist(civ, p, views) for p in pal[:8]),
                                                                   min(dist(civ, p, views) for p in pal)))
    if not cpp:
        # the original's colours (FAF's /lua/GameColors.lua, the lobby's 19), for comparison
        orig = ["E80A0A", "901427", "FF873E", "B76518", "A79602", "FAFA00", "9FD802", "40BF40", "2E8B57", "2F4F4F",
                "436EEE", "2929E1", "5F01A7", "9161FF", "66FFCC", "FFFFFF", "616D7E", "FF88FF", "FF32FF"]
        order = [11, 1, 12, 2, 14, 3, 15, 5]  # FAF's WarmColdMapping, 1v1 .. 4v4
        first8 = [orig[i - 1] for i in order]
        print("Off (FAF's warm/cold order for 8 players: %s):" % " ".join(first8))
        print("  first  2: " + ", ".join("%s %.1f" % (v, min_dist(first8[:2], [v])) for v in allviews))
        print("  first  8: " + ", ".join("%s %.1f" % (v, min_dist(first8, [v])) for v in allviews))


if __name__ == "__main__":
    main()
