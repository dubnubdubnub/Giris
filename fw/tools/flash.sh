#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
# Flash over the factory ROM DFU.
#
# The ROM bootloader is DfuSe on VID:PID 2e3c:df11 and lives on OTGFS (PA11/PA12)
# ONLY — that is connector J2 on this board, never J3. J2 has no VBUS net, so the
# board must be powered separately while you flash.
#
# To enter DFU: hold SW1 (BOOT0) and tap SW2 (NRST), then release SW1.
# Device layout, straight off the descriptor:
#   alt 0  @Internal Flash  /0x08000000/128*001Kg
#   alt 1  @Option byte     /0x1FFFF800/01*512g
#
# Every AT32 reports the serial "AT32" in DFU, so with two boards attached the
# only thing that tells them apart is the bus path. Pass it as the second
# argument; `dfu-util -l` prints it as path="0-1.3.2".
set -euo pipefail

BIN="${1:-build/giris.bin}"
DFU_PATH="${2:-}"
[ -f "$BIN" ] || { echo "no such file: $BIN" >&2; exit 1; }

# bash 3.2 on macOS has no mapfile, and the paths never contain whitespace.
PATHS=$(dfu-util -l 2>/dev/null | sed -n 's/.*2e3c:df11.*path="\([^"]*\)".*/\1/p' | sort -u)
NPATHS=$(printf '%s\n' $PATHS | grep -c . || true)

if [ "$NPATHS" -eq 0 ]; then
  echo "No AT32 DFU device found." >&2
  echo "Power the board, then hold SW1 (BOOT0) + tap SW2 (NRST), release SW1." >&2
  echo "Or, if the firmware is alive: tools/.venv/bin/python tools/dfu.py --serial <uid>" >&2
  exit 1
fi

FIRST=$(printf '%s\n' $PATHS | head -1)

if [ -z "$DFU_PATH" ] && [ "$NPATHS" -gt 1 ]; then
  echo "$NPATHS AT32 boards are in DFU: $(printf '%s ' $PATHS)" >&2
  echo "Pass one as the second argument: tools/flash.sh $BIN $FIRST" >&2
  exit 1
fi

TARGET="${DFU_PATH:-$FIRST}"
echo "flashing $BIN ($(wc -c < "$BIN") bytes) to 0x08000000 on path $TARGET"
dfu-util -a 0 -d 2e3c:df11 -p "$TARGET" -s 0x08000000:leave -D "$BIN"
