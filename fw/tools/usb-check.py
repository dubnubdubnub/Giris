#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""Verify the Giris device enumerated at USB high speed with the endpoint
interval we asked for. Reads descriptors only — no interface claim, so it works
even while macOS has the HID device open."""
import ctypes, ctypes.util, sys
import usb.core, usb.util

VID, PID = 0x1209, 0x6415
SPEED = {0: "unknown", 1: "low (1.5 Mb/s)", 2: "full (12 Mb/s)",
         3: "HIGH (480 Mb/s)", 4: "super (5 Gb/s)", 5: "super+ (10 Gb/s)"}

def speed_of(dev):
    """libusb's speed query is unreliable through the macOS backend. IOKit
    publishes the negotiated link rate directly, which is unambiguous."""
    import subprocess, re
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-w0", "-l"],
                             capture_output=True, text=True, timeout=10).stdout
        i = out.find(dev.product)
        if i < 0:
            return 0
        m = re.search(r'"UsbLinkSpeed" = (\d+)', out[i:i + 3000])
        if m:
            return {1500000: 1, 12000000: 2, 480000000: 3,
                    5000000000: 4, 10000000000: 5}.get(int(m.group(1)), 0)
    except Exception:
        pass
    return 0


d = usb.core.find(idVendor=VID, idProduct=PID)
if d is None:
    sys.exit(f"no device {VID:04x}:{PID:04x} — is it enumerated on J3?")

print(f"{d.manufacturer} / {d.product}  serial={d.serial_number}")
print(f"bcdUSB   0x{d.bcdUSB:04x}   speed: {SPEED.get(speed_of(d), '?')}")

ok = True
for cfg in d:
    for itf in cfg:
        print(f"\ninterface {itf.bInterfaceNumber}  class 0x{itf.bInterfaceClass:02x}"
              f" sub 0x{itf.bInterfaceSubClass:02x} proto 0x{itf.bInterfaceProtocol:02x}")
        for ep in itf:
            direction = "IN " if ep.bEndpointAddress & 0x80 else "OUT"
            xfer = ep.bmAttributes & 3
            kind = ["control", "iso", "bulk", "interrupt"][xfer]
            note = ""
            if xfer == 3:
                # High speed: period = 2^(bInterval-1) microframes of 125 us
                uframes = 2 ** (ep.bInterval - 1)
                hz = 8000 / uframes
                note = f"  -> {uframes} microframe(s) = {hz:.0f} Hz"
                if ep.bInterval != 1:
                    ok = False
            print(f"  ep 0x{ep.bEndpointAddress:02x} {direction} {kind:9s}"
                  f" wMaxPacketSize={ep.wMaxPacketSize:3d} bInterval={ep.bInterval}{note}")

print("\n" + ("PASS: high speed, bInterval=1 on every interrupt endpoint"
              if ok and speed_of(d) == 3 else
              "CHECK: not high speed and/or an interrupt endpoint is not at bInterval=1"))
