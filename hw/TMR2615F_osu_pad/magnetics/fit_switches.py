"""Predict the in-plane field at the osu!pad's TMR2615F for commercial magnetic switches.

Two evidence tiers:
  MEASURED -- magnet size and rest height taken off the vendor's dimensioned 5:2
    engineering drawing (hw/refs/switches/*.pdf, measured with measure_switch.py at
    600 dpi, calibrated against two dimensioned base widths; see the July 2026
    conversation). Remanence Br is then solved from the published bottom-out flux at
    the vendor's own reference plane, and the published initial flux (quoted at
    0.3 mm pressed, per the drawings) is a held-out validation.
  FITTED -- no drawing available. The magnet is assumed to be the measured Gateron
    cylinder (all five measured Gateron switches share Ø2.77 x 3.41 mm), Br fixed at
    1.3 T, and the rest clearance h_b fitted to the published bottom-out flux.

Vendor reference plane (flux spec measurement point) = ref PCB thickness + 0.3 mm die
depth. Modern Gateron sheets quote "(PCB1.2mm)" -> 1.5 mm; TTC/Akko quote 1.6 mm-PCB
figures -> 1.9 mm; unstated -> 1.9 mm assumed.

Board geometry (from TMR2615F_osu_pad.kicad_pcb):
  - sensor 3.09 mm radially offset from switch center, mounted on the F side
  - switch inserts from the B side: magnet sees the die through the 1.62 mm PCB
  - TMR2615F die plane sits 0.3 mm below the F-side surface (DFN3L pads-down)

Sensor: TMR2615F-AAC-1.500-500, mid-rail offset, output rails at 5/95% VDD
  => electrical clip at +/-300 Gs (ratiometric, independent of VDD).

Polarity note: every modern Gateron drawing states "Magnet N-pole is facing down";
the TMR2615F is bipolar, so N-down vs S-down only flips output slope -- don't mix
polarities on one board unless firmware calibrates sign per key.

Run: python3 fit_switches.py
"""
from magfield import cylinder_field

# ---- board geometry (mm) ----
R_SENSOR = 3.09          # sensor radial offset from switch axis
PCB = 1.62               # board thickness
DIE_DEPTH = 0.3          # die plane below the F-side surface

# ---- sensor ordering code ----
SEN = 1.5                # mV/V/Gs  (TMR2615F-AAC-1.500-500)
RAIL_GS = 450.0 / SEN

MEAS_DIA, MEAS_LEN = 2.77, 3.41   # the common measured Gateron magnet

# measured: (name, dia, len, rest_h, travel, pub_init@0.3mm, pub_bottom, ref_die, pol)
MEASURED = [
    ("Gateron Magnetic Jade",     2.77, 3.41, 4.26, 3.5, 120, 700, 1.5, "N-down"),
    ("Gateron Jade Pro",          2.76, 3.41, 4.31, 3.5, 120, 700, 1.5, "N-down"),
    ("Gateron Jade Ultra",        2.77, 3.42, 4.60, 3.2, 122, 550, 1.5, "N-down"),
    ("Gateron Jade Attraction",   2.78, 3.43, 4.73, 3.2, 150, 570, 1.5, "N-down"),
    ("Gateron x MCHOSE Apollo",   2.80, 3.42, 4.13, 3.1, 120, 570, 1.5, "N-down"),
]
# fitted: (name, travel, pub_rest, pub_bottom, ref_die, pol)
FITTED = [
    ("Gateron KS-20 / Wooting Lekker*",    4.1, 102, 905, 1.9, "n/p"),
    ("Gateron KS-37B Fox / Nebula",        4.0, 120, 800, 1.9, "n/p"),
    ("TTC KoM / Kailh Aurora",             3.4,  90, 480, 1.9, "n/p"),
    ("Kailh Magnetic God / Quick Trigger", 3.3, 120, 750, 1.9, "n/p"),
    ("Everglide Sticky Rice V2",           3.5, 120, 800, 1.9, "n/p"),
    ("Wuque Studio WS Flux",               3.5, 115, 635, 1.9, "n/p"),
    ("Akko AstroLink",                     3.4,  90, 480, 1.9, "N-down"),
    ("Akko AstroAim / Flash",              3.5,  95, 580, 1.9, "N-down"),
]


