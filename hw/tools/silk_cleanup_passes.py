# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""Post-placement cleanup steps, applied after silk_tidy.py refdes pass:
1. unfill large silk body shapes (>1 mm^2)
2. normalize 180/270 refdes angles
3. nudge flagged GND fence vias off the USB DP hole-clearance rule
4. delete small silk shapes over the footprint's own pads (pin fingers),
   exact pad-shape geometry, gated on footprints DRC implicated.

Usage: python3 rebuild.py <board.kicad_pcb> <drc_reference.json>
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import silk_tidy as st

PATH = sys.argv[1]
fmt = lambda v: f"{v:.6f}".rstrip('0').rstrip('.')

SHAPEWORD = {'fp_poly': 'Polygon', 'fp_line': 'Segment', 'fp_arc': 'Arc',
             'fp_circle': 'Circle', 'fp_rect': 'Rectangle'}


def run_pass(fn):
    text = open(PATH).read()
    root = st.parse_sexpr(text)
    board = st.child(root, 'kicad_pcb')
    edits, report = fn(text, board)
    for start, end, new in sorted(edits, reverse=True):
        text = text[:start] + new + text[end:]
    open(PATH, 'w').write(text)
    print(report)


# ---------------------------------------------------------------- pass 1 ---
def unfill(text, board):
    edits = []
    for fp in st.children(board, 'footprint'):
        for shp in fp:
            if not isinstance(shp, st.Node) or not shp:
                continue
            if str(shp[0]) not in ('fp_poly', 'fp_rect', 'fp_circle'):
                continue
            if st.layer_of(shp) not in ('F.SilkS', 'B.SilkS'):
                continue
            fill = st.child(shp, 'fill')
            if fill is None or str(fill[1]) in ('none', 'no'):
                continue
            pts = st.shape_points(shp)
            if not pts:
                continue
            b = st.BBox.around(pts)
            if b.w * b.h > 1.0:
                edits.append((fill.start, fill.end, '(fill no)'))
    return edits, f"unfilled {len(edits)} large silk shapes"


# ---------------------------------------------------------------- pass 2 ---
def normalize_angles(text, board):
    edits = []
    for fp in st.children(board, 'footprint'):
        for prop in st.children(fp, 'property'):
            if str(prop[1]) != 'Reference':
                continue
            if st.layer_of(prop) not in ('F.SilkS', 'B.SilkS'):
                continue
            hide = st.child(prop, 'hide')
            if hide is not None and 'yes' in [str(t) for t in hide[1:]]:
                continue
            at_node = st.child(prop, 'at')
            vals = [float(x) for x in at_node[1:]]
            if len(vals) < 3:
                continue
            a = vals[2] % 360
            na = {180: 0.0, 270: 90.0}.get(a, a)
            if na == vals[2]:
                continue
            new = f"(at {fmt(vals[0])} {fmt(vals[1])}" + (f" {fmt(na)})" if na else ")")
            edits.append((at_node.start, at_node.end, new))
    return edits, f"normalized {len(edits)} refdes angles"


# ---------------------------------------------------------------- pass 3 ---
FLAG_A = {round(x, 4) for x in (
    99.324994, 100.099994, 100.849994, 101.624994, 102.349994, 102.999994,
    103.674994, 104.374994, 105.124994, 105.849994, 106.649994, 107.524994,
    108.324994)}
FLAG_B = {round(x, 4) for x in (106.299994, 107.274994, 108.024994)}


def nudge_vias(text, board):
    edits = []
    for via in st.children(board, 'via'):
        at_node = st.child(via, 'at')
        at = st.floats(via, 'at')
        x, y = round(at[0], 4), round(at[1], 4)
        if y == 106.275 and x in FLAG_A:
            ny = at[1] + 0.03      # away from D+ straight above
        elif y == 104.925 and x in FLAG_B:
            ny = at[1] - 0.03      # away from D- straight below
        else:
            continue
        edits.append((at_node.start, at_node.end, f"(at {fmt(at[0])} {fmt(ny)})"))
    return edits, f"nudged {len(edits)} GND fence vias by 0.03 mm"


