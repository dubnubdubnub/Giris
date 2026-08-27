#!/usr/bin/env bash
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
set -euo pipefail

BIN="${1:-build/giris.bin}"
[ -f "$BIN" ] || { echo "no such file: $BIN" >&2; exit 1; }

if ! dfu-util -l 2>/dev/null | grep -q '2e3c:df11'; then
  echo "No AT32 DFU device found." >&2
  echo "Power the board, then hold SW1 (BOOT0) + tap SW2 (NRST), release SW1." >&2
  exit 1
fi

echo "flashing $BIN ($(wc -c < "$BIN") bytes) to 0x08000000"
dfu-util -a 0 -d 2e3c:df11 -s 0x08000000:leave -D "$BIN"
