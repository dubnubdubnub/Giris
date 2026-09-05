# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""Auto-place silkscreen reference designators in a .kicad_pcb file.

Standalone: parses the s-expression directly (supports the new
`(transform (translate ...) (rotate ...))` footprint format, file version
>= 20260623) and rewrites only the `(at ...)` node of each moved
Reference property, so the diff is surgical.

Semantics (from KiCad source, pcb_io_kicad_sexpr_parser.cpp / pcb_text.cpp
/ transform_trs.cpp):
  - footprint child positions are lib-frame; abs = R(rot)@(p*scale) + translate
    with RotatePoint convention x' = x cos + y sin, y' = -x sin + y cos
  - text and pad angles in the file are board-frame absolute
  - field text default justification is center/center

Usage: python3 silk_tidy.py board.kicad_pcb [--dry-run]
"""
import math
import re
import sys

CLEARANCE = 0.08     # mm gap between text bbox and obstacles
EDGE_MARGIN = 0.15   # mm gap to board edge bbox
MAX_DIST = 3.0       # mm max offset beyond courtyard to search
STEP = 0.2           # mm search step

NARROW = set("1iIljJ.,:;!|'()[] ")


# ---------------------------------------------------------------- s-expr ---

TOKEN_RE = re.compile(r'"(?:[^"\\]|\\.)*"|[()]|[^\s()"]+')


class Node(list):
    __slots__ = ("start", "end")


class Atom(str):
    __slots__ = ("start", "end")

    def __new__(cls, s, start, end):
        o = super().__new__(cls, s)
        o.start, o.end = start, end
        return o


def parse_sexpr(text):
    stack = [Node()]
    for m in TOKEN_RE.finditer(text):
        tok = m.group()
        if tok == "(":
            n = Node()
            n.start = m.start()
            stack.append(n)
        elif tok == ")":
            n = stack.pop()
            n.end = m.end()
            stack[-1].append(n)
        else:
            if tok.startswith('"'):
                tok = tok[1:-1]
            stack[-1].append(Atom(tok, m.start(), m.end()))
    return stack[0]


def children(node, name):
    for c in node:
        if isinstance(c, Node) and c and c[0] == name:
            yield c


def child(node, name):
    for c in children(node, name):
        return c
    return None


def floats(node, name, default=None):
    c = child(node, name)
    if c is None:
        return default
    return [float(x) for x in c[1:] if not isinstance(x, Node)]


# -------------------------------------------------------------- geometry ---

def rot_pt(x, y, deg):
    if deg % 360 == 0:
        return x, y
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return x * c + y * s, -x * s + y * c


class Transform:
    def __init__(self, tx, ty, rot, sx=1.0, sy=1.0):
        self.tx, self.ty, self.rot, self.sx, self.sy = tx, ty, rot, sx, sy

    def apply(self, x, y):
        x, y = x * self.sx, y * self.sy
        x, y = rot_pt(x, y, self.rot)
        return x + self.tx, y + self.ty

    def inverse(self, x, y):
        x, y = x - self.tx, y - self.ty
        x, y = rot_pt(x, y, -self.rot)
        return x / self.sx, y / self.sy


class BBox:
    __slots__ = ("x0", "y0", "x1", "y1")

    def __init__(self, x0, y0, x1, y1):
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1

    @classmethod
    def around(cls, pts, pad=0.0):
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return cls(min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad)

    def inflated(self, d):
        return BBox(self.x0 - d, self.y0 - d, self.x1 + d, self.y1 + d)

    def intersects(self, o):
        return not (self.x1 < o.x0 or o.x1 < self.x0
                    or self.y1 < o.y0 or o.y1 < self.y0)

    def contains(self, o):
        return (self.x0 <= o.x0 and self.x1 >= o.x1
                and self.y0 <= o.y0 and self.y1 >= o.y1)

    @property
    def w(self):
        return self.x1 - self.x0

    @property
    def h(self):
        return self.y1 - self.y0

    @property
    def cx(self):
        return (self.x0 + self.x1) / 2

    @property
    def cy(self):
        return (self.y0 + self.y1) / 2


def text_bbox(s, cx, cy, sx, sy, thick, angle, hjust="center", vjust="center"):
    """Estimated bbox of stroke-font text centered/justified at (cx, cy)."""
    w = sum(0.55 if ch in NARROW else 0.95 for ch in s) * sx + thick
    h = sy + thick
    if hjust == "left":
        ox = w / 2
    elif hjust == "right":
        ox = -w / 2
    else:
        ox = 0.0
    if vjust == "top":
        oy = h / 2
    elif vjust == "bottom":
        oy = -h / 2
    else:
        oy = 0.0
    # offset is in the text's rotated frame
    dx, dy = rot_pt(ox, oy, angle)
    ccx, ccy = cx + dx, cy + dy
    # axis-aligned envelope of the rotated w x h rect
    r = math.radians(angle)
    ew = abs(w * math.cos(r)) + abs(h * math.sin(r))
    eh = abs(w * math.sin(r)) + abs(h * math.cos(r))
    return BBox(ccx - ew / 2, ccy - eh / 2, ccx + ew / 2, ccy + eh / 2)


def shape_points(node):
    """Coordinate points of an fp_/gr_ shape node (local frame)."""
    pts = []
    for name in ("start", "end", "mid", "center"):
        v = floats(node, name)
        if v:
            pts.append((v[0], v[1]))
    p = child(node, "pts")
    if p is not None:
        for xy in children(p, "xy"):
            pts.append((float(xy[1]), float(xy[2])))
    if node[0].endswith("circle"):
        c = floats(node, "center")
        e = floats(node, "end")
        if c and e:
            rad = math.hypot(e[0] - c[0], e[1] - c[1])
            pts = [(c[0] - rad, c[1] - rad), (c[0] + rad, c[1] + rad)]
    return pts


def stroke_width(node):
    st = child(node, "stroke")
    if st is not None:
        v = floats(st, "width")
        if v:
            return v[0]
    v = floats(node, "width")
    return v[0] if v else 0.0


def justify_of(effects):
    hjust = vjust = "center"
    if effects is not None:
        j = child(effects, "justify")
        if j is not None:
            toks = [str(t) for t in j[1:]]
            for t in toks:
                if t in ("left", "right"):
                    hjust = t
                elif t in ("top", "bottom"):
                    vjust = t
    return hjust, vjust


def font_of(effects):
    sx = sy = 1.0
    thick = 0.15
    if effects is not None:
        f = child(effects, "font")
        if f is not None:
            v = floats(f, "size")
            if v:
                # file format is (size HEIGHT WIDTH); see
                # pcb_io_kicad_sexpr_parser.cpp: sz.y = "text height" first
                sy, sx = v[0], v[1]
            t = floats(f, "thickness")
            if t:
                thick = t[0]
    return sx, sy, thick


# ----------------------------------------------------------------- board ---

SILK = {"F": "F.SilkS", "B": "B.SilkS"}
CRTYD = {"F": "F.CrtYd", "B": "B.CrtYd"}


def layer_of(node):
    lay = child(node, "layer")
    return str(lay[1]) if lay is not None else None


def pad_sides(pad):
    lay = child(pad, "layers")
    toks = [str(t) for t in lay[1:]] if lay is not None else []
    sides = set()
    for t in toks:
        if t.startswith(("F.", "*.")):
            sides.add("F")
        if t.startswith(("B.", "*.")):
            sides.add("B")
    return sides


def main():
    path = sys.argv[1]
    dry = "--dry-run" in sys.argv
    text = open(path).read()
    root = parse_sexpr(text)
    board = child(root, "kicad_pcb")

    obstacles = {"F": [], "B": []}
    edge_pts = []
    jobs = []           # (refname, side, tb_current, body, anchor, job data)
    ref_boxes = {}      # refname -> (side, BBox)

    for fp in children(board, "footprint"):
        tr = child(fp, "transform")
        if tr is not None:
            tv = floats(tr, "translate", [0, 0])
            rv = floats(tr, "rotate", [0])
            sv = floats(tr, "scale", [1, 1])
            T = Transform(tv[0], tv[1], rv[0], sv[0], sv[1])
        else:
            at = floats(fp, "at", [0, 0, 0])
            T = Transform(at[0], at[1], at[2] if len(at) > 2 else 0)

        court_pts = {"F": [], "B": []}
        body_pts = {"F": [], "B": []}  # fallback: pads + graphics
        fp_obstacle_shapes = []        # (side, BBox)

        for shp in fp:
            if not isinstance(shp, Node) or not shp:
                continue
            tag = str(shp[0])
            if tag in ("fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly"):
                lay = layer_of(shp)
                pts = [T.apply(x, y) for x, y in shape_points(shp)]
                if not pts:
                    continue
                hw = stroke_width(shp) / 2
                for side in ("F", "B"):
                    if lay == CRTYD[side]:
                        court_pts[side].extend(pts)
                    elif lay == SILK[side]:
                        fp_obstacle_shapes.append(
                            (side, BBox.around(pts, hw)))
                        body_pts[side].extend(pts)
                    elif lay == f"{side}.Fab":
                        body_pts[side].extend(pts)
            elif tag == "pad":
                at = floats(shp, "at", [0, 0, 0])
                sz = floats(shp, "size", [0, 0])
                cx, cy = T.apply(at[0], at[1])
                ang = at[2] if len(at) > 2 else 0  # board-frame absolute
                w, h = sz[0], sz[1]
                if ang % 180 == 0:
                    pass
                elif ang % 90 == 0:
                    w, h = h, w
                else:
                    d = math.hypot(w, h)
                    w = h = d
                b = BBox(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
                for side in pad_sides(shp):
                    obstacles[side].append(("pad", b))
                    body_pts[side].append((b.x0, b.y0))
                    body_pts[side].append((b.x1, b.y1))
            elif tag == "fp_text":
                lay = layer_of(shp)
                if child(shp, "hide") is not None and "yes" in child(shp, "hide")[1:]:
                    continue
                at = floats(shp, "at", [0, 0, 0])
                cx, cy = T.apply(at[0], at[1])
                ang = at[2] if len(at) > 2 else 0
                eff = child(shp, "effects")
                sx, sy, thick = font_of(eff)
                hj, vj = justify_of(eff)
                b = text_bbox(str(shp[2]), cx, cy, sx, sy, thick, ang, hj, vj)
                for side in ("F", "B"):
                    if lay == SILK[side]:
                        obstacles[side].append(("text", b))

        for side in ("F", "B"):
            for s, b in fp_obstacle_shapes:
                if s == side:
                    obstacles[side].append(("silk", b))
            if court_pts[side]:
                obstacles[side].append(("court", BBox.around(court_pts[side])))
            if body_pts[side]:
                obstacles[side].append(("body", BBox.around(body_pts[side])))

        fp_layer = layer_of(fp)
        fp_side = "B" if fp_layer == "B.Cu" else "F"

        def body_for(side):
            if court_pts[side]:
                return BBox.around(court_pts[side])
            if body_pts[side]:
                return BBox.around(body_pts[side], 0.1)
            return None

        for prop in children(fp, "property"):
            pname, pval = str(prop[1]), str(prop[2])
            hide = child(prop, "hide")
            hidden = hide is not None and "yes" in [str(t) for t in hide[1:]]
            lay = layer_of(prop)
            side = next((s for s in ("F", "B") if lay == SILK[s]), None)
            if side is None or hidden:
                continue
            at_node = child(prop, "at")
            at = [float(x) for x in at_node[1:]]
            ang = at[2] if len(at) > 2 else 0.0
            cx, cy = T.apply(at[0], at[1])
            eff = child(prop, "effects")
            sx, sy, thick = font_of(eff)
            hj, vj = justify_of(eff)
            tb = text_bbox(pval, cx, cy, sx, sy, thick, ang, hj, vj)
            if pname == "Reference":
                body = body_for(side) or BBox(cx - 1, cy - 1, cx + 1, cy + 1)
                jobs.append(dict(
                    name=pval, side=side, tb=tb, body=body, T=T, ang=ang,
                    at_node=at_node, sx=sx, sy=sy, thick=thick,
                    anchor=(T.tx, T.ty)))
                ref_boxes[pval] = (side, tb)
            else:
                obstacles[side].append(("text", tb))

    # board-level items
    for tag in ("gr_line", "gr_rect", "gr_circle", "gr_arc", "gr_poly",
                "gr_curve"):
        for shp in children(board, tag):
            lay = layer_of(shp)
            pts = shape_points(shp)
            if not pts:
                continue
            hw = stroke_width(shp) / 2
            if lay == "Edge.Cuts":
                edge_pts.extend(pts)
            else:
                for side in ("F", "B"):
                    if lay == SILK[side]:
                        obstacles[side].append(("silk", BBox.around(pts, hw)))

    for shp in children(board, "gr_text"):
        lay = layer_of(shp)
        side = next((s for s in ("F", "B") if lay == SILK[s]), None)
        if side is None:
            continue
        at = floats(shp, "at", [0, 0, 0])
        eff = child(shp, "effects")
        sx, sy, thick = font_of(eff)
        ang = at[2] if len(at) > 2 else 0
        # justification of board text varies; cover both by doubling width
        b = text_bbox(str(shp[1]) * 2, at[0], at[1], sx, sy, thick, ang)
        obstacles[side].append(("text", b))

    for via in children(board, "via"):
        at = floats(via, "at", [0, 0])
        sz = floats(via, "size", [0.6])
        r = sz[0] / 2
        b = BBox(at[0] - r, at[1] - r, at[0] + r, at[1] + r)
        obstacles["F"].append(("via", b))
        obstacles["B"].append(("via", b))

    if not edge_pts:
        sys.exit("no Edge.Cuts found")
    edges = BBox.around(edge_pts).inflated(-EDGE_MARGIN)

    # ----------------------------------------------------------- placement
    # Relaxation tiers: strict, then allow silk over (tented) vias, then
    # additionally allow overlapping a neighbor's courtyard margin (real
    # part bodies, pads, silk artwork and text are never overlapped).
    TIERS = [set(), {"via"}, {"via", "court"}]

    jobs.sort(key=lambda j: j["body"].w * j["body"].h)
    kept, moved, failed, compromised = [], [], [], []
    tier_used = {0: 0, 1: 0, 2: 0}
    edits = []  # (start, end, new_text)

    for job in jobs:
        name, side = job["name"], job["side"]
        body, tb = job["body"], job["tb"]
        tw, th = tb.w, tb.h
        ax, ay = job["anchor"]
        radius = max(body.w, body.h) + 2 * MAX_DIST + max(tw, th)

        def near(b):
            return (abs(b.cx - ax) < radius + b.w / 2
                    and abs(b.cy - ay) < radius + b.h / 2)

        local = [(k, b) for k, b in obstacles[side] if near(b)]
        others = [b for r, (s, b) in ref_boxes.items()
                  if r != name and s == side and near(b)]

        def ok(b, ignore):
            i = b.inflated(CLEARANCE)
            if not edges.contains(i):
                return False
            if any(i.intersects(o) for o in others):
                return False
            return not any(
                i.intersects(o) for k, o in local if k not in ignore)

        # candidate slots ringing the body, nearest-first; each is
        # (bbox-center x, y, rotated?) -- rotated swaps the text envelope
        def ring(w, h, rot, maxdist=MAX_DIST):
            out = []
            nsteps = int(maxdist / STEP) + 1
            for i in range(nsteps):
                d = i * STEP
                top = body.y0 - CLEARANCE - h / 2 - d
                bot = body.y1 + CLEARANCE + h / 2 + d
                left = body.x0 - CLEARANCE - w / 2 - d
                right = body.x1 + CLEARANCE + w / 2 + d
                xs = {body.cx}
                if body.w > w:
                    xs |= {body.x0 + w / 2, body.x1 - w / 2}
                ys = {body.cy}
                if body.h > h:
                    ys |= {body.y0 + h / 2, body.y1 - h / 2}
                for x in xs:
                    out.append((x, top, rot))
                    out.append((x, bot, rot))
                for y in ys:
                    out.append((left, y, rot))
                    out.append((right, y, rot))
                out.append((left, top, rot))
                out.append((right, top, rot))
                out.append((left, bot, rot))
                out.append((right, bot, rot))
            return out

        cands = ring(tw, th, False) + ring(th, tw, True)
        # nearest first; slight penalty for changing orientation
        cands.sort(key=lambda p: (p[0] - ax) ** 2 + (p[1] - ay) ** 2
                   + (0.09 if p[2] else 0.0))

        def cand_box(x, y, rot):
            w, h = (th, tw) if rot else (tw, th)
            return BBox(x - w / 2, y - h / 2, x + w / 2, y + h / 2)

        def place(x, y, rot, nb):
            lx, ly = job["T"].inverse(x, y)
            ang = (job["ang"] + (90 if rot else 0)) % 360
            fmt = lambda v: f"{v:.6f}".rstrip("0").rstrip(".")
            if ang:
                new = f"(at {fmt(lx)} {fmt(ly)} {fmt(ang)})"
            else:
                new = f"(at {fmt(lx)} {fmt(ly)})"
            edits.append((job["at_node"].start, job["at_node"].end, new))
            ref_boxes[name] = (side, nb)
            moved.append(name)

        placed = False
        for tier, ignore in enumerate(TIERS):
            if ok(tb, ignore):
                kept.append(name)
                tier_used[tier] += 1
                placed = True
                break
            for x, y, rot in cands:
                nb = cand_box(x, y, rot)
                if ok(nb, ignore):
                    place(x, y, rot, nb)
                    tier_used[tier] += 1
                    placed = True
                    break
            if placed:
                break

        if not placed:
            # minimal-penalty fallback: never overlap other text/refs or
            # leave the board; otherwise pick the least-bad spot. Round 1
            # forbids pads too (clipped refdes is unreadable); round 2
            # permits them only as a last resort.
            ROUNDS = [
                {"pad": None, "silk": 6.0, "text": None, "body": 3.0,
                 "court": 1.0, "via": 0.5},
                {"pad": 100.0, "silk": 6.0, "text": None, "body": 3.0,
                 "court": 1.0, "via": 0.5},
            ]

            def overlap(a, b):
                w = min(a.x1, b.x1) - max(a.x0, b.x0)
                h = min(a.y1, b.y1) - max(a.y0, b.y0)
                return w * h if (w > 0 and h > 0) else 0.0

            far = ring(tw, th, False, 2 * MAX_DIST) + \
                ring(th, tw, True, 2 * MAX_DIST)
            far.sort(key=lambda p: (p[0] - ax) ** 2 + (p[1] - ay) ** 2
                     + (0.09 if p[2] else 0.0))
            best = None
            for WEIGHT in ROUNDS:
                for x, y, rot in far:
                    nb = cand_box(x, y, rot)
                    i = nb.inflated(CLEARANCE)
                    if not edges.contains(i):
                        continue
                    if any(i.intersects(o) for o in others):
                        continue
                    pen = 0.0
                    bad = False
                    for k, o in local:
                        a = overlap(i, o)
                        if a <= 0:
                            continue
                        if WEIGHT[k] is None:
                            bad = True
                            break
                        pen += a * WEIGHT[k]
                    if bad:
                        continue
                    d2 = (x - ax) ** 2 + (y - ay) ** 2
                    key = (pen, d2)
                    if best is None or key < best[0]:
                        best = (key, x, y, rot, nb)
                if best is not None:
                    break
            if best is not None:
                _, x, y, rot, nb = best
                place(x, y, rot, nb)
                compromised.append(name)
            else:
                failed.append(name)

    print(f"kept in place : {len(kept)}")
    print(f"moved         : {len(moved)}")
    print(f"tiers (strict/over-via/over-courtyard): {tier_used}")
    print(f"least-bad     : {len(compromised)}  {compromised}")
    print(f"unplaceable   : {len(failed)}  {failed}")

    if not dry and edits:
        for start, end, new in sorted(edits, reverse=True):
            text = text[:start] + new + text[end:]
        open(path, "w").write(text)
        print(f"saved {path}")


if __name__ == "__main__":
    main()
