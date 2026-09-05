#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""
Run a half at full load and see whether anything gives: 8 kHz ADC scan, 8 kHz
link frames in both directions, and 8 kHz USB HID telemetry, all at once.

    tools/.venv/bin/python tools/loadtest.py --seconds 180

Each of those has been measured on its own. This is the one that matters for a
split keyboard, because they share one main loop: the link is serviced by
polling a DMA ring from the same loop that runs tud_task(), so USB work delays
link work and vice versa. A stall long enough to lose a link frame shows up in
`gaps`, and a stall long enough to lose a USB report shows up in the firmware's
own tx_dropped counter.

Reports both the host-observed report rate and the device-side rate. The device
side is immune to how fast the host software runs; if they disagree, believe the
device and suspect Python.
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
CMD_INFO, CMD_STREAM_SET, CMD_SNAPSHOT, CMD_PEER, CMD_SPLIT = 0x01, 0x02, 0x03, 0x0C, 0x0D
RSP_INFO, RSP_STREAM, RSP_SNAPSHOT, RSP_PEER, RSP_SPLIT = 0x81, 0x82, 0x83, 0x8B, 0x8C
STATE = ("disabled", "discover", "alone", "paired", "switching", "running", "INCOMPATIBLE")

# Circuit breaker. A healthy 8 kHz run produces zero read errors; anything past
# a few thousand means the host side is not working and the run has no value.
ERROR_BUDGET = 5000


def open_dev(serial=None):
    boards = {}
    for e in giris_interfaces(VID, PID):
        boards.setdefault(e["serial_number"] or "?", e)
    if not boards:
        sys.exit(f"no Giris found ({VID:04x}:{PID:04x})")
    if serial:
        hits = [s for s in boards if serial.upper() in s.upper()]
        if len(hits) != 1:
            sys.exit(f"{serial!r} matches {len(hits)}: {', '.join(sorted(boards))}")
        want = hits[0]
    elif len(boards) > 1:
        sys.exit("several boards — pass --serial: " + ", ".join(sorted(boards)))
    else:
        want = next(iter(boards))
    d = hid.device()
    d.open_path(boards[want]["path"])
    d.set_nonblocking(0)
    return d, want


def send(d, cmd, *args):
    b = bytearray(RPT)
    b[0], b[1] = cmd, 1
    for i, v in enumerate(args):
        b[2 + i] = v
    d.write(bytes([0]) + bytes(b))


def wait(d, want, tmo=2.0):
    end = time.time() + tmo
    while time.time() < end:
        r = d.read(RPT, timeout_ms=250)
        if r and r[0] == want:
            return bytes(r)
    return None


def snapshot(d):
    send(d, CMD_SNAPSHOT)
    r = wait(d, RSP_SNAPSHOT)
    return struct.unpack_from("<I", r, 4)[0], struct.unpack_from("<I", r, 52)[0]


