# osu pad (TMR2615F_osu_pad) — schematic review

Review of `TMR2615F_osu_pad.kicad_sch` — a 6-key analog magnetic macropad / devboard for the TMR2615 sensor and Artery AT32F405 MCU. Generated from a multi-agent review (8 subsystem passes + adversarial verification against the netlist and datasheets in `hw/refs/`). Every kept finding cites specific nets/pins and datasheet specs; findings marked ✅ were independently re-verified by hand.

## Summary

The osu pad schematic has two independent show-stoppers that each kill the board's core magnetic-sensing function: the TMUX1574 mux enable (U3.13, active-low EN#) is hard-tied HIGH to +3.3VA so all four channels are permanently OFF, and all six TMR2615 sensors carry exclude_from_bom so they would never be purchased or placed. Beyond those, the sensor output network violates the sensor's 10 nF max-load-cap spec by ~10x (100 nF sitting directly on VOUT with a 0 Ohm series R that removes op-amp isolation and the anti-alias pole), and several USB/power protection details are wrong or unpopulated: U4 LM66100 ~CE tied to GND defeats reverse-current blocking and bypasses U5's current limit, 30 pF shunt caps (C2/C3) will prevent USB High-Speed on J2, J4's CC Rd resistors are DNP, and the J1 isolator U1 has no supply on either VDDA/VDDB pin so the isolated-UART port is dead. A cluster of medium/low robustness items rounds out the actionable list (SK6812 3.3V level margin and missing per-LED decoupling, I2C1 pull-ups defeated by a DNP jumper header, no ORing between J2/J4 VBUS, hotswap sockets tapping VOUT). A broad set of items were verified correct (TPS7A4700 3.3V ANY-OUT programming, MCU support/clock/boot/pin mapping, mux addressing, ESD placement) and most ERC violations are benign PWR_FLAG/ferrite/SBU artifacts. Three part datasheets (AP22653, SRV05-4A, XC6206) are unavailable, leaving the eFuse current-limit setpoint, the HS ESD-array line capacitance, and the 3.3V LDO rating unconfirmed — though none of these blocks a kept finding. 


## 🔴 CRITICAL

### #1 TMUX1574 mux (U3) active-low EN# is hard-tied to +3.3VA — mux permanently disabled, no sensor reaches the ADC  ·  _CONFIRMED_
**Parts:** U3, U7  ·  **Area:** sensor/mux

