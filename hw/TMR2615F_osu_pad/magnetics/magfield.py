"""Field of an axially magnetized cylinder magnet at an off-axis point.

Model: equivalent solenoid (azimuthal surface current sheet), integrated as a
stack of current loops. Loop field off-axis via complete elliptic integrals
(AGM implementation, no scipy needed).

Coordinates: cylindrical (r, z), magnet axis = z axis.
Magnet occupies z in [z0, z0 + L], radius a, remanence Br_rem (tesla).
Positive Br_rem = magnetized in +z (N pole at top face, S pole at bottom face).
Returns (B_r, B_z) in tesla at point (r, z).
"""
import math

MU0 = 4e-7 * math.pi


def ellipKE(m):
    """Complete elliptic integrals K(m), E(m), parameter m = k^2, 0 <= m < 1."""
    if m < 0 or m >= 1:
        raise ValueError(f"m out of range: {m}")
    a, b, c = 1.0, math.sqrt(1.0 - m), math.sqrt(m)
    s = 0.5 * m  # sum of 2^(n-1) c_n^2 starting at n=0: 2^-1 * c0^2
    n = 0
    while abs(c) > 1e-15 and n < 60:
        a, b, c = (a + b) / 2.0, math.sqrt(a * b), (a - b) / 2.0
        n += 1
        s += 2.0 ** (n - 1) * c * c
    K = math.pi / (2.0 * a)
    E = K * (1.0 - s)
    return K, E


def loop_field(a, I, r, z):
    """B field (Br, Bz) of a circular loop radius a, current I, loop in z=0 plane."""
    if r < 1e-12:
        bz = MU0 * I * a * a / (2.0 * (a * a + z * z) ** 1.5)
        return 0.0, bz
    alpha2 = a * a + r * r + z * z - 2 * a * r
    beta2 = a * a + r * r + z * z + 2 * a * r
    beta = math.sqrt(beta2)
    m = 1.0 - alpha2 / beta2  # k^2
    K, E = ellipKE(m)
    C = MU0 * I / math.pi
    br = C * z / (2 * alpha2 * beta * r) * ((a * a + r * r + z * z) * E - alpha2 * K)
    bz = C / (2 * alpha2 * beta) * ((a * a - r * r - z * z) * E + alpha2 * K)
    return br, bz


def cylinder_field(a, L, Br_rem, z0, r, z, nslices=400):
    """(Br, Bz) at (r, z) from axially magnetized cylinder, bottom face at z0."""
    Itot = Br_rem / MU0 * L  # total equivalent surface current
    dI = Itot / nslices
    br = bz = 0.0
    for i in range(nslices):
        zl = z0 + (i + 0.5) * L / nslices
        dbr, dbz = loop_field(a, dI, r, z - zl)
        br += dbr
        bz += dbz
    return br, bz


def sweep(name, dia, L, Br_rem, r_sensor, z_sensor, z_bottom_list):
    print(f"\n{name}: Ø{dia}mm x {L}mm, Br={Br_rem}T, sensor r={r_sensor}mm z={z_sensor}mm above PCB")
    print(f"{'magnet gap(mm)':>14} {'|Br| in-plane (mT)':>19} {'(Gs)':>7} {'Bz axial@r (mT)':>16} {'Bz on-axis (mT)':>16}")
    for zb in z_bottom_list:
        a = dia / 2 * 1e-3
        br, bz = cylinder_field(a, L * 1e-3, Br_rem, zb * 1e-3, r_sensor * 1e-3, z_sensor * 1e-3)
        _, bz_axis = cylinder_field(a, L * 1e-3, Br_rem, zb * 1e-3, 1e-9, z_sensor * 1e-3)
        print(f"{zb - z_sensor:>14.2f} {br*1e3:>19.2f} {br*1e4:>7.0f} {bz*1e3:>16.2f} {bz_axis*1e3:>16.2f}")


if __name__ == "__main__":
    # sanity check: on-axis field at face-center of Ø10x10mm N42 (Br=1.3T) magnet
    # analytic: Bz = Br/2 * (L/sqrt(L^2+a^2)) at face = 1.3/2*(10/sqrt(125)) = 0.5814T
    br, bz = cylinder_field(5e-3, 10e-3, 1.3, 0.0, 1e-9, -1e-9)
    print(f"sanity: {bz:.4f} T (expect ~0.5814)")
