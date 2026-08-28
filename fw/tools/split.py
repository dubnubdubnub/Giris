#!/usr/bin/env python3
"""
Watch the run-phase data plane — the 48-byte frames that cross J1 at 8 kHz once
peer.c has both halves at 12 Mbaud.

    tools/.venv/bin/python tools/split.py                    # one-shot, both boards
    tools/.venv/bin/python tools/split.py --watch            # live
    tools/.venv/bin/python tools/split.py --soak 120         # the number that matters
    tools/.venv/bin/python tools/split.py --keys             # peer key values, live

--soak is the real test. It puts both halves into test-pattern mode, so the
payload is generated from the sequence number alone and the receiver can
regenerate it — which separates the two failure modes that a CRC alone conflates:

  crc_errors     a frame arrived corrupted, and was correctly thrown away
  payload_errors a frame arrived, PASSED the CRC, and was still wrong

The second must be exactly zero. A nonzero count means a frame was accepted that
was not the frame that was sent, which no amount of retrying fixes.

The other number to read is period_max_us. Frames are paced by each half's own
ADC tick, so the nominal is 125 us; the worst case is how long the receiving
half's main loop ever went without servicing the ring.
"""
import argparse, struct, sys, time
import hid

VID, PID = 0x1209, 0x0001
RPT = 64
CMD_PEER, CMD_SPLIT = 0x0C, 0x0D
RSP_PEER, RSP_SPLIT, RSP_ERROR = 0x8B, 0x8C, 0xEE

PEER_STATE = ("disabled", "discover", "alone", "paired", "switching", "running")
ROLE = ("?", "A", "B")


def list_boards():
    out = {}
    for e in hid.enumerate(VID, PID):
        out.setdefault(e["serial_number"] or "?", e)
    return out


def open_all(serial=None):
    bs = list_boards()
    if not bs:
        sys.exit(f"no Giris found (looked for {VID:04x}:{PID:04x})")
    devs = {}
    for n, e in sorted(bs.items()):
        if serial and serial.upper() not in n.upper():
            continue
        d = hid.device()
        d.open_path(e["path"])
        d.set_nonblocking(0)
        devs[n[-4:]] = d
    if not devs:
        sys.exit(f"{serial!r} matched none of: {', '.join(sorted(bs))}")
    return devs


def txn(d, cmd, arg=0, want=RSP_SPLIT, timeout=2.0):
    buf = bytearray(RPT)
    buf[0], buf[1], buf[2] = cmd, 1, arg
    d.write(bytes([0]) + bytes(buf))
    end = time.time() + timeout
    while time.time() < end:
        r = d.read(RPT, timeout_ms=250)
        if not r:
            continue
        if r[0] == RSP_ERROR:
            sys.exit(f"device rejected 0x{cmd:02X} — firmware predates CMD_SPLIT?")
        if r[0] == want:
            return bytes(r)
    return None


def peer(d):
    r = txn(d, CMD_PEER, 0, RSP_PEER)
    if not r:
        return None
    return dict(state=r[4], role=r[5],
                tag=struct.unpack_from("<H", r, 6)[0],
                peer=struct.unpack_from("<H", r, 8)[0],
                baud=struct.unpack_from("<I", r, 12)[0])


def split(d, arg=0):
    r = txn(d, CMD_SPLIT, arg)
    if not r:
        return None
    f = struct.unpack_from("<6I", r, 4)
    g = struct.unpack_from("<4I", r, 28)
    n = r[49]
    return dict(tx=f[0], rx=f[1], crc=f[2], gaps=f[3], resync=f[4], bad=f[5],
                age=g[0], period=g[1], pmax=g[2], pmin=g[3],
                seq=struct.unpack_from("<H", r, 44)[0],
                flags=r[46], stale=r[47], testing=r[48],
                keys=list(struct.unpack_from("<%dH" % n, r, 50)))


def flagstr(f):
    return "".join(c if f & b else "-" for b, c in ((1, "k"), (2, "t"), (4, "H")))


