# Giris firmware

Bare-metal C for the **AT32F405RBT7-7** on `hw/TMR2615F_osu_pad` — the 6-key analog dev pad that
prototypes the 64-key split (32 keys per half).

Architecture, decisions and the bring-up plan live in
[`../hw/TMR2615F_osu_pad/firmware_architecture.md`](../hw/TMR2615F_osu_pad/firmware_architecture.md).
This README is just how to build and flash.

## Setup

```bash
brew install --cask gcc-arm-embedded   # or the Arm GNU Toolchain installer
brew install cmake ninja dfu-util
./tools/fetch-deps.sh                  # Artery BSP + TinyUSB into vendor/
```

`vendor/` is gitignored. Pinning ~500 MB of upstream firmware libraries into this repo (or into
`.gitmodules`, alongside the KiCad libraries) is a decision to make deliberately — `tools/fetch-deps.sh`
has the one-liners if you want them as submodules.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Produces `build/giris.{elf,bin,hex}` and a `giris.map`. The link step prints the memory budget, which is
worth watching as the firmware grows:

```
FLASH:  3868 B / 128 KB   2.95%
RAM:    1600 B /  70 KB   2.23%
```

`compile_commands.json` is exported for clangd.

## Flash

```bash
cmake --build build --target flash      # or: ./tools/flash.sh build/giris.bin
```

**Entering DFU:** hold **SW1 (BOOT0)** and tap **SW2 (NRST)**, then release SW1. The board appears as
`2e3c:df11`, DfuSe 1.1a.

Two things about this that are easy to get wrong:

- The ROM bootloader is on **OTG_FS (PA11/PA12) only — connector J2, never J3.** J3 is the USB HS host
  port and cannot reach the factory bootloader at all.
- **J2 has no VBUS net**, so it cannot power the board. Power it separately while flashing.

`dfu-util` reports `Error during download get_status` on the leave request. That is a DfuSe quirk — the
device resets before answering. `File downloaded successfully` on the line above is the one that matters.

The device advertises its own memory layout, which is how the flash geometry here was confirmed:

```
alt 0   @Internal Flash   /0x08000000/128*001Kg      128 x 1 KB sectors
alt 1   @Option byte      /0x1FFFF800/01*512g
```

Note this is the **xB** part: 1 KB sectors, 6.6 ms typical / 8 ms max erase. The 2 KB / 13.2–16 ms
figures in Artery's tables are the 256 KB xC part. (Artery's own `AT32F405xB_FLASH.ld` declares 256 KB
of flash — a BSP bug. `ld/at32f405rbt7.ld` is that file with the length corrected to 128 KB.)

## What the firmware does today

- clocks to **216 MHz** from the 12 MHz crystal (PLL MS=1, NS=72, FP /4; PLLU /18 for OTG_FS),
- runs the **ADC/mux scan engine at 8 kHz** — ADC1 converts PA3/PA2/PA1/PA0 on every TMR2 TRGO,
  DMA1_CH1 lands them in a circular 8-slot ring, and the half/full-transfer interrupts flip the
  TMUX1574 SEL line so bank A and bank B interleave,
- enumerates on **J3 as a USB high-speed raw-HID device** and streams the readings to the browser
  viewer,
- drives the SK6812 chain as a status indicator.

There are **two HID interfaces**: the vendor raw-HID one the viewer speaks (usage page `0xFF60`), and
a boot-protocol keyboard. They are separate interfaces on purpose — macOS evaluates Input Monitoring
per `IOHIDDevice`, which means per USB interface, so putting keyboard usages on the vendor interface
would make the viewer demand permission to open it. Keyboard output is gated off at boot; see
`CMD_KEYS`.

### The viewer

```bash
./tools/viewer/serve.sh          # http://localhost:8000, Chrome or Edge
```

WebHID needs a secure context and `http://localhost` qualifies, so there is no TLS and no build step.
Live traces for all six keys, per-key readouts in counts and Gauss, CSV recording, a raw-slot debug
table, and a burst capture that dumps a few thousand samples for an in-page FFT.

That FFT is the point of the whole exercise: **a flat spectrum means white noise and narrowing the
analog filter is worth it; a −1/2 amplitude slope means 1/f and narrowing buys ~5 % for a 103 µs
transport delay.** That measurement decides the filter respin.

