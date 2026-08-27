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

Phase 1 of the bring-up plan, and nothing more:

- clocks to **216 MHz** from the 12 MHz crystal (PLL MS=1, NS=72, FP /4; PLLU /18 for OTG_FS),
- drives the 7-pixel SK6812 chain on **PB9** with a slow dim rainbow — pixel 0 is the sacrificial
  level-shifter and is kept dark,
- pulses **PD2** (`/IO1`, J6 pin 17) high for 100 µs every ~20 ms as a scope reference.

**Proof of life:** the six key LEDs cycle colour. **Scope check:** the PD2 pulse should measure 100 µs.
If it measures ~2.2× that, the PLL did not engage and the core is running off HICK.

If nothing happens, BOOT0 + NRST always gets you back to DFU — the ROM bootloader runs before user code,
so you cannot brick the board this way.

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
- Everything under `src/`, `include/`, `ld/`, `cmake/`, `tools/` is ours.
