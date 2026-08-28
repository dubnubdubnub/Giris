#!/usr/bin/env python3
"""
Put a running Giris into the ROM bootloader, by name.

    tools/.venv/bin/python tools/dfu.py --list
    tools/.venv/bin/python tools/dfu.py --serial A1B2

The board resets and comes back as 2e3c:df11 on its J2, ready for
`tools/flash.sh build/giris.bin <path>`. It prints the path it found, so with two
boards attached you can tell which one is now sitting in DFU.

BOOT0 + NRST is still the recovery path: the ROM bootloader runs before user
code, so a bad image cannot lock you out.
"""
import argparse, subprocess, sys, time
import hid

VID, PID = 0x1209, 0x0001
CMD_BOOTLOADER = 0x7E


def boards():
    out = {}
    for e in hid.enumerate(VID, PID):
        out.setdefault(e["serial_number"] or "?", e)
    return out


def dfu_paths():
    try:
        out = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return set()
    return {ln.split('path="')[1].split('"')[0]
            for ln in out.splitlines() if "2e3c:df11" in ln and 'path="' in ln}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="UID substring picking one board of several")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    b = boards()
    if a.list:
        for s, e in sorted(b.items()):
            print(f"{s}  {e['product_string']}")
        print("in DFU already:", ", ".join(sorted(dfu_paths())) or "(none)")
        sys.exit(0)

    if not b:
        sys.exit("no running Giris found")
    if a.serial:
        hits = [s for s in b if a.serial.upper() in s.upper()]
        if len(hits) != 1:
            sys.exit("%r matches %d boards; attached: %s"
                     % (a.serial, len(hits), ", ".join(sorted(b))))
        want = hits[0]
    elif len(b) > 1:
        sys.exit("%d boards attached — pass --serial with one of: %s"
                 % (len(b), ", ".join(sorted(b))))
    else:
        want = next(iter(b))

    before = dfu_paths()
    d = hid.device()
    d.open_path(b[want]["path"])
    buf = bytearray(64)
    buf[0], buf[1] = CMD_BOOTLOADER, 1
    d.write(bytes([0]) + bytes(buf))
    print(f"asked {want} to reboot into DFU")
    try:
        d.close()
    except OSError:
        pass                                    # it is already gone

    for _ in range(60):
        time.sleep(0.25)
        new = dfu_paths() - before
        if new:
            path = sorted(new)[0]
            print(f"in DFU at path {path}")
            print(f"  tools/flash.sh build/giris.bin {path}")
            sys.exit(0)
    sys.exit("board never reappeared in DFU — is its J2 plugged in?")