def header(devs):
    for n, d in devs.items():
        p = peer(d)
        if not p:
            print(f"{n}: no answer to CMD_PEER")
            continue
        print(f"{n}: {PEER_STATE[p['state']]:9s} role={ROLE[p['role']]} "
              f"tag=0x{p['tag']:04X} peer=0x{p['peer']:04X} baud={p['baud']:,}")
        if p["state"] != 5:
            print(f"      not in the run phase — nothing is being framed")
    print()


def line(n, s):
    return (f"{n}  tx {s['tx']:>10,}  rx {s['rx']:>10,}  "
            f"crc {s['crc']:>6,}  gaps {s['gaps']:>6,}  resync {s['resync']:>7,}  "
            f"bad {s['bad']:>4,}  period {s['period']:>4}/{s['pmin']:>4}-{s['pmax']:>5} us  "
            f"{flagstr(s['flags'])}{' STALE' if s['stale'] else ''}")


def one_shot(devs):
    header(devs)
    for n, d in devs.items():
        s = split(d)
        print(line(n, s) if s else f"{n}: no answer")


def watch(devs, period):
    header(devs)
    try:
        while True:
            for n, d in devs.items():
                s = split(d)
                if s:
                    print(line(n, s))
            print()
            time.sleep(period)
    except KeyboardInterrupt:
        pass


def keys(devs, period):
    header(devs)
    try:
        while True:
            for n, d in devs.items():
                s = split(d)
                if not s:
                    continue
                k = " ".join(f"{v:3d}" for v in s["keys"])
                print(f"{n}  {'STALE' if s['stale'] else '  ok '}  peer keys: {k}")
            print()
            time.sleep(period)
    except KeyboardInterrupt:
        pass


def soak(devs, seconds):
    if len(devs) < 2:
        print("note: only one board visible, so this measures one direction only\n")
    header(devs)

    print("test pattern on")
    base = {}
    for n, d in devs.items():
        split(d, 1)                       # enable, and discard the pre-enable stats
    time.sleep(0.3)
    for n, d in devs.items():
        base[n] = split(d)

    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            time.sleep(min(5.0, t0 + seconds - time.time()))
            el = time.time() - t0
            print(f"  {el:6.1f}s", end="")
            for n, d in devs.items():
                s = split(d)
                dr = s["rx"] - base[n]["rx"]
                print(f"   {n} rx {dr:>9,} ({dr/el:>7,.0f}/s)"
                      f" crc {s['crc']-base[n]['crc']:>5,}"
                      f" gap {s['gaps']-base[n]['gaps']:>5,}"
                      f" bad {s['bad']-base[n]['bad']:>3,}", end="")
            print()
    except KeyboardInterrupt:
        print("\ninterrupted")

    el = time.time() - t0
    print(f"\n--- {el:.1f} s ---")
    ok = True
    for n, d in devs.items():
        s = split(d)
        b = base[n]
        rx, tx = s["rx"] - b["rx"], s["tx"] - b["tx"]
        crc, gaps, bad = s["crc"] - b["crc"], s["gaps"] - b["gaps"], s["bad"] - b["bad"]
        lost = gaps / (rx + gaps) if rx + gaps else 0
        print(f"{n}:  tx {tx:,} ({tx/el:,.0f}/s)   rx {rx:,} ({rx/el:,.0f}/s)")
        print(f"      frames lost   {gaps:,}  ({lost*100:.4f} %)")
        print(f"      crc rejects   {crc:,}")
        print(f"      resync slides {s['resync']-b['resync']:,} bytes")
        print(f"      payload errs  {bad:,}   <- must be 0")
        print(f"      inter-arrival {s['pmin']} / {s['period']} / {s['pmax']} us  (min/last/max)")
        if bad or crc or gaps:
            ok = False
    for d in devs.values():
        split(d, 2)                       # back to key data
    print("\ntest pattern off")
    print("\nCLEAN" if ok else "\nERRORS ABOVE — see the counters")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="UID substring picking one board of several")
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--keys", action="store_true")
    ap.add_argument("--soak", type=float, metavar="SECONDS")
    ap.add_argument("--period", type=float, default=1.0)
    a = ap.parse_args()

    devs = open_all(a.serial)
    if a.soak:
        return soak(devs, a.soak)
    if a.keys:
        keys(devs, a.period)
    elif a.watch:
        watch(devs, a.period)
    else:
        one_shot(devs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