def on_axis_gs(dia, length, brem, gap_mm):
    _, bz = cylinder_field(dia / 2e3, length / 1e3, brem, gap_mm / 1e3, 1e-12, 0.0)
    return bz * 1e4


def radial_gs(dia, length, brem, gap_mm, r_mm):
    br, _ = cylinder_field(dia / 2e3, length / 1e3, brem, gap_mm / 1e3, r_mm / 1e3, 0.0)
    return abs(br) * 1e4


def fit_hb(dia, length, brem, pub_bottom, ref_die):
    lo, hi = 0.05, 5.0
    if on_axis_gs(dia, length, brem, lo + ref_die) < pub_bottom:
        return lo
    for _ in range(50):
        mid = (lo + hi) / 2
        if on_axis_gs(dia, length, brem, mid + ref_die) > pub_bottom:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def board_field(dia, length, brem, rest_h, depth):
    """|Bx| (Gs) at this board's sensor, magnet bottom rest_h above PCB at key-up"""
    gap = (rest_h - depth) + PCB + DIE_DEPTH
    return radial_gs(dia, length, brem, gap, R_SENSOR)


def clip_depth(dia, length, brem, rest_h, travel):
    if board_field(dia, length, brem, rest_h, travel) <= RAIL_GS:
        return None
    lo, hi = 0.0, travel
    for _ in range(50):
        mid = (lo + hi) / 2
        if board_field(dia, length, brem, rest_h, mid) < RAIL_GS:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def rows():
    for name, d, L, rest, tr, p_i, p_b, ref, pol in MEASURED:
        br = p_b / on_axis_gs(d, L, 1.0, (rest - tr) + ref)
        chk = on_axis_gs(d, L, br, (rest - 0.3) + ref)
        yield name, d, L, br, rest, tr, p_i, p_b, chk, pol, "meas"
    for name, tr, p_r, p_b, ref, pol in FITTED:
        d, L, br = MEAS_DIA, MEAS_LEN, 1.3
        hb = fit_hb(d, L, br, p_b, ref)
        chk = on_axis_gs(d, L, br, hb + tr + ref)
        yield name, d, L, br, hb + tr, tr, p_r, p_b, chk, pol, "fit"


if __name__ == "__main__":
    print(f"sensor r={R_SENSOR}mm, PCB {PCB}mm, SEN {SEN} mV/V/Gs -> rails at +/-{RAIL_GS:.0f} Gs\n")
    hdr = (f"{'switch':<33} {'pol':>7} {'src':>4} {'Br':>5} {'init chk':>12} "
           f"{'rest':>5} {'@1mm':>5} {'@2mm':>5} {'bottom':>7} {'linear until':>13}")
    print(hdr)
    print("-" * len(hdr))
    for name, d, L, br, rest, tr, p_i, p_b, chk, pol, src in rows():
        f = lambda depth: board_field(d, L, br, rest, depth)
        cd = clip_depth(d, L, br, rest, tr)
        clip = f"clips {cd:.2f}mm" if cd is not None else "full travel"
        err = (chk / p_i - 1) * 100
        print(f"{name:<33} {pol:>7} {src:>4} {br:>5.2f} {chk:>4.0f}({err:>+3.0f}%) "
              f"{f(0):>5.0f} {f(1):>5.0f} {f(min(2, tr)):>5.0f} {f(tr):>7.0f} {clip:>13}")
    print("\nfields in Gauss at the TMR2615F die; 'init chk' = model vs published initial flux")
    print("* Wooting publishes no flux specs; Lekker modeled as Gateron KS-20 (community-assumed)")
