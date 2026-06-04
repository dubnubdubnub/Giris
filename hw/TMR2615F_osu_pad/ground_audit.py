"""
Ground audit for TMR2615F_osu_pad.

Run from KiCad PCB Editor: Tools -> Scripting Console, then:
    exec(open(r'D:\\gehub\\giris\\hw\\TMR2615F_osu_pad\\ground_audit.py').read())

Or from a shell that has KiCad's python on PATH:
    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" ground_audit.py

Produces ground_audit_report.md next to this script.

What it does:
  1. Lists footprints of interest (NT1, U7 LDO, U13 sensor) and their positions.
  2. Maps zone fills per layer so we can ask "what ground net is under this XY?".
  3. For every signal track on F.Cu / B.Cu, samples along its length and looks
     up the zone net on the adjacent inner reference layer (In1 for F.Cu,
     In2 for B.Cu). Flags any track whose reference net changes mid-trace
     (GND -> GNDA, or fill -> no-fill -> fill) -- these are return-path
     discontinuities that force return current to detour through NT1.
  4. Estimates the detour distance for each flagged crossing (XY distance
     from the crossing point to NT1).
  5. Counts stitching vias on the GND and GNDA nets per inner layer.
  6. Reports the total length of routed copper on the inner planes
     (anything non-trivial on In1/In2 eats into the plane's continuity).

Caveats:
  - Reference plane assignment is geometric, not field-solved. For 4-layer
    stackup 0.21/1.065/0.21 mm the closest plane dominates -- which here
    means F.Cu refs In1.Cu and B.Cu refs In2.Cu.
  - "Boundary crossing" detection samples every 0.25 mm along each track.
    Diagonal traces that briefly clip a zone corner can show false positives;
    eyeball the report rather than trust counts blindly.
  - pcbnew API names differ between KiCad versions; this script targets v9/v10.
"""

import os
import sys
import math

try:
    import pcbnew
except ImportError:
    sys.stderr.write("pcbnew not importable. Run from KiCad's scripting console or use KiCad's bundled python.\n")
    raise

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BOARD_PATH = os.path.join(SCRIPT_DIR, "TMR2615F_osu_pad.kicad_pcb")
REPORT_PATH = os.path.join(SCRIPT_DIR, "ground_audit_report.md")

SAMPLE_STEP_MM = 0.25
MIN_TRACK_LEN_MM = 0.5  # ignore stubs shorter than this
GND_NETS = {"GND", "GNDA"}

def to_mm(iu):
    return pcbnew.ToMM(iu)

def from_mm(mm):
    return pcbnew.FromMM(mm)

def vec_mm(p):
    return (to_mm(p.x), to_mm(p.y))

def layer_name(board, lid):
    return board.GetLayerName(lid)

def reference_plane_layer(track_layer):
    """Return the inner layer that carries the bulk of this track's return current."""
    if track_layer == pcbnew.F_Cu:
        return pcbnew.In1_Cu
    if track_layer == pcbnew.B_Cu:
        return pcbnew.In2_Cu
    if track_layer == pcbnew.In1_Cu:
        return pcbnew.In2_Cu  # next-closest plane
    if track_layer == pcbnew.In2_Cu:
        return pcbnew.In1_Cu
    return None

class ZoneIndex:
    """Per-layer point-in-zone lookup, priority-aware."""
    def __init__(self, board):
        self.by_layer = {}  # layer_id -> list of (priority, net_name, SHAPE_POLY_SET)
        for z in board.Zones():
            if not z.IsFilled():
                continue
            net = z.GetNetname()
            prio = z.GetAssignedPriority() if hasattr(z, "GetAssignedPriority") else z.GetPriority()
            for lid in z.GetLayerSet().Seq():
                try:
                    polys = z.GetFilledPolysList(lid)
                except TypeError:
                    polys = z.GetFilledPolysList()
                if polys is None or polys.OutlineCount() == 0:
                    continue
                self.by_layer.setdefault(lid, []).append((prio, net, polys, z.GetZoneName() or ""))
        # higher priority first
        for lid in self.by_layer:
            self.by_layer[lid].sort(key=lambda t: -t[0])

    def net_at(self, layer, x_iu, y_iu):
        entries = self.by_layer.get(layer, [])
        if not entries:
            return None
        pt = pcbnew.VECTOR2I(int(x_iu), int(y_iu))
        for prio, net, polys, name in entries:
            if polys.Contains(pt):
                return net
        return None

