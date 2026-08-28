#!/usr/bin/env python3
"""
Exercise the J1 inter-board link (USART6 on PC6/PC7) from the host.

    tools/.venv/bin/python tools/link.py --list
    tools/.venv/bin/python tools/link.py --selftest            # J1 EMPTY, both wires
    tools/.venv/bin/python tools/link.py --mode 3 --baud 9000000   # needs a loopback or peer

--probe is the one that works on a board by itself: it drives each link pad
open-drain and reads it back, tells driven from floating on /LM_ST and
/AP_FAULT, and sweeps all sixteen MUX indices watching the pad while the USART
shifts out 0x00. That is how MUX8 was confirmed to be USART6 on this part.

--selftest and --mode send 0x00..0xFF and report what came back, so they need the
loop closed OUTSIDE the chip: a peer on J1, or a jumper from TP1 (J1 D+) to TP2
(J1 D-). This silicon does not echo its own half-duplex transmission into RDBF,
whatever RM 12.2 implies — measured 2026-08-28 on both boards.
"""
import argparse, struct, sys, time
import hid

VID, PID = 0x1209, 0x0001
RPT = 64
CMD_INFO, CMD_LINK_TEST, CMD_LINK_PROBE = 0x01, 0x09, 0x0A
RSP_INFO, RSP_LINK, RSP_PROBE, RSP_ERROR = 0x81, 0x89, 0x8A, 0xEE

MODES = {
    1: ("HD PC6", "half duplex, open drain, J1 D-", False),
    2: ("HD PC7", "half duplex, open drain, J1 D+ (TRPSWAP)", False),
    3: ("FD",     "full duplex, TX PC6 (D-) / RX PC7 (D+)", True),
    4: ("FD SWAP","full duplex, TX PC7 (D+) / RX PC6 (D-)", True),
    5: ("HD PP",  "half duplex on PC6, push-pull (diagnostic)", False),
}


def list_boards():
    out = {}
    for e in hid.enumerate(VID, PID):
        out.setdefault(e["serial_number"] or "?", e)
    return out


def open_dev(serial=None):
    boards = list_boards()
    if not boards:
        sys.exit(f"no Giris found (looked for {VID:04x}:{PID:04x})")
    if serial:
        hits = [s for s in boards if serial.upper() in s.upper()]
        if len(hits) != 1:
            sys.exit("%r matches %d boards; attached: %s"
                     % (serial, len(hits), ", ".join(sorted(boards))))
        want = hits[0]
    elif len(boards) > 1:
        sys.exit("%d boards attached — pass --serial with one of: %s"
                 % (len(boards), ", ".join(sorted(boards))))
    else:
        want = next(iter(boards))
    d = hid.device()
    d.open_path(boards[want]["path"])
    d.set_nonblocking(0)
    return d, want


def txn(d, cmd, payload=b"", want=None, timeout=3.0):
    buf = bytearray(RPT)
    buf[0], buf[1] = cmd, 1
    buf[2:2 + len(payload)] = payload
    d.write(bytes([0]) + bytes(buf))
    end = time.time() + timeout
    while time.time() < end:
        r = d.read(RPT, timeout_ms=250)
        if not r:
            continue
        if r[0] == RSP_ERROR:
            sys.exit(f"device rejected command 0x{r[4]:02X} — is the firmware current?")
        if want is None or r[0] == want:
            return bytes(r)
    return None


def sense_str(v):
    return ("PB12 /LM_ST=%s  PB10 /AP_FAULT=%s"
            % ("high" if v & 1 else "low", "high" if v & 2 else "low"))


def run(d, mode, baud, nbytes):
    r = txn(d, CMD_LINK_TEST, struct.pack("<BIH", mode, baud, nbytes), want=RSP_LINK, timeout=10.0)
    if not r:
        sys.exit("no RSP_LINK — firmware predates CMD_LINK_TEST, or the test hung")
    got_mode = r[4]
    got_baud, sent, recv, mism, tmo = struct.unpack_from("<IHHHH", r, 5)
    bad_tx, bad_rx, errs, sense = r[17], r[18], r[19], r[20]

    name, why, external = MODES.get(got_mode, ("?", "?", False))
    ok = (recv == sent) and mism == 0 and tmo == 0 and errs == 0
    print(f"  mode {got_mode} {name:8s} {got_baud:>9,} baud  "
          f"sent {sent:4d}  back {recv:4d}  bad {mism:4d}  timeout {tmo:4d}  "
          f"err 0x{errs:X}   {'PASS' if ok else 'FAIL'}")
    if mism:
        print(f"      first mismatch: sent 0x{bad_tx:02X}, got 0x{bad_rx:02X}")
    if errs:
        flags = [n for b, n in ((1, "PERR"), (2, "FERR"), (4, "NERR"), (8, "ROERR")) if errs & b]
        print("      usart errors: " + " ".join(flags))
    sts, c1, c2, c3 = struct.unpack_from("<IIII", r, 21)
    print(f"      USART6  STS 0x{sts:08X}  CTRL1 0x{c1:08X}  CTRL2 0x{c2:08X}  CTRL3 0x{c3:08X}"
          f"   UEN={c1>>13&1} TEN={c1>>3&1} REN={c1>>2&1}"
          f" TRPSWAP={c2>>15&1} SLBEN={c3>>3&1}")
    if not ok and external:
        print(f"      ({why} — needs a TP1-TP2 jumper or a peer echoing)")
    return ok, sense


