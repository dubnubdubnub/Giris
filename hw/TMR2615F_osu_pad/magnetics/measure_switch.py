"""Measure magnet + geometry from a Gateron-style switch drawing PDF.

Renders 600dpi crops with pdftoppm, finds long straight edges by dark-pixel
accumulation (hatch lines are diagonal, so they never win a column/row vote),
calibrates px/mm from two dimensioned base widths in the front view.
"""
import subprocess, sys, os

def load_ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split()); f.readline()
        return w, h, f.read()

def render(pdf, page, x, y, W, H, out):
    subprocess.run(['pdftoppm', '-f', str(page), '-l', str(page), '-r', '600',
                    '-x', str(x), '-y', str(y), '-W', str(W), '-H', str(H),
                    pdf, out], check=True, capture_output=True)
    return f"{out}-{page}.ppm"

def dark(w, d, x, y):
    return d[3 * (y * w + x)] < 128

def col_counts(w, h, d, x0, x1, y0, y1):
    return {x: sum(dark(w, d, x, y) for y in range(y0, y1)) for x in range(x0, x1)}

def row_counts(w, h, d, x0, x1, y0, y1):
    return {y: sum(dark(w, d, x, y) for x in range(x0, x1)) for y in range(y0, y1)}

def clusters(counts, thresh):
    """cluster keys whose count >= thresh; return cluster centers"""
    keys = sorted(k for k, c in counts.items() if c >= thresh)
    groups = []
    for k in keys:
        if groups and k - groups[-1][-1] <= 4: groups[-1].append(k)
        else: groups.append([k])
    return [sum(g) / len(g) for g in groups]

def measure(pdf, page, sect, cal, cal_dims, mag_x, mag_y, label):
    """sect/cal: (x,y,W,H) 600dpi crop boxes. cal_dims: (narrow_mm, wide_mm).
       mag_x: (x0,x1,y0,y1) column-scan box; mag_y: (x0,x1,y0,y1) row-scan box."""
    s = render(pdf, page, *sect, 'm_sect')
    c = render(pdf, page, *cal, 'm_cal')

    # --- calibration: symmetric width pairs in the front-view base ---
    w, h, d = load_ppm(c)
    cc = col_counts(w, h, d, 0, w, cal[3] - 380, cal[3] - 60)
    cent = clusters(cc, 150)
    if len(cent) < 4:
        print(f"{label}: CAL FAIL, clusters={cent}"); return None
    # outermost pair = wide dim, next pair = narrow dim
    lo, hi = min(cent), max(cent)
    inner = [x for x in cent if x not in (lo, hi)]
    lo2, hi2 = min(inner), max(inner)
    narrow_mm, wide_mm = cal_dims
    s1, s2 = (hi - lo) / wide_mm, (hi2 - lo2) / narrow_mm
    if abs(s1 / s2 - 1) > 0.02:
        print(f"{label}: CAL WARN scales differ {s1:.2f} vs {s2:.2f} px/mm; clusters={['%.0f'%x for x in cent]}")
    pxmm = (s1 + s2) / 2

    # --- magnet edges in section crop ---
    w, h, d = load_ppm(s)
    vx = clusters(col_counts(w, h, d, mag_x[0], mag_x[1], mag_x[2], mag_x[3]),
                  int((mag_x[3] - mag_x[2]) * 0.62))
    hy = clusters(row_counts(w, h, d, mag_y[0], mag_y[1], mag_y[2], mag_y[3]),
                  int((mag_y[1] - mag_y[0]) * 0.82))
    # PCB top: full-width horizontal line below the magnet region
    pcb = clusters(row_counts(w, h, d, 60, w - 60, mag_y[3], h - 40), int((w - 120) * 0.55))
    print(f"{label}: {pxmm:.2f} px/mm")
    print(f"  vertical edges px: {['%.0f' % v for v in vx]}")
    print(f"  horizontal edges px: {['%.0f' % v for v in hy]}")
    print(f"  full-width rows (PCB) px: {['%.0f' % v for v in pcb]}")
    return pxmm, vx, hy, pcb

if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    REFS = '/Users/isaacchiu/Documents/GitHub/Giris/hw/refs/switches/'
    job = sys.argv[1]
    if job == 'jadepro':
        r = measure(REFS + 'Gateron_MagneticJadePro-KS-20TF10B045NW-Y106PRO1.pdf', 7,
                    (3600, 1770, 1560, 1330), (2160, 1800, 1200, 1400), (14.00, 14.70),
                    (450, 800, 580, 760), (535, 715, 520, 820), 'JadePro')
        if r:
            pxmm, vx, hy, pcb = r
            # magnet: expect 2 vertical + 2 horizontal magnet edges near the scan box
            print(f"  -> magnet dia {(vx[-1]-vx[0])/pxmm:.2f} mm, len {(hy[-1]-hy[0])/pxmm:.2f} mm")
            print(f"  -> magnet bottom to PCB top: {(pcb[0]-hy[-1])/pxmm:.2f} mm (rest)")