def split_stats(d):
    send(d, CMD_SPLIT)
    r = wait(d, RSP_SPLIT)
    if not r:
        return None
    f = struct.unpack_from("<6I", r, 4)
    g = struct.unpack_from("<4I", r, 28)
    return dict(tx=f[0], rx=f[1], crc=f[2], gaps=f[3], resync=f[4], bad=f[5],
                pmin=g[3], pmax=g[2], flags=r[46], stale=r[47])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial")
    ap.add_argument("--seconds", type=float, default=180.0)
    a = ap.parse_args()

    d, ser = open_dev(a.serial)
    send(d, CMD_INFO)
    info = wait(d, RSP_INFO)
    print(f"board {ser}  proto v{info[4]}  scan {struct.unpack_from('<H', info, 8)[0]} Hz")

    send(d, CMD_PEER, 0)
    p = wait(d, RSP_PEER)
    print(f"link  {STATE[p[4]]}  role={'?AB'[p[5]]}  peer=0x{struct.unpack_from('<H', p, 8)[0]:04X}"
          f"  baud={struct.unpack_from('<I', p, 12)[0]:,}")
    if p[4] != 5:
        print("      not in the run phase — this measures USB only")

    send(d, CMD_STREAM_SET, 0, 1)
    time.sleep(0.2)
    while d.read(RPT, timeout_ms=50):
        pass

    s0 = split_stats(d)
    f0, drop0 = snapshot(d)

    send(d, CMD_STREAM_SET, 1, 1)
    t0 = time.time()
    reports = gaps = read_errors = 0
    last_seq = None
    next_tick = t0 + 15.0
    try:
        while time.time() - t0 < a.seconds:
            now = time.time()
            if now >= next_tick:
                next_tick += 15.0
                el = now - t0
                print(f"  {el:6.1f}s  usb {reports/el:7,.0f}/s  seq gaps {gaps}  "
                      f"read errors {read_errors}")
            try:
                r = d.read(RPT, timeout_ms=100)
            except OSError:
                # Counted, not fatal, and NOT a reason to skip the tick above —
                # a run where every read errors used to print nothing at all.
                read_errors += 1
                # Back off. A failing read returns instantly, so `continue` on
                # its own spins: this loop managed 44,500 failed hid_read calls
                # per second, and the AMD xHCI controller it was aimed at ended
                # up in CM_PROB_FAILED_POST_START. Whatever the root cause of
                # the errors, hammering a sick HID stack flat out is never the
                # right response to it.
                time.sleep(0.001)
                if read_errors > ERROR_BUDGET and reports < read_errors // 10:
                    print(f"\nABORTING: {read_errors:,} read errors against only "
                          f"{reports:,} good reports.")
                    print("The host is not delivering this stream. Stopping rather "
                          "than continuing to pound the HID stack.")
                    break
                continue
            if not r or r[0] != RSP_STREAM:
                continue
            reports += 1
            seq = r[2] | (r[3] << 8)
            if last_seq is not None and ((last_seq + 1) & 0xFFFF) != seq:
                gaps += 1
            last_seq = seq
    finally:
        elapsed = time.time() - t0
        # Read the counters before stopping, because the scan never pauses and
        # anything after the window inflates the rate. But NEVER let a failure
        # here skip the stop below: the first version of this script raised out
        # of snapshot() inside the finally, so the stop never ran and the board
        # was left streaming into a host with nothing reading it. That is the
        # exact condition that took down a USB controller earlier in this
        # bring-up. Teardown must be unconditional.
        try:
            f1, drop1 = snapshot(d)
            s1 = split_stats(d)
        except OSError:
            f1 = drop1 = s1 = None
        for attempt in range(8):
            try:
                send(d, CMD_STREAM_SET, 0, 1)
                time.sleep(0.2)
                stale = 0
                while True:
                    r = d.read(RPT, timeout_ms=50)
                    if not r:
                        break
                    # Only RSP_STREAM counts. CMD_STREAM_SET answers with an
                    # RSP_INFO of its own, so counting every report made this
                    # loop find exactly one "stale" frame on a perfectly clean
                    # stop, every attempt, and warn each time. A warning that
                    # always fires is worse than no warning at all.
                    if r[0] == RSP_STREAM:
                        stale += 1
                if stale == 0:
                    break
            except OSError:
                time.sleep(0.3)
        else:
            print("WARNING: could not confirm the stream stopped")

    print(f"\n--- {elapsed:.1f} s at full load ---")
    print(f"  usb host      {reports/elapsed:9,.0f} reports/s  ({gaps} seq gaps, "
          f"{read_errors} read errors)")
    if f1 is None:
        print("  device counters unavailable — the pipe errored before teardown")
        return 1
    if f1 < f0:
        # The frame counter is monotonic at 8 kHz from boot, so this is not a
        # slow window — it is a reboot inside it. Every rate below would be
        # nonsense, and worse, a soak that quietly spanned a reset would read as
        # a pass. Say so instead.
        print("  *** THE BOARD RESET DURING THIS RUN ***")
        print(f"      frame counter went {f0:,} -> {f1:,}; uptime is now "
              f"{f1/8000:.1f}s, less than the {elapsed:.0f}s window.")
        print("      Nothing else here is meaningful. Check RSP_INFO[48] for why.")
        return 1
    produced, dropped = f1 - f0, drop1 - drop0
    delivered = produced - dropped
    print(f"  uptime        {f1/8000:9,.0f} s        (no reset inside the window)")
    print(f"  scan          {produced/elapsed:9,.0f} frames/s")
    print(f"  usb device    {delivered/elapsed:9,.0f} reports/s  ({dropped:,} dropped by the device)")
    if s0 and s1:
        rx, tx = s1['rx'] - s0['rx'], s1['tx'] - s0['tx']
        lost = s1['gaps'] - s0['gaps']
        print(f"  link tx       {tx/elapsed:9,.0f} frames/s")
        print(f"  link rx       {rx/elapsed:9,.0f} frames/s")
        print(f"  link lost     {lost:9,}   crc {s1['crc']-s0['crc']}  "
              f"resync {s1['resync']-s0['resync']}  payload errs {s1['bad']-s0['bad']}")
        print(f"  link period   {s1['pmin']} .. {s1['pmax']} us")
    return 0


if __name__ == "__main__":
    sys.exit(main())