- **What:** Netlist [+3.3VA] contains BOTH U3.13:EN#[input] and U3.14:VDD[power_in] — the active-low enable is tied to the same rail as its own VDD (hard HIGH); no pull-down/GND to U3.13 exists anywhere. TMUX1574 datasheet Pin Functions (UQFN pin 13 = EN): 'Active low enable: When this pin is high, all switches are turned off... Internal 6 MOhm pull-down to GND.' Truth table: EN=1, SEL=X -> Hi-Z (OFF). With EN=VDD all four SPDT channels are OFF regardless of SEL(/TMUX_SEL=PB2), so sources S1A/S1B..S3A/S3B (the six TMR sensors) and S4A/S4B (J7 header) are disconnected from drains D1..D4 = /ADC1_IN3..IN0 (U7 PA3/PA2/PA1/PA0).
- **Impact:** The entire analog sensing path is dead as drawn. All mux-fed ADC inputs read floating; none of the 6 TMR2615 sensors can be measured. This is the board's core function.
- **Fix:** Tie U3.13 (EN#) to GND (active-low asserted). The internal 6 MOhm pull-down means even leaving it unconnected would enable it; do NOT leave it on +3.3VA. Optionally route to a spare GPIO with a pull-down for firmware gating.

### #2 All 6 TMR2615 sensors (U10/U13/U16/U19/U22/U25) carry exclude_from_bom — core sensing parts absent from the BOM  ·  _CONFIRMED_
**Parts:** U10, U13, U16, U19, U22, U25  ·  **Area:** global/BOM

- **What:** Raw netlist osupad.net sets (property (name "exclude_from_bom")) on exactly 7 parts: J6 (Tag-Connect land, legitimate) and the six TMR2615x sensors (verified at the U10/U13/U16/U19/U22/U25 blocks). The exported osupad-bom.csv omits U10/U13/U16/U19/U22/U25 accordingly. The sensors are fully wired (each U*.3:VDD on +3.3VA; each U*.1:VOUT into R17..R22 -> mux), but a turnkey PCBA sources purchase/placement from the BOM, so excluded parts are neither bought nor placed.
- **Impact:** Built from this BOM the board is an MCU carrier with six unpopulated sensor sites — total loss of primary function even if every other issue (including the mux EN, rank 1) is fixed.
- **Fix:** Clear the 'Exclude from bill of materials' attribute on U10/U13/U16/U19/U22/U25 and re-export the BOM AND the placement/position file; add the TMR2615x MPN/supplier fields. Leave exclude_from_bom on J6 only.


## 🟠 HIGH

### #3 Sensor VOUT caps = 100nF exceed TMR2615 10nF max load cap by 10x; 0 Ohm series R17-R22 remove op-amp isolation and defeat the anti-alias filter (all 6 channels)  ·  _ADJUSTED_
**Parts:** C35, C38, C41, C44, C47, C50, R17, R18, R19, R20, R21, R22  ·  **Area:** sensor

- **What:** Net-(U10-VOUT) = {U10.1:VOUT, C35.2, R17.1, U11.2}, with C35.1 on GND — so C35 (100 nF, comps.txt) sits directly across VOUT-GND. R17..R22 = 0 Ohm (0402WGF0000), so the mux-side 30 pF cap (DNP) lumps onto VOUT and there is no RC pole. TMR2615x datasheet 5 Electrical Spec: max load capacitance CL = 10 nF; application Fig.6 uses CL = 0.1 nF; VOUT current drive max 1.5 mA. 100 nF is 10x the abs-max load cap and ~1000x the datasheet example — consistent with a 0.1uF-vs-0.1nF unit error. All six channels are identical.
- **Impact:** Driving 10x the rated capacitive load with zero series isolation risks the sensor's internal output buffer ringing/slow-settling/oscillating, and the 0 Ohm R leaves no anti-alias pole ahead of the ADC. Affects all six sensing channels — the reliability of the core signal path (kept high, not critical, because heavily-filtered slow key-press signals may still be sampled).
- **Fix:** Reduce the sensor-side cap to within spec (datasheet uses 0.1 nF; ~470 pF-1 nF is a reasonable noise cap). Populate R17..R22 with a real value (e.g. 1 kOhm) to isolate the buffer and form the RC anti-alias filter; move any bulk cap to the mux-drain/ADC side (D1..D4) rather than directly on VOUT.

### #4 U4 LM66100 ~CE tied to GND defeats reverse-current blocking; anti-parallel with always-on U5 bypasses the AP22653 current limit on the +5V<->J1 path  ·  _CONFIRMED_
**Parts:** U4, U5, J1  ·  **Area:** power

- **What:** [/VBUS_B] = U4.1:VIN + U5.6:OUT + J1.VBUS; [+5V] = U4.6:VOUT + U5.1:IN + U5.3:EN (hard-on); [GND] = U4.3:~CE. So U4 (VIN=/VBUS_B->VOUT=+5V) and U5 (IN=+5V->OUT=/VBUS_B) are anti-parallel between +5V and J1's VBUS. LM66100 datasheet: with CE=GND and VIN>0, CE<VIN always -> internal PMOS permanently ON; reverse-current blocking (8.3.2 RCB) requires CE->VOUT, which is NOT done here (CE->GND is the plain always-on RPP-only mode, 8.3.1). An always-on PMOS conducts bidirectionally (~79 mOhm), so U4 is an uncontrolled path in parallel with U5. Abs-max ISW = 1.5 A continuous / 2.5 A pulse.
- **Impact:** For the board-sources-J1 direction, an overload/short on J1 VBUS draws current +5V->U4->/VBUS_B limited only by RON, bypassing U5's current limit — risking >1.5 A through U4 and collapse of the +5V rail that also feeds the MCU/LEDs/J2/J4. Passes bring-up under light load; fails under a J1 fault or dual-source condition.
- **Fix:** Tie U4 pin 3 (~CE) to U4 VOUT (+5V) to enable Always-ON Reverse Current Blocking (datasheet 8.3.2), optionally with a small series R_CE per 9.2.1.2. This restores U4 as a sink-only ORing diode so U5 is the sole, current-limited source path to J1.

### #5 30pF shunt caps C2/C3 on OTGHS D+/D- will prevent USB High-Speed on J2 (FS-port caps C8/C9 are tolerable)  ·  _CONFIRMED_
**Parts:** C2, C3, U7, J2, U2  ·  **Area:** mcu/usb

- **What:** [/OTGHS1_D+] = C2.2 + J2.A6/B6 + U2(TVS) + U7.35:OTGHS1_D+; [/OTGHS1_D-] = C3.2 + ... + U7.34:OTGHS1_D-; C2.1/C3.1 on GND. comps.txt: C2=C3=30 pF C0G, NOT DNP. AT32F405 2.13.6: dedicated on-chip HS PHY (480 Mb/s) on pins 34/35; Table 45 tr/tf ~100 ps into ~45 Ohm. A 30 pF single-ended shunt gives RC ~0.7 ns (rise ~1.5 ns vs the 2.08 ns HS bit period) plus a gross impedance discontinuity on the 90 Ohm pair — closes the HS eye and fails chirp. The identical C8/C9 (30 pF) on the OTGFS pair (J4, 12 Mb/s, ~83 ns bit period) are harmless.
- **Impact:** J2 cannot come up at High-Speed (falls back to Full-Speed at best), defeating the headline reason the board places J2 on the OTGHS PHY.
- **Fix:** DNP/remove C2 and C3 (or reduce to <=2 pF if an EMI cap is truly needed). Confirm the U2 SRV05-4A ESD array is a low-capacitance type suitable for HS — its datasheet is unavailable, so if it is a high-cap part swap for a <1 pF USB-HS ESD array. C8/C9 on the FS pair may stay but could be reduced.

### #6 Isolator U1 (CA-IS3722) VDDA (pin1) and VDDB (pin8) connect to no supply rail — U1 is unpowered, the entire J1 isolated-UART port is dead  ·  _ADJUSTED_
**Parts:** U1, C1, C4  ·  **Area:** usb/isolator

- **What:** [Net-(U1-VDDA)] (2) = {C1.2, U1.1:VDDA[power_in]} and [Net-(U1-VDDB)] (2) = {C4.2, U1.8:VDDB[power_in]} — each net contains only a 100 nF decap and the IC power pin; neither touches +3.3V/+5V/+3.3VA. These are genuine dead-end supply nets (contrast +3.3V driven by U8.3:Vout, +5V by U4.6:VOUT), and match the ERC power_pin_not_driven hits on U1.1/U1.8. CA-IS372x datasheet Table 6-1: pin1=VDDA, pin8=VDDB, both required (2.375-5.5 V); UVLO+ ~2.24 V is never met. Data path is otherwise wired (MCU UART7 <-> U1 VI1/VO2, J1 D+/D- <-> U1 VO1/VI2).
- **Impact:** Both sides of the digital isolator are unpowered, so U1 transmits nothing and the J1 isolated-UART port is completely non-functional. The rest of the board (sensing, J2/J4 USB, SWD, LEDs) is unaffected, so this is one dead feature (high), not a board-kill (critical).
- **Fix:** Connect U1.8 VDDB (MCU/logic side) to +3.3V and U1.1 VDDA (J1/field side) to a 2.375-5.5 V rail, keeping C1/C4 as decaps. Since GNDA/GNDB are already common (rank 16), the simplest fix is to tie both VDDA and VDDB to +3.3V. If true galvanic isolation is intended, VDDA needs a separate isolated supply referenced to an isolated GNDA.

### #7 J4 USB-C CC pull-downs R4/R5 (5.1k Rd) are DNP — J4 won't attach or power over a C-to-C cable  ·  _CONFIRMED_
**Parts:** R4, R5, J4  ·  **Area:** usb

- **What:** [Net-(J4-CC1)] = J4.A5:CC1 + R5.2; [Net-(J4-CC2)] = J4.B5:CC2 + R4.2; R4.1/R5.1 on GND. comps.txt: R4/R5 = 5.1 kOhm; raw netlist osupad.net AND osupad-bom.csv mark both DNP (verified: R1/R2 on J2 are NOT DNP). J4.VBUS -> +5V and J4 D+/D- -> /OTGFS1 (U7.45/PA12, U7.44/PA11), so J4 is wired as a USB-FS device/sink. USB Type-C requires a sink to present 5.1k Rd on CC1/CC2 so a source detects attach and applies VBUS.
- **Impact:** With R4/R5 unpopulated, a Type-C source over a C-to-C cable never detects attach, never applies VBUS, and J4 won't enumerate or power the board through J4. (It would still work over a legacy A-to-C cable where VBUS is unconditional, so the port is cable-dependent, not universally dead.) J2 works because R1/R2 are populated.
- **Fix:** Populate R4/R5 (5.1k Rd) to match J2's R1/R2 if J4 is a real device port; otherwise document J4 as legacy-cable/expansion-only.


## 🟡 MEDIUM

### #8 First SK6812 data driven at 3.3V but VIH = 0.7xVDD = 3.5V at 5V — no level shifter  ·  _ADJUSTED_
**Parts:** U7, U12  ·  **Area:** leds

- **What:** [/DIN] (2) = U12.2:DIN[input] + U7.62:PB9[bidirectional] — MCU PB9 (3.3V logic; U7 VDD on +3.3V) drives the first LED directly with no buffer. All six SK6812 VDD pins are on +5V. SK6812MINI-E 10 (VDD=5.0V): VIH min = 0.7xVDD = 3.5 V. 3.3V < 3.5V, so the logic-high into U12.DIN is ~0.2 V below spec (safe vs abs-max input, but not a guaranteed HIGH). Downstream LEDs are fine (each driven by the prior DOUT ~5V).
- **Impact:** The first LED may fail to reliably latch data; marginal over temperature/supply/lot, typically appearing as whole-chain glitching/wrong colors traced to the head. LEDs are a cosmetic RGB feature and 3.3V->5V SK6812 chains often work at room temperature, so medium — one subsystem reviewer argued high on strict spec-violation and whole-chain-gating grounds.
- **Fix:** Add a 5V-supplied buffer with TTL inputs on PB9->U12.DIN (e.g. 74AHCT1G125 / 74LVC1T45); or drop only U12's VDD via a series Schottky to ~4.4V (VIH~3.1V while DOUT ~4.4V still exceeds the next LED's 3.5V); or run the whole chain at 3.7-4.3V (SK6812 VDD range 3.7-5.5V). At minimum verify margin on hardware.