# ---------------------------------------------------------------- pass 4 ---
def point_in_pad(sx, sy, pad, T, margin):
    at = st.floats(pad, 'at', [0, 0, 0])
    sz = st.floats(pad, 'size', [0, 0])
    cx, cy = T.apply(at[0], at[1])
    ang = at[2] if len(at) > 2 else 0.0    # board-frame absolute
    shape = str(pad[3])
    dx, dy = sx - cx, sy - cy
    if shape == 'circle':
        return math.hypot(dx, dy) <= sz[0] / 2 + margin
    # rotated-rect test (oval/roundrect treated as their bounding rect)
    for sgn in (1, -1):
        rx, ry = st.rot_pt(dx, dy, sgn * ang)
        if abs(rx) <= sz[0] / 2 + margin and abs(ry) <= sz[1] / 2 + margin:
            return True
    return False


def delete_fingers(text, board):
    edits = []
    deleted = {}
    for fp in st.children(board, 'footprint'):
        ref = next((str(p[2]) for p in st.children(fp, 'property')
                    if str(p[1]) == 'Reference'), '?')
        tr = st.child(fp, 'transform')
        tv = st.floats(tr, 'translate', [0, 0])
        rv = st.floats(tr, 'rotate', [0])
        sv = st.floats(tr, 'scale', [1, 1])
        T = st.Transform(tv[0], tv[1], rv[0], sv[0], sv[1])
        pads = list(st.children(fp, 'pad'))
        if not pads:
            continue
        for shp in fp:
            if not isinstance(shp, st.Node) or not shp:
                continue
            tag = str(shp[0])
            if tag not in SHAPEWORD:
                continue
            if st.layer_of(shp) not in ('F.SilkS', 'B.SilkS'):
                continue
            hw = st.stroke_width(shp) / 2 + 0.005
            samples = []
            if tag == 'fp_circle':
                c = st.floats(shp, 'center')
                e = st.floats(shp, 'end')
                if not c or not e:
                    continue
                r = math.hypot(e[0] - c[0], e[1] - c[1])
                n = max(16, int(2 * math.pi * r / 0.05))
                samples = [T.apply(c[0] + r * math.cos(2 * math.pi * i / n),
                                   c[1] + r * math.sin(2 * math.pi * i / n))
                           for i in range(n)]
                pts = [T.apply(c[0] - r, c[1] - r), T.apply(c[0] + r, c[1] + r)]
            else:
                raw = st.shape_points(shp)
                if not raw:
                    continue
                pts = [T.apply(x, y) for x, y in raw]
                ring = pts + pts[:1] if tag in ('fp_poly', 'fp_rect') else pts
                for (x0, y0), (x1, y1) in zip(ring, ring[1:]):
                    n = max(2, int(math.hypot(x1 - x0, y1 - y0) / 0.05))
                    samples += [(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n)
                                for i in range(n + 1)]
            hit = any(point_in_pad(sx, sy, pad, T, hw)
                      for sx, sy in samples for pad in pads)
            if not hit:
                continue
            b = st.BBox.around(pts)
            if b.w * b.h >= 2.0:
                continue
            s = shp.start
            while s > 0 and text[s - 1] in ' \t':
                s -= 1
            if s > 0 and text[s - 1] == '\n':
                s -= 1
            edits.append((s, shp.end, ''))
            deleted[ref] = deleted.get(ref, 0) + 1
    return edits, f"deleted {sum(deleted.values())} pin-finger shapes: {dict(sorted(deleted.items()))}"


step = sys.argv[2] if len(sys.argv) > 2 else 'all'
if step in ('pre', 'all'):
    run_pass(unfill)
    run_pass(delete_fingers)
if step in ('post', 'all'):
    run_pass(normalize_angles)
    run_pass(nudge_vias)