def probe(d, mode, baud):
    r = txn(d, CMD_LINK_PROBE, struct.pack("<BI", mode, baud), want=RSP_PROBE, timeout=20.0)
    if not r:
        sys.exit("no RSP_PROBE — firmware predates CMD_LINK_PROBE")
    pads, pu, pd = r[4], r[5], r[6]
    nsamp = struct.unpack_from("<H", r, 7)[0]
    mux = list(struct.unpack_from("<16H", r, 9))

    print("  pads (open-drain low / released to the fitted 10k):")
    for i, name in ((0, "PC6 (J1 D-)"), (2, "PC7 (J1 D+)")):
        lo = "low " if pads & (1 << i) else "STUCK HIGH"
        hi = "high" if pads & (2 << i) else "STUCK LOW (no pull-up?)"
        print(f"    {name}: driven -> {lo}   released -> {hi}")

    print("  sense pins, internal pull-up vs pull-down:")
    for b, name, note in ((1, "PB12 /LM_ST  ", "U2 LM66100 ST, the VBUS_B ideal diode"),
                          (2, "PB10 /AP_FAULT", "AP22653 overcurrent")):
        a, c = bool(pu & b), bool(pd & b)
        state = ("driven high" if a else "driven low") if a == c else "FLOATING"
        print(f"    {name}: pu={int(a)} pd={int(c)}  -> {state}   ({note})")

    print(f"  MUX sweep, TX pad low out of {nsamp} samples while shifting 0x00:")
    # A transmitting line is MODULATED. A run of 0x00 is low 9 bits in 10, so the
    # routed index lands in a band; a flat 100% is an undriven pad sitting low and
    # a flat 0% is one held high. Picking the maximum would always name the stuck
    # pin, so score the band instead.
    lo, hi = int(0.55 * nsamp), int(0.97 * nsamp)
    hits = [i for i in range(16) if lo <= mux[i] <= hi]
    for i in range(16):
        if mux[i] > nsamp // 20:
            note = "  <== USART6 (modulated)" if i in hits else "  (stuck, not a signal)"
            print(f"    MUX{i:<2d} {mux[i]:5d}  {100*mux[i]/nsamp:5.1f}%{note}")
    if not hits:
        print("    no MUX index produced a modulated pad — the USART never transmitted")
    elif len(hits) > 1:
        print(f"    ambiguous: {hits}")
    return hits[0] if hits else None


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="UID substring picking one board of several")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--probe", action="store_true",
                    help="electrical check of the link pins and a MUX sweep; J1 must be EMPTY")
    ap.add_argument("--selftest", action="store_true",
                    help="both half-duplex wires at 115200 and 9 Mbaud; J1 must be EMPTY")
    ap.add_argument("--mode", type=int, choices=sorted(MODES))
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--bytes", type=int, default=256)
    a = ap.parse_args()

    if a.list:
        for s, e in sorted(list_boards().items()):
            print(f"{s}  {e['product_string']}")
        sys.exit(0)

    dev, serial = open_dev(a.serial)
    info = txn(dev, CMD_INFO, want=RSP_INFO)
    if not info:
        sys.exit("no response to INFO")
    print(f"board {serial}  tag 0x{struct.unpack_from('<H', info, 45)[0]:04X}  "
          f"proto v{info[4]}")
    print(f"  {sense_str(info[47])}")

    if a.probe:
        probe(dev, a.mode or 1, a.baud)
        sys.exit(0)

    if a.selftest:
        allok = True
        for mode in (1, 2):
            for baud in (115200, 9000000):
                ok, _ = run(dev, mode, baud, a.bytes)
                allok &= ok
        print("\nself-test " + ("PASSED" if allok else "FAILED"))
        sys.exit(0 if allok else 1)

    if a.mode is None:
        ap.error("pass --selftest or --mode")
    ok, _ = run(dev, a.mode, a.baud, a.bytes)
    sys.exit(0 if ok else 1)