### Two boards at once

Every board now takes its USB serial from the 96-bit factory UID at `0x1FFFF7E8`, so two halves
running one image are separable everywhere — `capture.py`, `link.py`, `dfu.py` and the viewer all take
`--serial` with any unique substring. The last four hex characters are a good handle.

```bash
tools/.venv/bin/python tools/link.py --list
E75C30400080160505875C13  Giris osu pad (telemetry)
E85C30400080160505875113  Giris osu pad (telemetry)
```

Those two are adjacent dice off one wafer and differ in only two of twelve bytes, which is why
`uid_tag()` — the 16-bit condensation used to break ties in link arbitration — is FNV-1a over the whole
UID rather than a truncation. They come out 0x436A and 0xF885.

In DFU every AT32 reports the serial `AT32`, so there the bus path is the only discriminator:

```bash
tools/.venv/bin/python tools/dfu.py --serial 5C13     # prints the DFU path it lands on
./tools/flash.sh build/giris.bin 2-1.3
```

### The J1 link

```bash
tools/.venv/bin/python tools/link.py --probe --serial 5C13   # one board, J1 empty
tools/.venv/bin/python tools/link.py --continuity            # two boards: is there a wire?
tools/.venv/bin/python tools/link.py --peer                  # two boards: the real sweep
tools/.venv/bin/python tools/link.py --soak --baud 13500000
```

**Measured, two boards, straight-through USB-C on J1:**

| Config | Result |
|---|---|
| Half duplex, open drain, one wire on D− — *the discovery bus* | clean at 115200 and **500 kbaud**; framing errors at 1 Mbaud |
| Full duplex, push-pull, TRPSWAP on one half — *the run phase* | clean at every rung to **13.5 Mbaud**, the USART's ceiling, both directions |
| Soak at 9 and 12 Mbaud, **3 m cable** | 409,600 bytes each, **0 corrupt, 0 missing, 0 noise flags** |
| Same at 13.5 Mbaud, 3 m | NERR on **61/100 runs**; clean at 1.5 m |
| Sustained rate, DMA both ends | **98.8–99.6 % of line rate** — the wire is saturated, not the CPU |

At 13.5 Mbaud a 72-byte frame (32 keys × u16 + header + mech bitmap + CRC-16) takes **54 µs, 43 % of a
125 µs microframe**, and the link is full duplex so the return path is free. 13.5 Mbaud is also the
hard ceiling: RM 12.6.1 requires `DIV ≥ 16` with fixed 16× oversampling, and APB2 is already at the
part's 216 MHz maximum. **Run at 12 Mbaud**: it is clean over a 3 m cable with the receiver's noise
flag never firing, where 13.5 raises NERR on most runs. See the architecture doc for the full table
and for why the link should negotiate its own rate.

500 kbaud is exactly where the architecture doc predicted the open-drain bus
would die: two 10 k pull-ups into ~70 pF. Discovery at 115200 has 4× margin.

Three traps are baked into the tooling because each one costs an afternoon:

- **`--probe` before anything else.** The datasheet gives PC6 = USART6_TX and
  PC7 = USART6_RX but not the MUX index. The probe sweeps all sixteen while the
  USART shifts `0x00` and watches the pad; only **MUX8** modulates.
- **`--continuity` before believing any link failure.** It is a DC test with no
  USART involved: one board holds a pad low, the other reads its own. A
  charge-only USB-C cable carries VBUS and GND but neither D+ nor D−, and from
  the firmware side that is indistinguishable from a dead peripheral. The first
  cable tried here was one of those, and it also fed 5 V into J1 — which is what
  made one board's `/LM_ST` read high and sent us hunting a phantom.
- **Receive by DMA, never by polling.** A CPU loop gets preempted by the 16 kHz
  ADC/mux ISR for longer than a byte time above ~8 Mbaud. The USART overruns,
  the lost byte shifts the expected value for every byte after it, and reading
  `DT` silently clears `ROERR` — so you see *almost every byte arriving and
  almost every byte wrong, with no error flag*, which looks precisely like a bad
  cable. It is not. `DMA1_CHANNEL1` is the ADC ring, so the link takes channel 2.