def sample_segment(start, end, step_iu):
    sx, sy = start.x, start.y
    ex, ey = end.x, end.y
    dx, dy = ex - sx, ey - sy
    length = math.hypot(dx, dy)
    if length == 0:
        return [(sx, sy)]
    n = max(1, int(length / step_iu))
    pts = []
    for i in range(n + 1):
        t = i / n
        pts.append((sx + dx * t, sy + dy * t))
    return pts

def main():
    board = pcbnew.LoadBoard(BOARD_PATH)
    print(f"Loaded {BOARD_PATH}")

    # 1. Footprints of interest
    targets = {"NT1": None, "U7": None, "U13": None}
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in targets:
            targets[ref] = fp

    # 2. Zone index
    zindex = ZoneIndex(board)

    # 3. Tracks: categorize and check crossings
    tracks_by_layer_net = {}
    inner_routed_len = {pcbnew.In1_Cu: 0.0, pcbnew.In2_Cu: 0.0}
    crossings = []  # (net, layer, ref_layer, mid_mm, from_net, to_net, length_mm)

    step_iu = from_mm(SAMPLE_STEP_MM)
    min_len_iu = from_mm(MIN_TRACK_LEN_MM)

    for t in board.GetTracks():
        if not isinstance(t, pcbnew.PCB_TRACK):
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            continue
        net = t.GetNetname()
        layer = t.GetLayer()
        length_iu = t.GetLength()
        length_mm = to_mm(length_iu)
        tracks_by_layer_net.setdefault((layer, net), 0.0)
        tracks_by_layer_net[(layer, net)] += length_mm

        if layer in inner_routed_len and net not in GND_NETS:
            inner_routed_len[layer] += length_mm

        if length_iu < min_len_iu:
            continue
        if net in GND_NETS or net == "":
            continue
        ref_lid = reference_plane_layer(layer)
        if ref_lid is None:
            continue

        pts = sample_segment(t.GetStart(), t.GetEnd(), step_iu)
        prev_net = None
        for (x, y) in pts:
            n = zindex.net_at(ref_lid, x, y)
            if prev_net is not None and n != prev_net:
                # transition
                crossings.append({
                    "net": net,
                    "layer": layer_name(board, layer),
                    "ref_layer": layer_name(board, ref_lid),
                    "x_mm": to_mm(x),
                    "y_mm": to_mm(y),
                    "from": prev_net or "(no fill)",
                    "to": n or "(no fill)",
                    "track_len_mm": length_mm,
                })
            prev_net = n

    # 4. Stitching vias
    via_counts = {}  # (net, top_layer, bot_layer) -> count
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            net = t.GetNetname()
            if net not in GND_NETS:
                continue
            top = layer_name(board, t.TopLayer())
            bot = layer_name(board, t.BottomLayer())
            key = (net, top, bot)
            via_counts[key] = via_counts.get(key, 0) + 1

    # 5. Distances from NT1
    def fp_pos_mm(fp):
        if fp is None:
            return None
        p = fp.GetPosition()
        return (to_mm(p.x), to_mm(p.y))

    nt1_xy = fp_pos_mm(targets["NT1"])
    u7_xy = fp_pos_mm(targets["U7"])
    u13_xy = fp_pos_mm(targets["U13"])

    def dist(a, b):
        if a is None or b is None:
            return None
        return math.hypot(a[0] - b[0], a[1] - b[1])

    # ---- Report
    lines = []
    lines.append("# Ground audit: TMR2615F_osu_pad\n")
    lines.append("Generated by ground_audit.py. All coordinates in mm.\n")

    lines.append("## Key footprint positions\n")
    for ref, fp in targets.items():
        xy = fp_pos_mm(fp)
        if xy is None:
            lines.append(f"- **{ref}**: not found")
        else:
            lines.append(f"- **{ref}** ({fp.GetValue()}): ({xy[0]:.2f}, {xy[1]:.2f})")
    if nt1_xy:
        if u7_xy:
            lines.append(f"- Distance NT1 -> U7: {dist(nt1_xy, u7_xy):.2f} mm")
        if u13_xy:
            lines.append(f"- Distance NT1 -> U13: {dist(nt1_xy, u13_xy):.2f} mm")
    lines.append("")

    lines.append("## Inner-layer routed copper (non-ground)\n")
    lines.append("If these are large, the inner layer can't be treated as a continuous plane.\n")
    for lid, total in inner_routed_len.items():
        lines.append(f"- **{layer_name(board, lid)}**: {total:.1f} mm of non-GND/GNDA track")
    lines.append("")

    lines.append("## Tracks per layer per net (totals in mm)\n")
    lines.append("| Layer | Net | Total length (mm) |")
    lines.append("|---|---|---:|")
    for (layer, net), L in sorted(tracks_by_layer_net.items(), key=lambda kv: (kv[0][0], -kv[1])):
        if L < 0.1:
            continue
        lines.append(f"| {layer_name(board, layer)} | {net or '(unassigned)'} | {L:.1f} |")
    lines.append("")

    lines.append("## Ground stitching vias\n")
    lines.append("| Net | Top layer | Bottom layer | Count |")
    lines.append("|---|---|---|---:|")
    for (net, top, bot), c in sorted(via_counts.items()):
        lines.append(f"| {net} | {top} | {bot} | {c} |")
    lines.append("")

    lines.append("## Return-path discontinuities\n")
    lines.append(
        "Each row is a sample point along a signal track where the *reference plane* "
        "net changes from one ground island to another. Return current at that point "
        "must take the long way around (typically through NT1).\n"
    )
    # Group by track net for readability
    by_net = {}
    for c in crossings:
        by_net.setdefault(c["net"], []).append(c)

    if not crossings:
        lines.append("_None detected. All signal tracks have continuous reference under them._\n")
    else:
        lines.append("| Signal net | Track layer | Ref layer | X (mm) | Y (mm) | From -> To | Detour to NT1 (mm) |")
        lines.append("|---|---|---|---:|---:|---|---:|")
        for net, items in sorted(by_net.items(), key=lambda kv: -len(kv[1])):
            # Deduplicate consecutive identical transitions on the same track
            seen = set()
            for c in items:
                key = (net, c["layer"], round(c["x_mm"], 2), round(c["y_mm"], 2),
                       c["from"], c["to"])
                if key in seen:
                    continue
                seen.add(key)
                detour = dist((c["x_mm"], c["y_mm"]), nt1_xy) if nt1_xy else None
                detour_str = f"{detour:.1f}" if detour is not None else "?"
                lines.append(
                    f"| {net} | {c['layer']} | {c['ref_layer']} | "
                    f"{c['x_mm']:.2f} | {c['y_mm']:.2f} | "
                    f"{c['from']} -> {c['to']} | {detour_str} |"
                )

    lines.append("")
    lines.append("## Interpretation hints\n")
    lines.append(
        "- A crossing with `from`/`to` between **GND** and **GNDA**: real return-path "
        "discontinuity. Return current must reach NT1 (single bond point) to close "
        "the loop. Loop area ~= 2 * detour_to_NT1 * (dielectric thickness to ref layer). "
        "For F.Cu -> In1.Cu that's ~0.21 mm; loop area = detour_mm * 0.42 mm^2.\n"
        "- A crossing involving **(no fill)**: the reference layer has a copper void "
        "below the trace. Either a routed track on the plane layer or an unfilled "
        "gap. These are worse than GND<->GNDA transitions because there's no nearby "
        "return at all.\n"
        "- Many crossings on **In1.Cu** as ref layer means the digital top-side "
        "routing is the main beneficiary/victim of the split. If the count is high, "
        "the split is actively hurting signal integrity, not helping.\n"
        "- Zero crossings means the split exists but no signal actually crosses it -- "
        "the split is harmless but also pointless.\n"
    )

    out = "\n".join(lines)
    with open(REPORT_PATH, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"Wrote {REPORT_PATH}")
    print(f"Crossings detected: {len(crossings)}")

if __name__ == "__main__":
    main()
