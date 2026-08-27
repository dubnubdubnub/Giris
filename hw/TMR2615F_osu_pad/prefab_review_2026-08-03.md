# TMR2615F osu pad — pre-fab design review (2026-08-03)

**Verdict: DO NOT FAB YET.** One board-killing schematic error (U8 regulator pinout), one
functional-block error (I2C1 pins swapped), and one signal-integrity risk on the main USB HS port.
Everything else — power architecture, sensor chain, USB-C, LED chain, fab outputs — verified clean.

Scope: full schematic + PCB + gerber/BOM/CPL analysis (kicad-happy analyzers, run
`analysis/2026-08-03_0406/`), plus manual datasheet verification of every IC against the PDFs in
`hw/refs/` (5 parallel verification passes, page-level citations). DRC report reviewed. Board:
70.1 × 60.0 mm, 4-layer (In1/In2 = solid GND), 167 footprints, routing complete, 0 unconnected pads.

---

## Blockers (fix before ordering)

### 1. CRITICAL — U8 (XC6206P332MR) VIN/VOUT swapped → +3.3V rail becomes ~4.4 V, kills the MCU

Torex datasheet (ETR0305_003a, p.2 "PIN ASSIGNMENT" + top view): SOT-23 **pin 1 = VSS,
pin 2 = VOUT, pin 3 = VIN** (pin 3 is the lone pin). The custom symbol
`Regulator_Linear__C:XC6206P332MR` has it backwards — the netlist puts **+5V on pad 2 (real VOUT)**
and **+3.3V on pad 3 (real VIN)**. Verified in the raw PCB: pad 3 is the lone pad and carries +3.3V.

Consequence: the pass-PFET's parasitic diode (block diagram, p.3) conducts VOUT→VIN, so the "+3.3V"
net sits at ≈ 5 V − 0.6 V ≈ **4.4 V unregulated**. AT32F405 abs-max VDD−VSS is **4.0 V**
(Table 10, p.37) — the MCU and all +3.3V logic get overstressed the moment VBUS arrives.
The fab BOM orders the real part (LCSC C5446), so this *will* happen as built.

Fix options:
- Correct the symbol pin numbers (1=VSS, 2=VOUT, 3=VIN) and re-dress the copper at U8
  (both rails are F.Cu pours right there — small edit), **or**
- Keep the layout and BOM-swap to a regulator whose SOT-23 pinout matches what's drawn
  (1=GND, 2=VIN, 3=VOUT), e.g. MCP1700T-3302E/TT (250 mA). Check thermal note #6 either way.

### 2. CRITICAL (functional) — I2C1 SDA/SCL swapped on PF4/PF5

AT32F405 datasheet Table 9, p.31 (LQFP-64 pins 18/19): **PF4 = I2C1_SDA, PF5 = I2C1_SCL**.
Schematic has PF4→I2C1_SCL, PF5→I2C1_SDA. I2C pins can't be remapped internally, so hardware I2C1
(the J4 SH1.0 accessory port, via R16/R21 + U9 ESD) is dead as wired — bit-bang would be the only
fallback. Fix: swap the two nets at the MCU (two wires), or move I2C1 to PB6/PB7
(also I2C1_SCL/SDA per Table 9 p.35, currently header IOs IO3/IO4).

### 3. HIGH — SRV05-4A (U6) doubled channels load the USB **HS** pair with ~6–10 pF/line

SRV05-4A junction capacitance is 3 pF typ / 5 pF max per I/O (Semtech EC table). U6 parallels two
channels per line (IO1+IO4 on D+, IO2+IO3 on D−) → ~6 pF typ per line lumped onto the 480 Mbps
pair feeding J3, the main host port. Practical HS budget is ~1–2 pF; this risks eye-diagram
failure/marginal HS link — and HS (125 µs microframes) is presumably the point of this port.
Recommendation: replace U6 with a sub-1 pF HS-rated part (RClamp0524P, TPD2EUSB30, ESD122,
PESD5V0X1BCSF-class). U5 on the FS pair and U1/U9 on UART/I2C are fine as-is (doubling is harmless
at those speeds). Related: keep C17/C18 (30 pF, DNP) unpopulated forever — or delete the stub pads
from the HS pair.

## Should-fix (cheap, while you're in there)

4. **BOOT0 pull-down R26 = 10 k; datasheet requires ≤ 3.3 k** (Table 9 footnote 7, p.35:
   "externally connects a pull-down resistor (3.3 kΩ or below) to ground"). Change R26 to 2.2–3.3 k.
   Same footnote: after reset the pin becomes PF11 GPIO — never drive it low in firmware while the
   BOOT button could be held (button shorts it to +3.3V with no series R).
