#!/usr/bin/env python3
"""
Record Giris telemetry straight to CSV, without the browser in the loop.

    tools/.venv/bin/python tools/capture.py --seconds 20 --out press.csv
    tools/.venv/bin/python tools/capture.py --burst 3 --count 4096 --out noise.csv

macOS lets only one process own a HID device at a time, so close the browser
viewer's connection first (or just reload the page) before running this.
"""
import argparse, csv, struct, sys, time
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
CMD_INFO, CMD_STREAM_SET, CMD_SNAPSHOT = 0x01, 0x02, 0x03
CMD_BURST_START, CMD_BURST_STATUS, CMD_BURST_READ = 0x04, 0x05, 0x06
RSP_INFO, RSP_STREAM, RSP_BURST_STATUS, RSP_BURST_DATA = 0x81, 0x82, 0x85, 0x86
FRAMES_PER_RPT, FRAME_BYTES, STREAM_HDR, BURST_PER_RPT = 3, 16, 8, 28


def list_boards():
    """Every Giris on the bus, deduped by serial.

    hidapi enumerates one entry per HID collection, so a two-endpoint device can
    appear twice with the same path prefix; keying on the serial is what makes
    "how many boards are attached" answerable."""
    out = {}
    for e in giris_interfaces(VID, PID):
        out.setdefault(e["serial_number"] or "?", e)
    return out


def open_dev(serial=None):
    """serial may be any unique substring of the 24-hex UID — a 4-char suffix is
    plenty to tell two boards apart and much easier to type."""
    boards = list_boards()
    if not boards:
        sys.exit(f"no Giris found (looked for {VID:04x}:{PID:04x})")

    if serial:
        hits = [s for s in boards if serial.upper() in s.upper()]
        if not hits:
            sys.exit("no board matches %r; attached: %s" % (serial, ", ".join(sorted(boards))))
        if len(hits) > 1:
            sys.exit("%r matches %d boards: %s" % (serial, len(hits), ", ".join(sorted(hits))))
        want = hits[0]
    elif len(boards) > 1:
        sys.exit("%d boards attached — pass --serial with one of: %s"
                 % (len(boards), ", ".join(sorted(boards))))
    else:
        want = next(iter(boards))

    d = hid.device()
    d.open_path(boards[want]["path"])
    d.set_nonblocking(0)
    return d


def send(d, cmd, *args):
    buf = [0] * RPT
    buf[0], buf[1] = cmd, 1
    for i, v in enumerate(args):
        buf[2 + i] = v
    d.write(bytes([0] + buf))          # leading 0 = report ID for hidapi
    return 1


def wait_for(d, msg, timeout=2.0):
    end = time.time() + timeout
    while time.time() < end:
        r = d.read(RPT, timeout_ms=200)
        if r and r[0] == msg:
            return bytes(r)
    return None


def get_info(d):
    send(d, CMD_INFO)
    r = wait_for(d, RSP_INFO)
    if not r:
        sys.exit("no response to INFO — is the browser viewer still connected?")
    ns = r[6]
    return {
        "version": r[4], "num_keys": r[5], "num_slots": ns,
        "scan_hz": struct.unpack_from("<H", r, 8)[0],
        "slot_map": list(r[10:10 + ns]),
        "counts_per_gauss": struct.unpack_from("<I", r, 20)[0] / 256.0,
        "sequence": list(r[28:33]),
        "uid": r[33:45].hex().upper(),
        "uid_tag": struct.unpack_from("<H", r, 45)[0],
        "sense": r[47],
    }


def stream(d, info, seconds, decim, out):
    send(d, CMD_STREAM_SET, 1, decim)
    rows, end, last = [], time.time() + seconds, None
    gaps = 0
    while time.time() < end:
        r = d.read(RPT, timeout_ms=200)
        if not r or r[0] != RSP_STREAM:
            continue
        n = r[4]
        if r[5] & 1:
            gaps += 1
        for i in range(n):
            b = STREAM_HDR + i * FRAME_BYTES
            frame = struct.unpack_from("<I", bytes(r), b)[0]
            if last is not None and frame <= last:
                continue
            last = frame
            keys = struct.unpack_from("<6H", bytes(r), b + 4)
            rows.append((frame / info["scan_hz"], *keys))
    send(d, CMD_STREAM_SET, 0, decim)

    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s"] + [f"key{k}" for k in range(info["num_keys"])])
        w.writerows(rows)
    print(f"{len(rows)} frames -> {out}  ({gaps} gap flags)")
    if rows:
        for k in range(info["num_keys"]):
            col = [r[1 + k] for r in rows]
            print(f"  key{k}: min {min(col)}  max {max(col)}  span {max(col)-min(col)}")


def burst(d, info, slot, count, out):
    send(d, CMD_BURST_START, slot, count & 0xFF, count >> 8)
    wait_for(d, RSP_BURST_STATUS)
    captured = 0
    for _ in range(300):
        send(d, CMD_BURST_STATUS)
        r = wait_for(d, RSP_BURST_STATUS)
        if r:
            state, captured = r[4], struct.unpack_from("<H", bytes(r), 6)[0]
            if state == 2:
                break
        time.sleep(0.05)
    period_ns = struct.unpack_from("<I", bytes(r), 8)[0] if r else 0

    samples = [0] * captured
    off = 0
    while off < captured:
        send(d, CMD_BURST_READ, off & 0xFF, off >> 8)
        c = wait_for(d, RSP_BURST_DATA)
        if not c:
            break
        o, n = struct.unpack_from("<H", bytes(c), 4)[0], c[6]
        if n == 0:
            break
        samples[o:o + n] = struct.unpack_from(f"<{n}H", bytes(c), 7)
        off = o + n

    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["n", "counts"])
        w.writerows(enumerate(samples))
    fs = 1e9 / period_ns if period_ns else 0
    print(f"{len(samples)} samples of slot {slot} @ {fs/1000:.2f} kHz -> {out}")
    if samples:
        mean = sum(samples) / len(samples)
        var = sum((s - mean) ** 2 for s in samples) / len(samples)
        print(f"  mean {mean:.2f}  rms {var**0.5:.3f} LSB  pp {max(samples)-min(samples)}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--decim", type=int, default=8)
    ap.add_argument("--burst", type=int, default=None, help="slot 0-7")
    ap.add_argument("--count", type=int, default=4096)
    ap.add_argument("--out", default="giris.csv")
    ap.add_argument("--serial", help="UID substring picking one board of several")
    ap.add_argument("--list", action="store_true", help="show attached boards and exit")
    a = ap.parse_args()

    if a.list:
        for s, e in sorted(list_boards().items()):
            print(f"{s}  {e['product_string']}")
        sys.exit(0)

    dev = open_dev(a.serial)
    info = get_info(dev)
    print(f"board  {info['uid']}  tag 0x{info['uid_tag']:04X}")
    print(f"proto v{info['version']}  {info['num_keys']} keys  {info['scan_hz']} Hz  "
          f"{info['counts_per_gauss']:.3f} counts/Gs")
    print(f"  sequence (rank1..5 = ADC ch): {info['sequence']}   slot_map: {info['slot_map']}")
    if a.burst is not None:
        burst(dev, info, a.burst, a.count, a.out)
    else:
        stream(dev, info, a.seconds, a.decim, a.out)
    dev.close()
