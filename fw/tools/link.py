#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""
Exercise the J1 inter-board link (USART6 on PC6/PC7) from the host.

    tools/.venv/bin/python tools/link.py --list
    tools/.venv/bin/python tools/link.py --serial 5C13 --probe     # one board, J1 empty
    tools/.venv/bin/python tools/link.py --continuity              # two boards, is there a wire?
    tools/.venv/bin/python tools/link.py --peer                    # two boards, the real sweep
    tools/.venv/bin/python tools/link.py --soak --baud 13500000

Three tests, in the order you should reach for them:

--probe needs nothing plugged into J1. It drives each link pad open-drain and
reads it back, tells a driven pin from a floating one on /LM_ST, /AP_FAULT and
PC13, and sweeps all sixteen GPIO MUX indices while the USART shifts out 0x00.
That sweep is how MUX8 was confirmed to be USART6 on this part.

--continuity is a DC test with no USART involved at all: one board holds a link
pad low open-drain, the other reads its own pad. Run this FIRST whenever a link
test fails, because a charge-only USB-C cable — which carries VBUS and GND but
neither D+ nor D- — looks from the firmware side exactly like a dead peripheral.

--peer is the real thing: one board transmits 0x00..0xFF while the other
receives by DMA. Half duplex is only swept to 500 kbaud because that is where
the open-drain bus dies; full duplex is swept to the USART's 13.5 Mbaud ceiling.
"""
import argparse, struct, sys, time
import hid

# The board now exposes TWO HID interfaces: the vendor telemetry one this
# protocol lives on, and a System Control one that exists only to make the
# device wake-capable. hid.enumerate() returns both, so anything that picks the
# first match is a coin flip — and landing on System Control looks exactly like
# a dead board, because it answers nothing.
GIRIS_USAGE_PAGE = 0xFF60


def giris_interfaces(vid, pid):
    """Only the vendor telemetry interface, on any platform."""
    es = hid.enumerate(vid, pid)
    hits = [e for e in es if e.get("usage_page") == GIRIS_USAGE_PAGE]
    if not hits:                      # platforms that do not report usage_page
        hits = [e for e in es if e.get("interface_number") in (0, -1, None)]
    return hits or es



VID, PID = 0x1209, 0x0001
RPT = 64
CMD_INFO, CMD_LINK_TEST, CMD_LINK_PROBE, CMD_LINK_HOLD = 0x01, 0x09, 0x0A, 0x0B
RSP_INFO, RSP_LINK, RSP_PROBE, RSP_ERROR = 0x81, 0x89, 0x8A, 0xEE

ROLE_LOOPBACK, ROLE_TX, ROLE_RX = 0, 1, 2
HD_PC6, HD_PC7, FD, FD_SWAP, HD_PP = 1, 2, 3, 4, 5
MODE_NAME = {HD_PC6: "HD PC6", HD_PC7: "HD PC7", FD: "FD", FD_SWAP: "FD SWAP", HD_PP: "HD PP"}

# 216 MHz APB2 with an integer divider and DIV >= 16. These are the exact ones.
FD_LADDER = (1_000_000, 2_000_000, 4_000_000, 6_000_000, 8_000_000,
             9_000_000, 12_000_000, 13_500_000)
HD_LADDER = (115_200, 500_000, 1_000_000, 2_000_000)


def list_boards():
    out = {}
    for e in giris_interfaces(VID, PID):
        out.setdefault(e["serial_number"] or "?", e)
    return out


def open_all():
    bs = list_boards()
    devs = {}
    for n, e in bs.items():
        d = hid.device()
        d.open_path(e["path"])
        d.set_nonblocking(0)
        devs[n] = d
    return devs


def open_one(serial=None):
    bs = list_boards()
    if not bs:
        sys.exit(f"no Giris found (looked for {VID:04x}:{PID:04x})")
    if serial:
        hits = [s for s in bs if serial.upper() in s.upper()]
        if len(hits) != 1:
            sys.exit("%r matches %d boards; attached: %s"
                     % (serial, len(hits), ", ".join(sorted(bs))))
        want = hits[0]
    elif len(bs) > 1:
        sys.exit("%d boards attached — pass --serial with one of: %s"
                 % (len(bs), ", ".join(sorted(bs))))
    else:
        want = next(iter(bs))
    d = hid.device()
    d.open_path(bs[want]["path"])
    d.set_nonblocking(0)
    return d, want


def send(d, cmd, payload=b""):
    buf = bytearray(RPT)
    buf[0], buf[1] = cmd, 1
    buf[2:2 + len(payload)] = payload
    d.write(bytes([0]) + bytes(buf))


def drain(d, want, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = d.read(RPT, timeout_ms=250)
        if not r:
            continue
        if r[0] == RSP_ERROR:
            sys.exit(f"device rejected command 0x{r[4]:02X} — is the firmware current?")
        if r[0] == want:
            return bytes(r)
    return None


def txn(d, cmd, payload=b"", want=RSP_INFO, timeout=5.0):
    send(d, cmd, payload)
    return drain(d, want, timeout)


# ------------------------------------------------------------------ sense

def sense(d):
    return txn(d, CMD_INFO)[47]


def sense_str(v):
    # LM66100 ST is Hi-Z (so R5 pulls it high) while that ideal diode conducts,
    # and pulled low while it blocks. U2's input is VBUS_B off J1.
    # The AP22653 fitted is the active-HIGH variant, so PC13 high = sourcing.
    return ("/LM_ST=%-4s (+5V from %s)   /AP_FAULT=%-4s   PC13=%-4s (%s)   pads PC6=%d PC7=%d"
            % ("high" if v & 1 else "low", "J1" if v & 1 else "own J3",
               "high" if v & 2 else "LOW-FAULT",
               "HIGH" if v & 4 else "low",
               "SOURCING 5V ON J1" if v & 4 else "not sourcing",
               bool(v & 8), bool(v & 16)))


# ------------------------------------------------------------- continuity

def continuity(devs):
    names = sorted(devs)
    if len(names) != 2:
        sys.exit(f"--continuity needs exactly two boards, found {len(names)}")
    for n in names:
        send(devs[n], CMD_LINK_HOLD, bytes([0, 0])); drain(devs[n], RSP_INFO, 2)
        send(devs[n], CMD_LINK_HOLD, bytes([1, 0])); drain(devs[n], RSP_INFO, 2)
        print(f"  {n[-4:]}  {sense_str(sense(devs[n]))}")
    print()
    ok = True
    for pin, label in ((0, "D- (PC6)"), (1, "D+ (PC7)")):
        for drv in names:
            other = [x for x in names if x != drv][0]
            send(devs[drv], CMD_LINK_HOLD, bytes([pin, 1])); drain(devs[drv], RSP_INFO, 2)
            time.sleep(0.05)
            seen = sense(devs[other])
            send(devs[drv], CMD_LINK_HOLD, bytes([pin, 0])); drain(devs[drv], RSP_INFO, 2)
            good = not (seen & (8 if pin == 0 else 16))
            ok &= good
            print(f"  {drv[-4:]} holds {label} low -> {other[-4:]} sees "
                  + ("LOW  == CONTINUITY" if good else "high == NO CONNECTION"))
    if not ok:
        print("\n  No wire. A charge-only USB-C cable carries VBUS and GND but not")
        print("  D+/D-, and from the firmware side that is indistinguishable from a")
        print("  dead peripheral. Swap the cable, or jumper TP1/TP2/TP9 directly.")
    return ok


# -------------------------------------------------------------- link test

def trial(devs, tx, rx, txmode, rxmode, baud, nbytes, tmo=2000):
    """Arm the listener (it blocks in firmware receiving by DMA), then fire."""
    send(devs[rx], CMD_LINK_TEST, struct.pack("<BIHBH", rxmode, baud, nbytes, ROLE_RX, tmo))
    time.sleep(0.15)
    send(devs[tx], CMD_LINK_TEST, struct.pack("<BIHBH", txmode, baud, nbytes, ROLE_TX, tmo))
    drain(devs[tx], RSP_LINK, 6.0)
    r = drain(devs[rx], RSP_LINK, 10.0)
    if not r:
        return None
    _, sent, recv, mism, short = struct.unpack_from("<IHHHH", r, 5)
    return dict(got=recv, bad=mism, short=short, err=r[19],
                over=struct.unpack_from("<H", r, 37)[0],
                first=(r[17], r[18]))


def row(label, baud, n, res):
    if res is None:
        print(f"  {label:32s} {baud:>10,}   no reply")
        return False
    ok = res["bad"] == 0 and res["short"] == 0 and res["err"] == 0
    print(f"  {label:32s} {baud:>10,}  {res['got']:5d}/{n:<5d} bad {res['bad']:4d}  "
          f"missing {res['short']:4d}  err 0x{res['err']:X}   {'PASS' if ok else 'fail'}")
    if res["bad"]:
        print(f"      first mismatch: expected 0x{res['first'][0]:02X}, got 0x{res['first'][1]:02X}")
    return ok


def peer(devs, nbytes):
    names = sorted(devs)
    if len(names) != 2:
        sys.exit(f"--peer needs exactly two boards, found {len(names)}")
    a, b = names
    sa, sb = a[-4:], b[-4:]

    print("\nhalf duplex, open drain, single wire on J1 D- — the discovery bus.")
    print("Both halves run this unswapped; it is the only symmetric channel that")
    print("exists before anyone has agreed who sets TRPSWAP.")
    for baud in HD_LADDER:
        row(f"{sa} -> {sb}  HD open-drain", baud, 1024,
            trial(devs, a, b, HD_PC6, HD_PC6, baud, 1024))

    print("\nfull duplex, push-pull — the run phase. A straight-through USB-C cable")
    print("joins D+ to D+ and D- to D-, so the two halves must be TRPSWAP opposites.")
    allok = True
    for baud in FD_LADDER:
        for tx, rx in ((a, b), (b, a)):
            allok &= row(f"{tx[-4:]} -> {rx[-4:]}  FD (peer swapped)", baud, nbytes,
                         trial(devs, tx, rx, FD, FD_SWAP, baud, nbytes))
    return allok


def soak(devs, baud, nbytes, runs):
    names = sorted(devs)
    if len(names) != 2:
        sys.exit(f"--soak needs exactly two boards, found {len(names)}")
    a, b = names
    tot = bad = short = err = 0
    for i in range(runs):
        tx, rx = (a, b) if i % 2 == 0 else (b, a)
        r = trial(devs, tx, rx, FD, FD_SWAP, baud, nbytes)
        if r is None:
            print("  dropped run")
            continue
        tot += r["got"]; bad += r["bad"]; short += r["short"]; err |= r["err"]
    print(f"  {baud:,} baud, {runs} runs alternating direction: {tot:,} bytes, "
          f"{bad} corrupt, {short} missing, err 0x{err:X}   "
          + ("CLEAN" if not (bad or short or err) else "ERRORS"))
    return not (bad or short or err)


# ------------------------------------------------------------------ probe

def probe(d, mode, baud):
    r = txn(d, CMD_LINK_PROBE, struct.pack("<BI", mode, baud), want=RSP_PROBE, timeout=20.0)
    if not r:
        sys.exit("no RSP_PROBE — firmware predates CMD_LINK_PROBE")
    pads, pu, pd = r[4], r[5], r[6]
    nsamp = struct.unpack_from("<H", r, 7)[0]
    mux = list(struct.unpack_from("<16H", r, 9))

    print("  pads (open-drain low / released to the fitted 10k):")
    for i, name in ((0, "PC6 (J1 D-)"), (2, "PC7 (J1 D+)")):
        print("    %s: driven -> %s   released -> %s"
              % (name,
                 "low " if pads & (1 << i) else "STUCK HIGH",
                 "high" if pads & (2 << i) else "STUCK LOW (no pull-up?)"))

    print("  sense pins, internal pull-up vs pull-down:")
    for b, name in ((1, "PB12 /LM_ST  "), (2, "PB10 /AP_FAULT")):
        x, y = bool(pu & b), bool(pd & b)
        # R5's 10k pull-up beats the MCU's ~40k internal pull-down, so agreement
        # is only conclusive when it agrees LOW.
        state = "driven low" if (x == y and not x) else \
                ("driven high or floating — R5's 10k outvotes the internal pull-down"
                 if x == y else "FLOATING")
        print(f"    {name}: pu={int(x)} pd={int(y)}  -> {state}")

    print(f"  MUX sweep, TX pad low out of {nsamp} samples while shifting 0x00:")
    lo, hi = int(0.55 * nsamp), int(0.97 * nsamp)
    hits = [i for i in range(16) if lo <= mux[i] <= hi]
    for i in range(16):
        if mux[i] > nsamp // 20:
            note = "  <== USART6 (modulated)" if i in hits else "  (stuck, not a signal)"
            print(f"    MUX{i:<2d} {mux[i]:5d}  {100*mux[i]/nsamp:5.1f}%{note}")
    if not hits:
        print("    no MUX index produced a modulated pad — the USART never transmitted")
    return hits[0] if hits else None


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="UID substring picking one board of several")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--continuity", action="store_true")
    ap.add_argument("--peer", action="store_true")
    ap.add_argument("--soak", action="store_true")
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--mode", type=int, choices=sorted(MODE_NAME))
    ap.add_argument("--baud", type=int, default=13_500_000)
    ap.add_argument("--bytes", type=int, default=4096)
    ap.add_argument("--runs", type=int, default=40)
    a = ap.parse_args()

    if a.list:
        for s, e in sorted(list_boards().items()):
            print(f"{s}  {e['product_string']}")
        sys.exit(0)

    if a.continuity or a.peer or a.soak:
        devs = open_all()
        if a.continuity:
            sys.exit(0 if continuity(devs) else 1)
        if a.soak:
            sys.exit(0 if soak(devs, a.baud, a.bytes, a.runs) else 1)
        if not continuity(devs):
            sys.exit("\nstopping: there is no wire, so a link result would mean nothing")
        sys.exit(0 if peer(devs, a.bytes) else 1)

    dev, serial = open_one(a.serial)
    info = txn(dev, CMD_INFO)
    print(f"board {serial}  tag 0x{struct.unpack_from('<H', info, 45)[0]:04X}  proto v{info[4]}")
    print(f"  {sense_str(info[47])}")
    if a.probe:
        probe(dev, a.mode or HD_PC6, 115200)
        sys.exit(0)
    ap.error("pass --probe, --continuity, --peer or --soak")