**This silicon does not echo its own half-duplex transmission.** RM 12.2 says TX
and SW_RX are interconnected, but with `SLBEN=1 TEN=1 REN=1` a lone board never
raises RDBF, at any baud, open-drain or push-pull, with no error flag — and
turning the listener's transmitter off does not change it. Testing the receive
path needs a peer on J1 or a jumper from **TP1** (D+) to **TP2** (D−).

### Is it really 8 kHz?

```bash
tools/.venv/bin/python tools/ratetest.py --seconds 10
```

Measured, not asserted. The streaming path emits exactly **one frame per report** — `adc_read_frame()`
always returns the newest complete frame, so the pack loop breaks on its second iteration — which means
the device wants to send at the full scan rate and achieves `min(poll rate, scan rate)`. Counting
reports therefore counts polls. `bInterval = 1` is 1 ms at full speed and 125 µs at high speed, so
~8000/s proves both at once.

Two independent numbers come out and should agree: the host-observed count, which Python can bottleneck,
and a device-side figure computed from the firmware's own `tx_dropped` counter, which cannot.

| Host | scan | host-observed | device-side | dropped | seq gaps |
|---|---|---|---|---|---|
| macOS 26, 30 s | 8000 frames/s | 7902 reports/s | 8000 reports/s | 0 | 0 |
| **Windows 11 24H2, 5 s** | 8001 frames/s | **7883 reports/s** | 8001 reports/s | 0 | 0 |

Windows matters here because it honours the `bInterval` exponent only for intervals 1–5, and it is the
platform where 8 kHz claims usually fall apart. It does not: within 0.2 % of macOS.

**Caveat:** a 30 s Windows run ended in an `OSError: read error` from hidapi and the hub then dropped
off entirely, taking its other devices with it. That looks like cable movement rather than a device
fault, but a long Windows soak has not yet completed cleanly and should be repeated.

### Checking the USB side

```bash
python3 tools/usb-check.py
```

Reads descriptors only — no interface claim — so it works while the browser has the device open. It
should report high speed (480 Mb/s) and `bInterval=1` on the interrupt endpoints.

### Reflashing without touching the board

The viewer's **DFU** button issues `CMD_BOOTLOADER`, which jumps to the ROM bootloader at
`0x1FFFA400`. The device disappears from J3 and reappears as `2e3c:df11` on J2, ready for
`tools/flash.sh`. BOOT0 + NRST remains the recovery path if the firmware ever stops answering — the
ROM bootloader runs before user code, so you cannot brick the board this way.

## Deliberately not done yet

`main()` **never drives PC13**. That pin is the AP22653 enable and it sources 5 V out of J1 into whatever
is plugged in there. It is Hi-Z at reset with a 10 k pulldown holding the switch off, and it must stay
that way until link arbitration has run and the VBUS_B divider (mux S4A → ADC1_IN0) reads cold.

Likewise the SK6812 driver is a blocking bit-bang with interrupts disabled — fine while nothing else
runs, and exactly what the architecture doc says not to ship. The real one is SPI + DMA at 3.375 MHz.

## Layout

```
cmake/arm-none-eabi.cmake   toolchain file
ld/at32f405rbt7.ld          BSP linker script with the flash length corrected
include/board.h             pin map, read out of the exported netlist (not the schematic source)
include/at32f402_405_conf.h BSP module switches
src/clock.c                 12 MHz -> 216 MHz
src/sk6812.c                bit-banged LED chain (bring-up only)
src/main.c                  first light
tools/fetch-deps.sh         vendor deps
tools/flash.sh              dfu-util wrapper
```

`include/board.h` is the single source of truth for pin assignments and carries the traps inline — the
inverted UART net names, the PB12/OTGHS_ID overlap, the hotswap sockets sitting on the analog nodes, and
the sensor's inverted output.

## Licences

- **TinyUSB** — MIT.
- **Artery BSP** — *not* a free licence. Artery grants use, copying and distribution "for the purpose of
  design and development in conjunction with Artery microcontrollers". Fine for this product; it does not
  travel to a non-Artery target, and it is not BSD-3.
- Everything under `src/`, `include/`, `ld/`, `cmake/`, `tools/` is ours **except three
  Artery-derived files**: `src/startup_at32f402_405.s`, `ld/at32f405rbt7.ld` (modified) and
  `include/at32f402_405_conf.h`. Those keep Artery's notice and are not covered by Giris'
  Apache-2.0 grant. See `THIRD_PARTY.md`.