### #9 No per-LED decoupling capacitors on the SK6812 chain  ·  _CONFIRMED_
**Parts:** U12, U15, U18, U21, U24, U27  ·  **Area:** leds

- **What:** The entire [+5V] net carries only C6/C19/C31 (10 uF X5R bulk) + C7/C18 (100 nF). None of the six SK6812 VDD pins (U12.3..U27.3) has a dedicated adjacent bypass cap. SK6812MINI-E 16: a per-bead decoupling cap is indispensable for stable inter-IC operation.
- **Impact:** Each SK6812 switches ~50-60 mA at PWM rates; without local bypass, supply transients can corrupt the single-wire data timing -> flicker/color errors, worst at the end of the chain far from the bulk caps. A short 6-LED chain will likely still run, so medium.
- **Fix:** Add a 100 nF X7R 0402 from VDD (pin3) to GND (pin1) placed adjacent to each of U12/U15/U18/U21/U24/U27 (6 caps).

### #10 I2C1 pull-ups R23/R24 are defeated by DNP jumper header J3 — default build has no I2C1 pull-ups  ·  _CONFIRMED_
**Parts:** J3, R23, R24, J5, U28  ·  **Area:** global

- **What:** R23 (5.1k) on /I2C1_SCL and R24 (5.1k) on /I2C1_SDA both terminate their supply side at [Net-(J3-Pin_2)] = {J3.2, R23.2, R24.2}; J3.1 is on +3.3V. J3 is a 2-pin header (Conn_01x02) acting as an enable jumper, and osupad-bom.csv marks J3 DNP, so +3.3V never reaches the pull-ups. I2C1 is a real external bus: PF4/PF5 (U7.18/19) -> U28 ESD -> 120 Ohm R25/R26 -> J5 (SH1.0 4P).
- **Impact:** As shipped, the external I2C1 connector J5 cannot communicate unless the attached module supplies its own pull-ups. Placed-but-defeated pull-ups are a latent inconsistency that will silently kill the bus.
- **Fix:** Decide intent: if on-board pull-ups are wanted by default, populate J3 with a shunt (or replace J3 with a hard link / default-closed solder jumper); if the remote module provides pull-ups, remove R23/R24 to avoid confusion. Document the jumper either way.