5. **VDDA is fed from a different LDO than VDD.** Datasheet §2.4.1 p.14 and Table 15 p.38 require
   VDDA connected to VDD (and Table 10 allows only 50 mV between VDD pins). Here VDD = +3.3V
   (XC6206 ±2%) and VDDA = ferrite-filtered +3.3VA (TPS7A4700 ±1%) — worst-case ~100 mV apart.
   It's low damage-risk in practice, and the ratiometric pairing with the sensors (both on 3.3VA)
   is genuinely good analog design — but it's out of spec on paper. Options: accept (firmware
   per-key calibration absorbs gain error anyway), or move L1's feed to +3.3V (spec-true, small
   ratiometric penalty). Your call; document whichever you keep.
6. **XC6206 thermal margin.** SOT-23 Pd abs-max = 250 mW (p.3). At (5−3.3) V the limit is ~147 mA;
   MCU at 216 MHz with the USB HS PHY active can plausibly reach 100–150 mA. 20 mA of headroom at
   room temp, less in an enclosure. Since U8 must be touched anyway (blocker #1), consider a
   higher-Pd package/part or verify the real 3.3 V load at bring-up.
7. **C8 pin 2 is floating in the schematic** (single-pin net `__unnamed_2`); on the PCB the pad is
   correctly on VBUS_HOST. The gerbers are right, but the next "Update PCB from schematic" will rip
   that connection out. Add the missing wire stub.
8. **UART7 net names are reversed vs the pin functions**: Table 9 p.33 has PC6 = UART7_TX,
   PC7 = UART7_RX; the nets are named the other way. Electrically fine for the half-duplex
   inter-board link (UART7 supports single-wire half-duplex + TX/RX swap, Table 8 p.22 — plan on
   UART7 for one wire, USART6 for the other). Rename to avoid firmware confusion.

## Minor / notes

- **TMUX S4B (pin 11) floats when SEL=1** (goes only to DNP header J5.10). TI recommends grounding
  unused S inputs; harmless if firmware ignores that slot. VBUS_HOST never got a sense divider
  (only VBUS_B on S4A) — role detection must come from USB enumeration, which works.
- **PC14/PC15 (IO6/IO7 on J6 header) are TC-type — NOT 5 V-tolerant** (abs-max 4.0 V), and
  PC13–PC15 are 3 mA-limited outputs. Fine for PW_PSTRH (0.33 mA into the AP22653 EN pulldown).
- **First-LED level shift (D1 trick) verified working** but data-dependent: at dim first-pixel and
  VBUS = 5.25 V, VIH ≈ 3.25 V vs 3.3 V drive — near-zero margin. If pixel 1 ever flickers, load
  D1's cathode with ~1 k to GND.
- **22 TestPoint rows sit in both fab BOMs with no part number** — JLC/NextPCB will flag them;
  ignore or strip the rows.
- **Bottom silk art extends ~5 mm past the board outline** (B.SilkS y to −155.9 vs edge −150.95);
  fab will clip it — confirm the logo still looks right clipped.
- **DRC leftovers** (reviewed): 30 copper-edge-clearance, 28 hole-clearance, 24 malformed-courtyard,
  8 starved-thermal (1-spoke pads: C19, C47, U3.2, R12, SW2…), 7 courtyard-overlap. Nothing
  connectivity-related; starved thermals are a solder-quality nit only.
- **Sensor footprint has no pin-1 silkscreen marker** (custom DFN3L). Pad pattern is mechanically
  keyed so it can't be misplaced, but AOI has no reference. Cosmetic.
- Stale/wrong reference copy `hw/refs/tmr2615x.kicad_sym` (wrong pin electrical types) — the board
  uses the correct library copy; delete or fix the refs copy. `hw/refs/ENFA0018.pdf` is a Murata
  BLM15 bead spec, not the board's Sunlord GZ1608D601TF — housekeeping only.

---

## Verified clean (datasheet-backed, citations in agent transcripts)

- **Power architecture** implements `power_architecture.md` items 1–9 faithfully:
  U7/U2 LM66100 with ~CE→VOUT = datasheet §8.3.2 "Always-ON RCB" one-way diode (U7.ST→GND is
  explicitly sanctioned); U3 AP22653 EN **active-high** (10 k pulldown = default off), ILIM 22 k →
  1.04–1.28 A, FAULT# open-drain, reverse-blocking when disabled (0.01 µA spec at VIN=0/VOUT=5.5 V);
  back-powered slave leaves J2/J3 receptacles cold. Droop budget honored (TPS7A4700 regulates from
  4.4 V worst case with ~1 V headroom).
- **TPS7A4700**: all 21 pins match RGW pinout; ANY-OUT grounding (1P6+0P2+0P1) = exactly 3.3 V;
  NC-to-GND sanctioned; NR 1 µF = recommended; 47 µF out = recommended value; pad→GND required ✓.
- **TMUX1574**: all 16 pins match UQFN table; EN active-low tied to GND = enabled; SEL=0→SxA;
  internal 6 MΩ pulldowns make floating SEL during MCU reset safe (analyzer's PU-001 overruled);
  Ron/leakage negligible vs 5.1 k source.
- **TMR2615F-AAC-1.500-500 ×6**: pinout (1=VOUT, 2=GND, 3=VDD, bar=GND) and the custom DFN3L
  footprint dimensions match the package drawing; ratiometric output paired with VDDA on the same
  +3.3VA rail ✓; ±30 mT electrical range covers the modeled 3.4–28.6 mT swing; **in-plane sensing
  axis correctly handled** — all six sensors at identical 3.09 mm offsets from switch centers,
  reading the radial field per `magnetics/README.md`; 100 nF per sensor at 1.9 mm ✓.
  Hotswap socket contacts wired across SIG–GND double as a mechanical-switch fallback ✓.
- **AT32F405**: all 64 pins match Table 9 (VDD/VSS/VDDA, NRST, BOOT0, OTGHS1_R/D∓ on 33–35,
  PA11/12 = OTGFS1, ADC1_IN0–15 mapping, SPI3/USART/SWD/PB9-LED AFs); OTGHS1_R = 12 k 1 % to GND
  exactly as required; PB2/PB10/PB12/PC13/PC6/PC7 all 5 V-tolerant; decoupling per Figure 9;
  NRST RC per guideline §1.4.4.
- **Crystal**: X322512MSB4SI is a passive 12 MHz / CL=20 pF crystal; **C31/C32 30 pF are populated**
  (in fab BOM, LCSC C1570) and correct for CL=20 pF with 2–7 pF stray (guideline §2.1). 12 MHz HEXT
  is mandatory for USB HS (§2.13.6) — satisfied. (The analyzer's BOM grouping made these look DNP;
  they are not.)
- **SK6812MINI-E chain**: symbol pin numbering differs from the datasheet but the footprint matches
  the bottom-view pad map of this reverse-mount part — geometrically correct, don't "fix" it.
  Chain order U12→U14→U16→U18→U20→U22→U24, 100 nF each, 120 Ω series at input, D1 orientation
  correct, ~37 mA through the 150 mA diode.
- **USB-C**: 5.1 k Rd on CC1+CC2 of all three ports (analyzer's "missing Rd" = false positive);
  J2 VBUS intentionally unconnected (data-only debug port — note: with Rd fitted it still
  enumerates via C-to-C cables); J1 link port per the architecture doc.
- **Layout**: HS pair skew 0.25 mm, FS pair 0.28 mm, both single-layer F.Cu, no vias; diff-pair
  geometry (0.2/0.13 over 0.21 mm to In1 GND) ≈ 90 Ω recipe; In1/In2 solid GND; 4 thermal vias
  under U4's pad; paste present on all hotswap sockets/ICs; single-sided assembly (back = TPs +
  SWD header only); fiducials ×3; BOM↔CPL↔schematic fully consistent, DNP correctly excluded,
  every real part has MPN + LCSC code.

## Analyzer false positives (triaged, ignore)

SS-001 "MPN coverage <50%" (MPNs live in Value + fab BOMs complete) · VM-001 5V/3.3V crossings on
USB/LM_ST (SRV05 VP & FT-tolerant pins) · PP-001 U12 VDD no DC path (deliberate diode level-shift)
· CC-Rd "fail" on all ports (Rd present) · PU-001 TMUX SEL pull (internal 6 MΩ) · **all
position-based PCB findings** — courtyard overlaps (identical bogus 65.709 mm²), edge distances
(−93 mm), KO-001 vias in "auto-placement-area", DC-001/DC-003 decoupling distances, TV-001 U4
thermal vias, plane-split island counts — the fork-format 20260623 file breaks the analyzer's
coordinate parsing (all footprints read (0,0)); manual checks via CPL/gerbers substituted ·
EMC SU-001 "adjacent signal layers" (In1/In2 are GND) · GR-002 layer-extent variance (intentional
symmetric 5.1 mm copper-free side margins) · GR-004 paste ratio (verified present).

## Not performed / limits

- **SPICE**: ran, found 0 simulatable subcircuits (board is digital + sensor buffers; the R4/R9
  VBUS divider wasn't pattern-matched). Nothing meaningful lost.
- **Thermal analyzer**: assessed 0 components (couldn't model the regulators) — covered manually
  (XC6206 item #6; TPS7A4700 at ~40 mA analog load is trivially fine).
- **Lifecycle audit**: not run; all parts carry current LCSC codes in the fab BOMs.
- **EMC quantitative score**: unreliable (position parsing, above). Qualitative posture is good:
  solid GND planes, per-IC decoupling, ESD on every user-facing connector.
- **ERC**: not re-run via the fork's kicad-cli; the C8 stub (#7) came from connectivity analysis.
- Position-parse limitation noted above applies to any automated copper-presence claim; sensor
  keepout/copper checks were not machine-verified (sensors measure in-plane field; copper clearance
  is less critical than for capacitive parts).

## Previous-review delta

`schematic_review.md` (Jul 3) and `power_architecture.md` (Jul 5) action items: all power-path
items (1–9) implemented and verified; the CA-IS372x isolator was deleted and replaced with the
half-duplex UART link + SRV05 ESD on J1 data, as planned. The three blockers above are all new
findings (never covered by prior reviews).
