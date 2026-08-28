#!/usr/bin/env python3
"""
Measure the actual USB polling rate the host gives us.

    tools/.venv/bin/python tools/ratetest.py --seconds 5

This is the 8 kHz claim, checked rather than asserted. It works because the
streaming path emits exactly ONE frame per report: adc_read_frame() always
returns the newest complete frame, so the pack loop breaks on its second
iteration. The device therefore wants to send a report at the full scan rate and
achieves min(poll rate, scan rate).

Two independent numbers come out, and they should agree:

  host-observed   reports/s counted here — can be limited by Python, not the bus
  device-side     (frames produced - reports dropped) / elapsed, read out of the
                  firmware's own tx_dropped counter, which is immune to how fast
                  the host software is

bInterval = 1 means 1 ms at full speed and 125 us at high speed, so ~8000/s is
proof of both high speed and an honoured interval; ~1000/s means the device fell
back to full speed or the host ignored the exponent.
"""
import argparse, struct, sys, time
import hid

VID, PID = 0x1209, 0x0001
RPT = 64
CMD_INFO, CMD_STREAM_SET, CMD_SNAPSHOT = 0x01, 0x02, 0x03
RSP_INFO, RSP_STREAM, RSP_SNAPSHOT = 0x81, 0x82, 0x83


def open_dev(serial=None):
    boards = {}
    for e in hid.enumerate(VID, PID):
        boards.setdefault(e["serial_number"] or "?", e)
    if not boards:
        sys.exit(f"no Giris found ({VID:04x}:{PID:04x})")
    if serial:
        hits = [s for s in boards if serial.upper() in s.upper()]
        if len(hits) != 1:
            sys.exit("%r matches %d of: %s" % (serial, len(hits), ", ".join(sorted(boards))))
        want = hits[0]
    elif len(boards) > 1:
        sys.exit("%d boards — pass --serial from: %s" % (len(boards), ", ".join(sorted(boards))))
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


def wait(d, msg, timeout=2.0):
    # Tolerate a host-side read error rather than aborting: on Windows a long
    # 8 kHz read loop in Python eventually wedges the HID handle, and that is a
    # limit of this tool, not of the bus. The device-side counters are the point.
    end = time.time() + timeout
    while time.time() < end:
        try:
            r = d.read(RPT, timeout_ms=200)
        except OSError:
            time.sleep(0.05)
            continue
        if r and r[0] == msg:
            return bytes(r)
    return None


def snapshot(d):
    send(d, CMD_SNAPSHOT)
    r = wait(d, RSP_SNAPSHOT)
    if not r:
        sys.exit("no snapshot")
    return struct.unpack_from("<I", r, 4)[0], struct.unpack_from("<I", r, 52)[0]


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--no-read", action="store_true",
                    help="load the bus without draining, so Python cannot be the limit")
    a = ap.parse_args()

    dev, ser = open_dev(a.serial)
    send(dev, CMD_INFO)
    info = wait(dev, RSP_INFO)
    if not info:
        sys.exit("no INFO")
    scan_hz = struct.unpack_from("<H", info, 8)[0]
    print(f"board {ser}  proto v{info[4]}  scan {scan_hz} Hz")

    send(dev, CMD_STREAM_SET, 0, 1)
    time.sleep(0.2)
    while dev.read(RPT, timeout_ms=50):
        pass
    f0, d0 = snapshot(dev)

    send(dev, CMD_STREAM_SET, 1, 1)
    t0 = time.time()
    reports = frames = gaps = 0
    last_seq = None
    read_errors = 0
    while time.time() - t0 < a.seconds:
        if a.no_read:
            # Do not drain. The host controller keeps polling the endpoint at the
            # negotiated interval regardless of whether anything reads, so this
            # loads the BUS at 8 kHz without Python in the path. Use it to tell a
            # host-software limit apart from a real one.
            time.sleep(0.05)
            continue
        try:
            r = dev.read(RPT, timeout_ms=100)
        except OSError:
            # Do not abort the soak for a host-side hiccup; the device-side
            # counters are what we are actually here to read.
            read_errors += 1
            continue
        if not r or r[0] != RSP_STREAM:
            continue
        reports += 1
        frames += r[4]
        seq = r[2] | (r[3] << 8)
        if last_seq is not None and ((last_seq + 1) & 0xFFFF) != seq:
            gaps += 1
        last_seq = seq
    elapsed = time.time() - t0
    # Snapshot BEFORE stopping the stream. The ADC scan never pauses, so anything
    # between the window and the snapshot lands in `produced` and inflates the
    # scan rate — 0.25 s of teardown across a 5 s window is a 5 % error.
    f1, d1 = snapshot(dev)
    send(dev, CMD_STREAM_SET, 0, 1)
    time.sleep(0.2)
    while dev.read(RPT, timeout_ms=50):
        pass

    produced = f1 - f0
    dropped = d1 - d0
    delivered = produced - dropped

    print(f"\n{elapsed:.2f} s")
    print(f"  scan rate        {produced/elapsed:9.0f} frames/s   (firmware frame counter)")
    if a.no_read:
        print(f"  host-observed          n/a             (--no-read: nothing drained)")
    else:
        print(f"  host-observed    {reports/elapsed:9.0f} reports/s  ({frames} frames, "
              f"{gaps} seq gaps, {read_errors} read errors)")
    print(f"  device-side      {delivered/elapsed:9.0f} reports/s  ({dropped} dropped by the device)")
    best = delivered / elapsed if a.no_read else max(reports / elapsed, delivered / elapsed)
    if best > 6000:
        print("\n  -> 8 kHz: high speed and bInterval=1 both honoured")
    elif best > 3000:
        print(f"\n  -> ~{best:.0f} Hz — above 1 kHz, so high speed, but not the full 8 kHz")
    else:
        print(f"\n  -> ~{best:.0f} Hz — full-speed behaviour, or the interval is being ignored")