### #11 J1 USB-C CC1/CC2 float (no Rd/Rp) — J1 cannot negotiate Type-C power over a compliant cable  ·  _ADJUSTED_
**Parts:** J1  ·  **Area:** usb

- **What:** [unconnected-(J1-CC1-PadA5)] (1) = J1.A5:CC1 and [unconnected-(J1-CC2-PadB5)] (1) = J1.B5:CC2 — no resistor (contrast J2's R1/R2). J1's VBUS (/VBUS_B) has both a sink path (U4 LM66100, VIN=/VBUS_B->+5V) and a source path (U5 AP22653, IN=+5V->OUT=/VBUS_B), i.e. J1 is a bidirectional power port. With CC floating it advertises neither Rd (sink) nor Rp (source). J1's data pins are an isolated UART, so unused CC-for-data is expected.
- **Impact:** Over a compliant C-to-C cable J1 will not attach in either power role, so VBUS is not negotiated in or out. The presence of both U4 and U5 implies the VBUS path was meant to be usable over a real cable, making this a real gap; benign only if J1 is used with a fixed/captive harness. One reviewer rated this low as fully intent-dependent.
- **Fix:** Add 5.1k Rd on CC1/CC2 if J1 is a sink; add Rp (e.g. 5.1k to VBUS, or a CC controller) if it should source 5V; if only ever used with a captive harness, add a no-connect flag and document.


## 🔵 LOW

### #12 J2 and J4 VBUS hard-wire to +5V with no ORing — two simultaneously-plugged hosts back-drive each other  ·  _ADJUSTED_
**Parts:** J2, J4  ·  **Area:** power/usb

- **What:** [+5V] contains J2.A4B9/B4A9:VBUS and J4.A4B9/B4A9:VBUS directly on the same node with no series diode/switch between the two connectors (contrast J1, whose VBUS reaches +5V only through U5+U4).
- **Impact:** If two hosts are plugged into J2 and J4 at once, their VBUS rails are shorted and the higher back-feeds the lower, which can trip a host's VBUS OCP. Both are device ports and normal use is a single host; USB hosts are current-limited (worst case = OCP trip / failed enumeration, not damage), so this is a robustness/documentation foot-gun. One reviewer kept medium for the back-drive risk.
- **Fix:** For a devboard, silkscreen 'plug only one of J2/J4 at a time.' For robustness, OR the two VBUS inputs with ideal-diode/Schottky parts into +5V (as J1 already is).

### #13 Kailh hotswap sockets tap sensor VOUT to GND — a mechanical contact switch would short the sensor output  ·  _ADJUSTED_
**Parts:** U11, U14, U17, U20, U23, U26  ·  **Area:** sensor

- **What:** Each CPG151101 socket has pin1 on GND (U11.1 in [GND]) and pin2 on the sensor VOUT node (Net-(U10-VOUT) includes U11.2); same for U14/U17/U20/U23/U26. The socket tap is upstream of R17..R22, so a short there is not current-limited by the series R. TMR2615 VOUT current-drive abs-max = 1.5 mA, min load 10 kOhm.
- **Impact:** If a standard mechanical NO switch (contacts internally connected) is fitted, a keypress shorts VOUT (~50% VDD) to GND, corrupting that channel and exceeding the 1.5 mA drive limit. This is a magnetic keypad, so the intended (contactless) switch has open dummy retention pins and the pattern is harmless — hence a build-time foot-gun, not a normal-operation defect.
- **Fix:** Confirm and document that only contactless magnetic switches (no internal pin1-pin2 contact) may be installed. If mechanical switches must be tolerated, don't tie both socket pads to VOUT/GND, or add a series resistor so a closed contact can't hard-short VOUT.

### #14 No series/damping resistor on the SK6812 data input  ·  _CONFIRMED_
**Parts:** U7, U12  ·  **Area:** leds

- **What:** [/DIN] connects U7.62 (PB9) straight to U12.2 (DIN) with no series element. SK6812MINI-E 16 recommends series protection resistors on the signal in/out lines (~500 Ohm for short strips) to protect the IC I/O against hot-plug transients and to damp ringing/reflections.
- **Impact:** Fast 3.3->5V edges into U12.DIN can ring and, on any hot-plug/ESD event, stress the input. A robustness/EMC nicety for a short on-board trace with soldered-down LEDs, hence low.
- **Fix:** Place a small series resistor (33-100 Ohm for damping) in /DIN close to U12.DIN; if the level-shifter fix (rank 8) is adopted, put the resistor on the shifter output.

### #15 J6 TC2030 debug connector: benign lib_symbol_mismatch, but a non-standard pinout (pin1=GND, no VTref)  ·  _CONFIRMED_
**Parts:** J6, U7  ·  **Area:** erc/debug

- **What:** The ERC lib_symbol_mismatch on J6 is benign — the netlist is generated from the embedded symbol and is authoritative. As drawn: SWDIO(J6.2)->PA13, SWCLK(J6.4)->PA14, SWO(J6.6)->PB3, USART1_TX(J6.3)->PA9, USART1_RX(J6.5)->PA10, and J6.1 is on GND. This is a custom TC2030 pinout: pin1=GND (standard ARM TC2030 has pin1=VCC/VTref) and NO VTref/target-voltage pin is broken out.
- **Impact:** Connectivity is fine and the built netlist is unaffected. But a debug probe that requires VTref sensing (e.g. some J-Link modes) will read 0 V, and a standard TC2030 cable expecting pin1=VCC would tie its VCC-sense to GND.
- **Fix:** Run Tools -> Update Symbols from Library on J6 and re-check ERC (the mismatch clears if the embedded copy is simply stale). Confirm the debugger in use is configured for a self-powered target (no VTref required). No connectivity change needed.


## ⚪ INFO

### #16 U1 isolator GNDA and GNDB are the same net (GND) — no galvanic isolation is realized; U1 is used as a common-ground buffer  ·  _ADJUSTED_
**Parts:** U1, J1  ·  **Area:** usb/isolator

- **What:** [GND] contains both U1.4:GNDA and U1.5:GNDB, plus J1's shell/GND pins (J1.A1B12/B1A12). There is no isolated ground plane, no isolated VDDA supply (Net-(U1-VDDA) is a two-pin island), and J1's GND is on board GND — so no ground-potential difference can develop across the CA-IS3722 barrier (rated 3750 VRMS). It is used as a same-ground signal buffer.
- **Impact:** The isolator's isolation function is not achieved; it is over-specified for a common-ground link. If galvanic isolation of J1 was the intent, it is not met; if common-ground buffering is acceptable for a devboard, it works but the part is over-spec'd. No electrical harm.
- **Fix:** Decide intent. For true isolation, break GNDA from board GND (isolated ground + isolated VDDA supply + J1 shell on the isolated ground). For a devboard buffer, leave as-is and document. Fix the unpowered-VDD issue (rank 6) regardless.

### #17 Firmware must add a settling delay after each SEL/PB2 toggle before ADC sampling (0 Ohm R17-R22, 30pF DNP mux-side caps)  ·  _CONFIRMED_
**Parts:** U3, R17, R18, R19, R20, R21, R22  ·  **Area:** mux

- **What:** R17-R22 = 0 Ohm, so the mux source nodes have no RC beyond the 30 pF caps (C36-family, DNP). TMUX1574: charge injection QC ~3.5 pC and tTRAN 160 ns typ / 350 ns max. On a SEL A/B change the drain connects to a freshly-charged node plus a ~0.1 V injection transient. Not a wiring error — a firmware timing consideration.
- **Impact:** If the ADC samples immediately after toggling PB2, readings can be corrupted by the switch transient.
- **Fix:** In firmware, insert a settling delay (>~1 us, comfortably above tTRAN 350 ns and the RON x C time constant) after each PB2/SEL change before starting a conversion.

### #18 ERC hygiene: no PWR_FLAG in the design; VDDA-through-ferrite, GND-shield and SBU flags are benign; C28/C29 refdes gap benign  ·  _CONFIRMED_
**Parts:** U1, U7, J1, J2, J4  ·  **Area:** erc

- **What:** osupad.net has 0 PWR_FLAG symbols, so the LDO/diode outputs feeding +3.3V/+5V/+3.3VA and the GND net (J1.1:EH shield tab) trip power_pin_not_driven cosmetically; U7.13:VDDA trips because it is fed from +3.3VA through ferrite L1 (ERC can't propagate power across a passive). Six pin_not_connected on SBU1/SBU2 (J1/J2/J4 pads A8/B8) are correct for USB-2.0-only Type-C. C-series skips C28/C29 (deleted-part numbering gap; absent from BOM/netlist); U1..U28 continuous.
- **Impact:** These ERC entries are noise, not defects. The one genuinely-real power_pin_not_driven (U1 VDDA/VDDB) is a separate finding at rank 6.
- **Fix:** Add one PWR_FLAG on GND and on Net-(U7-VDDA) (or exclude that pin in ERC), and place no-connect (X) flags on the six SBU pads, to clear the benign ERC. No wiring change.

### #19 Verified OK: TPS7A4700 (U9) ANY-OUT programming yields 3.3V; NR/SENSE/output/EN network correct  ·  _CONFIRMED_
**Parts:** U9, C32, C33  ·  **Area:** power

- **What:** Grounded ANY-OUT pins U9.8(1P6V)/U9.11(0P2V)/U9.12(0P1V), others floating -> VREF 1.4 + 1.6 + 0.2 + 0.1 = 3.3 V (datasheet Eq.2 / Table 6-1). SENSE/FB (U9.3) tied to OUT (+3.3VA); NR C32=1 uF; COUT C33=47 uF (the datasheet-recommended value); IN/EN on +5V (1.7 V headroom >> 307 mV dropout at 1 A). This rail powers the 6 sensors, the mux and MCU VDDA.
- **Impact:** The analog rail is correctly set to 3.3 V per TI's reference. No defect.
- **Fix:** No change needed.

### #20 Verified OK: MCU (U7) power/decoupling, clock, reset, boot, USB pin mapping, ADC mapping, OTGHS1_R  ·  _CONFIRMED_
**Parts:** U7, X1, L1, R12  ·  **Area:** mcu

- **What:** VDD 1/36/64->+3.3V, VSS 31/63 + VSSA 12->GND, VDDA 13->+3.3VA via ferrite L1 (no external VBAT/VREF+ on LQFP64). X1 12 MHz on PF0/PF1 with 30 pF loads (~20 pF CL). NRST R9/C15/SW2; BOOT0=PF11 with 3.3k pull-down + SW1 (boot-from-flash default, correct sense). OTGHS1_R = 12k +/-1% (datasheet-exact). OTGHS D+/D- on dedicated pins 34/35; OTGFS on PA11/PA12. Mux drains -> PA0-PA3 = ADC1_IN0-IN3 (single ADC, no cross-split). All 64 pins map uniquely.
- **Impact:** Core MCU support circuitry is correct. No defect.
- **Fix:** No change needed.

### #21 Verified OK: TMUX1574 SEL logic, channel-to-sensor addressing, D->ADC mapping, VDD decoupling and signal range (apart from the EN defect, rank 1)  ·  _CONFIRMED_
**Parts:** U3, C34  ·  **Area:** mux

- **What:** SEL=U3.15=PB2. Truth table: SEL=0->SxA->Dx, SEL=1->SxB->Dx. D1=/ADC1_IN3(PA3), D2=/ADC1_IN2(PA2), D3=/ADC1_IN1(PA1), D4=/ADC1_IN0(PA0); all 6 sensors addressable across the A/B sets, Ch4 to the J7 header. C34=100 nF VDD decap (meets the 0.1-10 uF spec). VDD=3.3V and 0-3.3V signal are within the 0..VDDx2 range; RON ~2 Ohm negligible vs ADC input.
- **Impact:** Addressing and mux operating conditions are sound once EN is fixed. No defect.
- **Fix:** No change; verify C34 is placed adjacent to U3 pin 14 in layout.

### #22 Verified OK: J1 is an isolated UART on a USB-C connector (not a USB port) — correct use of the 1+1 CA-IS3722  ·  _CONFIRMED_
**Parts:** U1, J1  ·  **Area:** usb

- **What:** CA-IS3722 = 1 forward + 1 reverse channel. MCU /UART7_TX (VI1 pin7) -> J1 D+ (VO1 pin2); J1 D- (VI2 pin3) -> MCU /UART7_RX (VO2 pin6): a clean isolated full-duplex UART. The R13-R16 0 Ohm crossbar is populated as exactly one diagonal (R15/R16 fitted; R13/R14 DNP in osupad.net), so there is NO TX/RX or VI1/VO2 short.
- **Impact:** J1 will never work as USB, nor is it meant to. No defect here beyond the VDD/CC issues above.
- **Fix:** Label/firmware treat J1 as an isolated UART, not USB.

### #23 Verified OK: SRV05-4A ESD placement (U2/U6/U28) and VBUS clamp correct; SBU floating benign (SRV05 datasheet caveat)  ·  _CONFIRMED_
**Parts:** U2, U6, U28  ·  **Area:** usb

- **What:** U2/U6 sit connector-first on the D+/D- pairs with VP on +5V and VN on GND; U28 protects I2C1 (VP on +3.3V). No series element between connector and TVS, so the array sees the transient first, and the VCC-GND clamp provides VBUS TVS. SRV05-4A datasheet is unavailable, so the pin map (I/O1=1, GND=2, I/O2=3, I/O3=4, VCC=5, I/O4=6) is inferred from the symbol pin-function labels and the standard SOT-23-6 array.
- **Impact:** ESD protection is wired correctly; no defect. Confidence is medium pending the SRV05-4A datasheet for exact pinout and (for rank 5) HS line capacitance.
- **Fix:** No change; confirm the SRV05-4A pinout/capacitance against the ON Semi datasheet when available.

### #24 Verified OK: status LEDs LED1/LED2 current-limit resistors (330 Ohm) correct  ·  _CONFIRMED_
**Parts:** LED1, LED2, R10, R11  ·  **Area:** leds

- **What:** LED1 anode on +3.3VA, LED2 anode on +3.3V, each cathode -> 330 Ohm (R10/R11, comps.txt) -> GND. Polarity correct; current ~1-4 mA depending on Vf, well under any 0603 LED rating; resistor dissipation <16 mW into a 62.5 mW part.
- **Impact:** Indicator LEDs wired and valued correctly. No defect.
- **Fix:** Optional: relocate LED1 from +3.3VA to +3.3V to keep the low-noise analog rail clean.

### #25 Single unified GND (no analog/digital split, no net-tie) — verify a solid ground pour in layout  ·  _CONFIRMED_
**Parts:** U9, U3  ·  **Area:** global

- **What:** No NT1/net-tie exists; one [GND] net (138 pins) covers all subsystems. Supplies are split (+3.3V digital from U8, +3.3VA analog from U9) but the return is common. The only 'GNDA' token is U1.4's pin-function name, tied into GND.
- **Impact:** A single solid ground plane is acceptable/preferable for a mixed-signal board of this scale; absence of a net-tie is not a defect. The real concern is layout-time.
- **Fix:** In layout, keep one continuous pour and route the +3.3VA analog return (sensors, mux, TPS7A4700) away from the SK6812 LED and USB return currents.

### #26 DNP audit: remaining DNP parts are sensible; note the C0G caps are 30pF (not 300pF)  ·  _CONFIRMED_
**Parts:** C2, C3, C8, C9, R13, R14, J7, J8, J9, J10  ·  **Area:** global

- **What:** 19 DNP flags total: C2/C3/C8/C9 (USB data shunt caps — correctly unpopulated), C36/C39/C42/C45/C48/C51 (mux-side 30 pF, moot given 0 Ohm series R), R13/R14 (the unused isolator-crossbar diagonal), J7/J8/J9/J10 (dev headers), plus R4/R5 and J3 (the functional DNPs called out at ranks 7 and 10). The 0402CG300J500NT value is 30 pF C0G, NOT 300 pF as some documentation (including the task brief) states.
- **Impact:** The deliberate DNPs are individually correct (except R4/R5 and J3, handled above). No action beyond the functional ones.
- **Fix:** No action; correct any documentation that calls the C0G caps '300 pF' — they are 30 pF.


## Rejected in verification (not defects)

- xc6206-budget-unverifiable (power) — REJECTED: premise false. The MCU run current IS quotable (AT32F405 datasheet Table 23: max 74.3 mA at 216 MHz with all peripherals incl. USB PHY, 105 C), giving ~2.7x margin over the XC6206P332 ~200 mA rating — beyond the finding's own '>2x = no change' threshold. The analog/LED loads are on +3.3VA/+5V, not +3.3V, so the digital rail carries only MCU VDD. The finding collapses to 'rail is adequate' with no open item.
- isolator-uart-crossbar-short (usb) — REJECTED: load-bearing premise ('all four R13-R16 populated per BOM') is false. The authoritative osupad.net marks R13 and R14 (property (name "dnp")) while R15/R16 are populated, so the board already fits exactly one diagonal ({PC6,VO2} and {PC7,VI1}) — no PC6<->PC7 or VI1<->VO2 short. The finding relied on the flat comps.txt, which does not encode DNP. The recommended 'populate one diagonal' state is already implemented.

## Open questions (need missing datasheets or design intent)

- AP22653 (U5) datasheet is unavailable: the current-limit setpoint (R_ILIM value/net), reverse-current-blocking behavior, EN polarity/threshold, and soft-start could not be verified. This underpins rank 4's assumption that U5 is the intended current-limited source path to J1 — confirm U5 is correctly configured before relying on it as the sole protected source.
- SRV05-4A (U2/U6/U28) datasheet is unavailable: the exact pinout is inferred from the KiCad symbol's pin-function labels (VCC=5, GND=2, I/O=1/3/4/6), and — critically for rank 5 — the line/junction capacitance is unknown. Even after removing C2/C3, a high-capacitance TVS on the OTGHS pair could still impair USB High-Speed. Obtain the ON Semi datasheet to confirm HS suitability (and swap for a <1 pF USB-HS array if needed).
- XC6206P332 (U8) datasheet is unavailable: the ~200 mA / 3.3V rating and dropout rest on part knowledge. The ~74 mA max MCU load leaves comfortable margin, but confirm the actual current rating and dropout of the part actually fitted.
- Design intent for J1 and the isolator (ranks 6/11/16): is J1 meant to be a galvanically-isolated, cable-powered Type-C port, or a bench/captive-harness common-ground UART? This decides whether the floating CC termination and the common GNDA/GNDB are true defects or acceptable simplifications — and whether VDDA should come from an isolated supply vs simply +3.3V.
- Intended switch type for the hotswap sockets (rank 13): confirm that only contactless magnetic switches (open dummy retention pins) will ever be installed, so the socket-to-VOUT/GND tap cannot short the sensor output.

---
*Datasheets downloaded to `hw/refs/` during review: LM66100, CA-IS372x, SK6812MINI-E. Still missing (blocked by JS/cookie walls): AP22653W6, SRV05-4A, XC6206P332.*
