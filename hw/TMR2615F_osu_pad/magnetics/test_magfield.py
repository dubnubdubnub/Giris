"""Checks for the cylinder-magnet field model. Run: python3 test_magfield.py"""
import math

from magfield import MU0, cylinder_field, ellipKE, loop_field

FAIL = []


def check(name, got, want, rtol=1e-6):
    ok = abs(got - want) <= rtol * max(abs(want), 1e-30)
    print(f"{'ok  ' if ok else 'FAIL'} {name}: got {got:.10g}, want {want:.10g} (rtol {rtol:g})")
    if not ok:
        FAIL.append(name)


# elliptic integrals against known values (parameter m convention, same as scipy)
K0, E0 = ellipKE(0.0)
check("K(0) = pi/2", K0, math.pi / 2)
check("E(0) = pi/2", E0, math.pi / 2)
K5, E5 = ellipKE(0.5)
check("K(0.5)", K5, 1.8540746773013719, rtol=1e-12)
check("E(0.5)", E5, 1.3506438810476755, rtol=1e-12)

# loop on-axis field vs closed form Bz = mu0 I a^2 / 2(a^2+z^2)^1.5
a, I, z = 3e-3, 2.0, 4e-3
_, bz = loop_field(a, I, 1e-12, z)
check("loop on-axis", bz, MU0 * I * a * a / (2 * (a * a + z * z) ** 1.5))

# loop Br antisymmetric in z; Bz symmetric
br_p, bz_p = loop_field(a, I, 2e-3, 3e-3)
br_m, bz_m = loop_field(a, I, 2e-3, -3e-3)
check("loop Br antisymmetric", br_p, -br_m, rtol=1e-9)
check("loop Bz symmetric", bz_p, bz_m, rtol=1e-9)

# cylinder on-axis face field vs analytic Bz = Br/2 * L/sqrt(L^2 + a^2)
Br_rem, dia, L = 1.3, 10e-3, 10e-3
_, bz = cylinder_field(dia / 2, L, Br_rem, 0.0, 1e-12, -1e-9)
check("cylinder face field", bz, Br_rem / 2 * L / math.hypot(L, dia / 2), rtol=1e-4)

# far field approaches dipole: |Bz(axis)| -> mu0 * 2m / (4 pi z^3), m = Br V / mu0
# (on-axis Bz has the same sign above and below an axially magnetized cylinder)
zfar = 0.5  # 50 cm away from a 10x10mm magnet
vol = math.pi * (dia / 2) ** 2 * L
m_dip = Br_rem * vol / MU0
_, bz = cylinder_field(dia / 2, L, Br_rem, -L / 2, 1e-12, -zfar)  # centered at origin
check("dipole far-field limit", bz, MU0 * 2 * m_dip / (4 * math.pi * zfar**3), rtol=1e-3)

# radial component vanishes on axis
br, _ = cylinder_field(dia / 2, L, Br_rem, 0.0, 0.0, 5e-3)
assert br == 0.0, f"Br on axis should be exactly 0, got {br}"
print("ok   Br = 0 on axis")

print()
if FAIL:
    raise SystemExit(f"{len(FAIL)} check(s) failed: {FAIL}")
print("all checks passed")
