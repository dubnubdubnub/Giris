# osu pad — master/slave power & interconnect architecture

> **Implementation status (2026-07-05):** items 1–9 are DONE in the schematic — U4 CE→VOUT; U5 EN on
> PC13 (net `/PW_PSTRH`, R27 10 k pulldown); J2 VBUS split to `/VBUS_HOST` via U29 (ST tied to GND,
> C56 10 µF); J4 VBUS disconnected (data-only) with R4/R5 CC Rd populated; J1 CC Rd (R28/R29 5.1 k);
> `/VBUS_B` divider R30/R31 10 k/10 k + C57 47 nF + R32 330 Ω into mux **S4A** (VBUS reads on SEL=0);
> R3 pull-up on +3.3V; bulk caps moved behind the switches (C10 22 µF stays at J1); R8 BOOT0 now 10 k.
> Net renames: `/AP_FAULT` (U5 FAULT#→PB10), `/LM_ST` (U4 ST→PB12).
> **Still open:** the inter-board data link rework (U1 delete → half-duplex UART, below) + J1 data ESD;
> optional: move U5.IN to `/VBUS_HOST` (saves one diode drop to the slave); LED level-shift decision.

Design intent: the same PCB is both halves of a split system. **Master** = host on the main
USB-C (J2), sources 5 V out the inter-board USB-C (J1). **Slave** = powered *in* through J1.
Inter-board data = UART on J1 D+/D−.

Verified against the netlist and datasheets (AP22653 DS41186 Rev 5-2 now in `hw/refs/AP22653.pdf`;
LM66100; CA-IS372x). Four independent analyses: part research, contention verification,
clean-sheet architecture, and a failure-mode sweep of the as-drawn wiring.

## Verified part facts (AP22653W6-7, U5)

- **EN is ACTIVE-HIGH** (suffix "3" = active high; AP22652 is the active-low sibling). Non-latching current limit.
- **R6 = 22 kΩ → ILIM ≈ 1.16 A typ (1.04–1.28 A)** per the datasheet best-fit equation
  `ILIM_typ[mA] = 30321 / R[kΩ]^1.055`. **Keep 22 k** — slave worst-case load (~0.5 A: MCU+analog+6 LEDs
  full white) gets >2× headroom.
- Soft-start 0.5 ms typ; FAULT# open-drain with **6 ms deglitch** (debounce ≥10 ms in firmware), self-clearing.
- Reverse current: **blocked only when disabled** (0.01 µA). When *enabled* with VOUT>VIN it
  reverse-*limits* at ~0.32 A — it does not block. When disabled, a ~600 Ω discharge FET loads OUT
  (≈8 mA burned from the link when a slave's U5 is off — acceptable, just know it's there).

## Why the as-drawn wiring fails (failure sweep, quantified)

U4's CE=GND puts the LM66100 in "always-on RPP" mode — a **bidirectional** 79 mΩ FET. With U5 also
hard-enabled, `+5V` and `/VBUS_B` are permanently fused through two parallel FETs:

| Scenario | Consequence |
|---|---|
| Dual source (host on J2 + 5 V arriving on J1) | Uncontrolled loop through U4; ΔV=0.75 V → ~2.7 A > U4 1.5 A abs-max → **U4 dies (often short)** |
| Slave powered via J1 | Slave's J2/J4 receptacle VBUS pins are **hot at 5 V**; host later plugged into slave's J2 back-drives through both U4s to the master's host |
| Any state | U5's current limit and soft-start are **decorative** — U4 bypasses them |
| Hot-plug link cable | 132 µF (C11–C14,C16,C17) sits connector-side, charged: ~20–30 A cap-to-cap dump at contact + recharge surge through the already-on U4 → master +5V sag, possible brownout/USB drop |
| Role detection | Impossible: J2 VBUS is hard-wired to +5V, so "host present" is indistinguishable from "rail is up" |

## How it should be wired

```
J2 VBUS ──/VBUS_HOST──┬─ U29 LM66100 (CE→VOUT: RCB) ─► +5V ─┬─ U8 XC6206 ─► +3.3V
                      │                                     └─ U9 TPS7A4700 ─► +3.3VA
                      └─ U5 AP22653 IN, EN←MCU GPIO ─► /VBUS_B ── J1 VBUS   (source, master only)
J4 VBUS ──/VBUS_AUX──── U30 LM66100 (RCB) ─────────────► +5V               (or make J4 data-only)
J1 VBUS ──/VBUS_B────── U4 LM66100 (re-strap CE→VOUT: RCB) ─► +5V          (sink, slave)
```

### Wiring diff (ordered by leverage)

1. **U4 pin 3 (~CE): GND → VOUT (+5V).** One trace. Turns U4 into a true one-way ideal diode
   (LM66100 §8.3.2 "Always-ON Reverse Current Blocking"). Single most load-bearing fix — kills the
   dual-source destruction loop, stops slave receptacle backfeed through U4, and makes U5's
   limit/soft-start real.
2. **U5 pin 3 (EN): +5V → MCU GPIO + 100 k pull-down.** Default = not sourcing. Firmware asserts
   only when master (host seen on J2) AND /VBUS_B not already high. Cold J1 on unprogrammed boards
   (provision each board over its own J2 first).
3. **Split J2 VBUS off the +5V rail**: new net `/VBUS_HOST`, new LM66100 (RCB) into +5V. Gives
   (a) cold receptacles on a link-powered slave, (b) a real "host present" signal for role detection,
   (c) feed U5.IN from /VBUS_HOST so the slave path has one less diode drop.
4. **J4**: same treatment with a third LM66100, or simplest — disconnect J4 VBUS entirely (data-only
   debug port; it enumerates fine with power from the rail? No — a device port needs no VBUS supply
   *out*, and the board is powered anyway; keep a VBUS-detect divider if firmware should know).
5. **VBUS telemetry with zero new ADC pins**: mux channel 4 (S4A/S4B) currently just goes to header
   J7. Feed it 2:1 dividers (2×100 k) from /VBUS_HOST (S4A) and /VBUS_B (S4B) — the existing sensor
   scan then reads both VBUS rails through ADC1_IN0. Only one new GPIO total (U5 EN).
6. **J1 CC1/CC2: add 5.1 k Rd to GND on both** (currently floating). A real host/charger mistakenly
   plugged into J1 will then legitimately grant 5 V and the board boots as slave — safe by design.
   Master sourcing VBUS without Rp detection is non-compliant but bounded (firmware-gated, 1.16 A
   limited, RCB everywhere); acceptable for a proprietary link.
7. **R3 (U4.ST pull-up): +5V → +3.3V** so PB12 sees 3.3 V logic.
8. **R6 stays 22 k** (≈1.16 A — already right; earlier assumption of ~0.5 A was wrong).
9. Bulk caps: 132 µF on /VBUS_B is connector-side → hot-plug cap-dump and >10 µF Type-C sink-cap
   guideline. Consider moving most of the bank behind U4/U5 (keep ~10–22 µF at the connector).
10. Firmware: debounce FAULT#/ST ≥10 ms; role arbitration = VBUS_HOST high → master, U4.ST high →
    slave; never assert U5 EN if /VBUS_B already high; cap total LED current when the host budget
    matters (master+slave ≈ up to ~1 A from one USB2 port that defaulted to 500 mA — enumerate and
    request high power, or dim).

### Droop budget (both halves linked, verified OK)

Host 4.75 V worst − U5 (65 mΩ) − cable (~0.25 V @ 0.5 A) − slave U4 (~60 mV) → slave +5V ≈ **4.4 V
worst-case**. XC6206 and TPS7A4700 both stay in regulation with margin; SK6812 (3.7–5.5 V) fine and
its 0.7·VDD VIH actually *improves* at lower VDD. No changes needed.

## ⚠ The inter-board data link cannot work as drawn (independent of power)

Confirmed by adversarial verification against the CA-IS372x datasheet and the netlist:

- U1 pin 2 (VO1) is a **fixed silicon output** driving J1 D+; pin 3 (VI2) a fixed input on D−.
- A USB-C cable connects D+↔D+, D−↔D− (straight; receptacle A6/B6 ties absorb flip).
- Two identical boards ⇒ **push-pull output vs push-pull output on D+** (CA-IS3722CHS is fail-safe
  HIGH, so an idle board actively drives high — every start bit is output-stage shoot-through), and
  **two floating inputs on D−**. Zero working signal paths, either direction, any orientation.
- The R13–R16 crossbar is MCU-side only and cannot fix cable-side direction. No population option,
  firmware TX/RX-swap, or orientation rescues it.
- Also: U1 VDDA/VDDB are unpowered (only decoupling caps on their nets), and GNDA=GNDB=GND so the
  isolator provides no isolation anyway.

### Fix (recommended): half-duplex single-wire UART, delete U1

- Remove U1, C1, C4. Route J1 D+ (A6/B6) → 22–33 Ω series → **PC6**; one 4.7–10 k pull-up to +3.3V
  per board. AT32F405 USART6/UART7 support single-wire half-duplex natively on PC6/PC7.
- Route J1 D− → series R → **PC7** as a second, symmetric spare line (presence/handshake or a second
  half-duplex channel).
- Add an SRV05-4A on J1's data lines (J1 is currently the only user-facing connector with no ESD).
- Perfectly symmetric: identical PCBs, any cable, any orientation. Protocol: master polls, slave
  responds; half-duplex echo gives free collision detection.

Alternatives, ranked: I²C on D+/D− (symmetric but bus-lockup risk over a cable, and PC6/PC7's I2C1
alt-function is already used on PF4/PF5); two assembly SKUs (CA-IS3721 on one half — same footprint,
mirrored directions — plus crossbar swap; breaks "any two boards pair"); SBU rerouting (rejected —
SBU crosses in full-featured cables and flips with orientation: a 50% lottery, and USB2-only cables
omit SBU entirely).

## New parts summary

| Ref | Part | Purpose |
|---|---|---|
| U29 (, U30) | LM66100DCK (already on BOM as U4) | RCB ORing diode for J2 (and J4) VBUS |
| R27, R28 | 5.1 k 0402 | J1 CC Rd |
| R29–R32 | 100 k 0402 | VBUS sense dividers → mux ch4 |
| R33 | 100 k 0402 | U5 EN pull-down |
| C52 (, C53) | 10 µF | /VBUS_HOST (, /VBUS_AUX) input caps |
| U31 | SRV05-4A | ESD on J1 D+/D− |
| — | delete U1, C1, C4; add 2× 4.7–10 k pull-up + 2× 22–33 Ω series | half-duplex link |
