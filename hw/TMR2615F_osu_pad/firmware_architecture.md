# Giris firmware — architecture options

Scope: the firmware platform for the `TMR2615F_osu_pad` (6-key analog dev pad) **and** the full-size
split keyboard it is a prototype for — **64 keys total, 32 per half**. One image, either half, three topologies, 8 kHz USB HS, browser
configurator, Wooting-class analog feature set.

Written 2026-08-24 after a survey of every open keyboard firmware that could plausibly serve, plus
primary-source verification against the AT32F402/405 datasheet + reference manual, the USB 2.0 spec,
TinyUSB, Chromium, EDK2, and the source of libhmk / QMK / ZMK / RMK / minipad / DeskHop / VIA / Vial.
Claims below are marked where they are still **unverified** and need a bench measurement.

---

## 0. TL;DR

**Nothing off the shelf fits.** The requirement set — 8 kHz *and* analog *and* split *and* dual-host
*and* a good web UI — has no intersection in existing firmware. The closest single project (libhmk)
matches the silicon and the analog feature set exactly but has no split, no RGB, is GPL-3.0, and its
superloop cannot hold a 125 µs deadline once a link and RGB are added.

**Recommended stack:**

| Layer | Choice |
|---|---|
| Base | Clean-room C on Artery BSP (BSD-3) + TinyUSB (MIT), CMake. libhmk as a read-only reference. |
| Scheduling | SOF-phased interrupt pipeline, strict NVIC priority hierarchy, lock-free SPSC handoff |
| Split cut point | **Semi-smart peripheral**: each half owns analog→key-position; centre owns keymap/layers/HID |
| Link | Fixed-size full-duplex isochronous frames on **USART6 at 9 Mbaud**, DMA + IDLE-line, CRC-16; 115200 open-drain only for discovery |
| Roles | **Symmetric peers + transferable input-owner token.** Master/slave survives only as a *power* concept |
| Config transport | Own protocol over a dedicated raw-HID interface (vendor usage page), WebHID |
| Configurator | Own web app, self-describing device (gzipped keyboard metadata in flash) |
| Storage | RAM-mirrored blob + word write log now; banked image + hot-field journal for the full-size board |

The single decision that shapes everything else is #4 (peer model). The hardest constraints are the
flash erase stall (6.6–8 ms on this 128 KB part) and the **44–74 µs** the 64-key pipeline costs out of
every 125 µs microframe.

**Two board respins are worth doing**: the per-key filter to **2.2 kΩ + 47 nF** (kills 12.9 LSB of
inter-key crosstalk), and the sensor's **offset trim code** to reclaim the half of the ADC range the
unipolar field never uses. The 120 Ω link resistors should be left alone.

---

## 1. Prior art: what exists and why none of it is the answer

| Project | Platform | 8 kHz | Analog | Split | Web UI | License | Why not |
|---|---|---|---|---|---|---|---|
| **libhmk** | AT32F405 + STM32F446, TinyUSB, superloop | ✅ HS | ✅ full | ❌ | ✅ own (hmkconf, Svelte) | GPL-3.0 | Right silicon, right features, **no split**, no RGB, superloop won't hold the deadline, `uint8` distance |
| **QMK** | everything, ChibiOS | ❌ ~1 kHz | ❌ upstream | ✅ (≤1 kHz) | via VIA/Vial | GPL-2.0 | Analog is a [feature request](https://github.com/qmk/qmk_firmware/issues/25875), split transport is request/response with 20 ms timeouts |
| **Vial (vial-qmk)** | QMK fork | ❌ | ❌ | ✅ | ✅ vial.rocks | GPL-2.0 | Best *protocol* ideas; the web app is a PyQt5 app compiled to WASM (~17.8 MB first load) |
| **ZMK** | Zephyr, BLE-first | ❌ | ❌ | ✅ (event, 15–20 ms) | ✅ Studio (protobuf) | MIT | Wrong performance class entirely; endpoint-switching code is worth reading |
| **RMK** | Rust + Embassy | ❌ | ❌ | ✅ (central + ≤8 peripherals) | Vial | Apache/MIT | Serial split is COBS/postcard at 115200 **with no CRC**; no AT32 HAL |
| **minipad** | RP2040 + Arduino | ❌ | ✅ | ❌ | CLI/desktop | GPL-3.0 | osu-pad scope; good calibration ideas (runtime-fitted LUT, 0.01 mm units) |
| **macrolev** | STM32F411 / ESP32-S3 | ❌ | ✅ | (rev2 WIP) | ✅ basic | — | Work in progress, no keymap config |
| **Keychron QMK fork** | QMK + HE | ❌ | ✅ | ❌ | Launcher (VIA-derived) | GPL-2.0 | Vendor fork, not a platform |
| **DeskHop** | 2×RP2040, UART | n/a | n/a | n/a (KVM) | — | GPL-3.0 | **The dual-host prior art.** Not a keyboard, but solves the state hygiene |

Notable absence: **there is no open wired split link running above ~1 kHz.** QMK's `split_common` is
four sequential turnarounds per transaction with a 20 ms timeout at 230400–460800 baud
(`serial_protocol.c:113-158`). Whatever the link protocol ends up being, it is new work.

What is worth stealing, concretely:

- **libhmk** — the AT32 clock/USB bring-up sequence, the descriptor layer (boot-compatible NKRO), the
  self-describing metadata pipeline, the DKS/null-bind data model, the forward-migration table.
- **QMK/ZMK/RMK** — all three ship **key positions, not keycodes**, over the split link. Copy that.
- **DeskHop** — per-host lock LEDs (`structs.h:97`), `release_all_keys()` on the half losing the host
  plus a message so the peer does the same (`handlers.c:385-394`), and merging local + remote state
  into one report before every send (`keyboard.c:203-216`).
- **ZMK** — clear reports on the *outgoing* endpoint *before* flipping the active one
  (`endpoints.c:482-489`), and keep `hid_indicators[]` per endpoint.
- **minipad** — 0.01 mm units and a runtime-fitted magnet-model LUT rather than a fixed curve.

---

## 2. Hard constraints (verified, and they kill options)

> Numbers below are for the **64-key full-size board: 32 keys per half**. Everything was re-derived
> after an adversarial audit; where the audit refuted an earlier figure the correction is marked.

### Silicon
- Cortex-M4F @ 216 MHz = **~27,000 cycles per 125 µs microframe**, but **six flash wait states with
  prefetch and no confirmed I-cache**. A branch-dense per-key pipeline running from flash costs
  **130–250 cycles/key**, not the 150 I first assumed. At 64 keys that is **44–74 µs, i.e. 35–59 % of a
  microframe for the key pipeline alone**; with USB, link, CRC, HID assembly and RGB prep, plan on
  **50–70 %**. Relocate the hot loop to SRAM (102 KB, zero wait state).
- **Flash program/erase halts the CPU** — and the board's part is better than I first wrote.
  **AT32F405xB (the RBT7 on this board) has 1 KB sectors, 6.6 ms typ / 8 ms max erase.** The 2 KB /
  13.2 ms / 16 ms figures are the **xC (256 KB)** part. So moving to RCT7 for flash space would
  *double* the worst-case single-erase stall. TPROG is 40 µs typ / 42 max per word either way.
  The RM attributes the erase stall to *flash read access*, so a RAM-resident writer with RAM-resident
  vectors *may* survive it — **unverified, first thing to measure.** Not a disconnect: the DWC2 core
  NAKs IN tokens in hardware without the CPU.
- Flash operations **require HICK enabled**.
- One 12-bit ADC. **TS = 1.5 cycles is legal here** — the 3.3 nF (47 nF after the respin) reservoir is
  the source the ADC sees, not the series resistor, so RAIN is only the mux R_ON, far under the 100 Ω
  that DS Table 51 demands at fADC = 28 MHz. That gives 14 cycles = **518.5 ns per conversion**, so a
  **32-key scan is 17.3 µs = 57.8 kHz** — 41 % faster than the 41 kHz I first quoted, and right at the
  2 MSPS ceiling. Four to seven complete scans fit in a microframe.
- Hardware ADC oversampler (ratio 2–256, shift 0–8) and 2×7 DMA channels with full DMAMUX — a
  **zero-CPU mux scan** (timer → DMA writes SEL → DMA reads ADC) is possible.

### USB
- 8 kHz ⇒ **HS enumeration + `bInterval = 1`** (USB 2.0 §9.6.6). FS cannot do better than 1 kHz, so the
  **J3** host port must be OTG_HS; the 12 MHz crystal is load-bearing (HS PHY reference is **HEXT only**,
  ±50 ppm).
- DWC2 spec 4.00a, internal DMA, EP0 + 7 IN + 7 OUT, 4096-byte FIFO SPRAM. A libhmk-shaped composite
  uses ~136 of 1024 words — endpoint *count* is the tighter budget.
- TinyUSB supports the part as `OPT_MCU_AT32F402_405`, including the AT32 quirk forcing `TRDT = 9`.
- **Windows applies 2^(Interval−1) only for Interval 1–5**, clamping 6–255 to 32 microframes (4 ms), so
  a "1 kHz mode" cannot be expressed by bInterval alone — rate-limit in firmware.
- bInterval is a *request* (§5.7.4); 8 kHz is never guaranteed by the descriptor alone.
- **TinyUSB's device stack is a single global instance** — J3 (HS) and J2 (FS) cannot both be device
  without patching it.

### The link (J1)
- **Open-drain half-duplex is a ≤500 kbaud transport** with 10 kΩ pull-ups into ~70 pF (~560 ns rise).
  Fine for the 115200 discovery phase; 2 Mbaud open-drain is impossible. Push-pull for steady state.
- `USART_BAUDR` is an **integer divider, DIV ≥ 16, fixed 16× oversampling** — no fractional divider, no
  OVER8. On APB1 (108 MHz) **UART7 tops out at 6.75 Mbaud**. **USART6 is on the same PC6/PC7 pins at
  MUX8 and is clocked from APB2 = 216 MHz**, giving an exact ladder of **13.5 (DIV 16), 12, 9, 8 and
  6 Mbaud**. The "6 Mbaud wall" is a UART7 fact, not a link fact. **Use USART6.**
- **Do not lower the 120 Ω series resistors.** *(This refutes my earlier recommendation.)* 120 Ω supports
  roughly 19 Mbaud over 1 m and 11 Mbaud over 2 m — both above USART6's 13.5 Mbaud ceiling — and going
  **below 47 Ω breaks an absolute maximum rating**. Nothing in any link plan needs them changed.
- A straight-through USB-C cable connects like pin to like pin, so **exactly one half must set
  `CTRL2.TRPSWAP`** — which is what role negotiation is for.

### Analog front end — corrected against the netlist and the audit
- As built: sensor VOUT → **30 pF** (C33…) → **5.1 kΩ** (R29–R34) → node with **3.3 nF** (C34…C54) →
  mux. τ = 16.8 µs, f_c = 9.46 kHz. *(Not the 330 Ω + 47 nF in my earlier notes — that was a plan.)*
- **The sensor's own 30 kHz pole cascades with the RC**, so the noise bandwidth is
  (π/2)·f_c·30k/(f_c+30k) = **12.0 kHz**, not 16.1.
- **Charge sharing is the biggest analog defect on the board as built.** The sampling node is
  C_ADC 13 pF max + parasitic ~7 pF + TMUX1574 C_ON 7.5–12 pF ≈ **28 pF**, giving a charge-share
  fraction of **28/(3300+28) = 0.84 %**. Against a worst neighbour-key step (KS-20 full travel =
  1.233 V) that is **12.9 LSB of signal-dependent inter-key crosstalk** — key A's reading moves with
  key B's position, so calibration cannot remove it. Sub-LSB needs **C_res ≥ 27 nF (Jade) / 43 nF
  (KS-20) ⇒ 47 nF is the right value.**
- **But the series resistor cannot go down to 330 Ω.** Worst-case node-to-rail transient current is
  0.95·VDD/R, and the TMR2615's **VOUT drive is a 1.5 mA absolute maximum** — 330 Ω gives 9.5 mA (6.3×
  over), 1 kΩ gives 3.1 mA. **R ≥ 2.09 kΩ; use 2.2 kΩ.**
- ⇒ **Respin target: 2.2 kΩ + 47 nF.** τ = 103 µs, f_c = 1.54 kHz. Sub-LSB crosstalk, drive-limit safe,
  and lower noise. The lag objection does not apply the way I stated it: at a rapid-trigger reversal the
  velocity is zero, so the one-pole error is **τ²·a, not τ·v** — at τ = 103 µs and a = 1000 m/s² that is
  **10.7 µm**. The τ·v term is transport *delay* (≈0.83 of a microframe), not position error.
- **Resolution, corrected.** Travel is per-family, not a flat 4 mm (Jade 3.5, KS-20 4.1, Apollo 3.1…).
  True *local* slope ratio rest→bottom-out is **6.89:1** for the Jade, spread **5.58:1 (Apollo) to
  9.13:1 (KS-20 — the worst, not the best)**. So the radial-offset geometry beats libhmk's 8.6:1 by
  ~20 %, not 3×. One ADC count is **11.15 µm at rest and 1.62 µm at bottom-out** (Jade).
- **Noise floor**: σ ≈ **2.9–3.4 LSB** ⇒ **25–36 µm rms at rest, ~5–9 µm at bottom-out.** The sensor,
  not the ADC, is the floor. Honest rapid-trigger floor is depth-dependent: with 4× ADC oversampling and
  a mild EMA, roughly **0.050 mm at 0.3 mm depth, 0.035 mm at 1 mm, 0.020 mm at 2 mm, 0.011 mm at 3 mm.**
- ⚠️ **Whether narrowing the filter helps at all is unmeasured.** If the sensor noise is white, dropping
  f_c from 9.5 kHz to 3 kHz buys ~1.8×. If it is 1/f-dominated — which MTJ/TMR elements usually are, and
  MDT publishes no corner — the same change buys **5 %**, and the lag is paid for nothing. **Measure the
  noise spectrum before committing to the filter respin.**
- **The F variant's output is inverted**: 95 % VDD → 5 % VDD as B goes −B → +B, so **counts decrease with
  travel** (subject to magnet polarity). Fix the sign convention in firmware and calibrate sign per key.
- Never reorder the ADC scan sequence at runtime — the residual charge-sharing error is deterministic
  only if the order is fixed.

## 3. Decision 1 — base platform

| Option | Verdict |
|---|---|
| **A. Artery BSP (BSD-3) + TinyUSB (MIT) + own CMake, libhmk as reference** | ✅ **Recommended.** Only combination that leaves licensing free. Board code is genuinely small (libhmk's is ~260 lines). 1–2 weeks to first enumerating 8 kHz HID with ADC scanning. |
| B. Fork libhmk and add split + RGB | 3–5 days to a blinking fork, then weeks unpicking single-half assumptions — and you ship GPL-3.0. Good source of validated constants (clock recipe, DWC2 quirks). |
| C. Artery's own `usbd` stack | Keep the clone for bisecting a TinyUSB bug. Not a base. |
| D. QMK / vial-qmk | 1 kHz, no analog, GPL-2.0. Not viable. |
| E. Rust + Embassy | No AT32 HAL exists. 3–5 months vs 2–3. The value is concentrated in the pure algorithm layer, which you can get by unit-testing C on the host instead. |

**Firmware update path** — the ROM DFU is on **OTGFS PA11/PA12 only** (Artery UM0001 §4.12 Table 12),
so the J3 host port cannot reach the factory bootloader. Netlist-verified: **J2** carries OTGFS1_D+/D− (PA11/PA12)
and **has no VBUS net at all**, so the ROM DFU is electrically reachable on J2 — but `GCCFG.VBUSIG` resets to 0
(VBUS not ignored), so whether the ROM bootloader enumerates there is **unverified and must be bench tested early.** It decides whether a custom HS bootloader is polish or the only unbrick path.
Primary user-facing update should be **app-resident self-flashing over the existing WebHID channel**
(A/B images) — it is the only mechanism that works driver-free on all three OSes over J3 *and* can
update the far half through the link. Note Chrome's WebUSB cannot claim the DfuSe interface on Windows
without WinUSB binding, so "browser WebDFU" is not the free win it looks like.

## 4. Decision 2 — scheduling model

| Option | Verdict |
|---|---|
| A. Bare superloop, DMA-only ISRs (libhmk) | Right for the first two weeks of bring-up. Wrong the moment a link or RGB exists. |
| **B. SOF-phased interrupt pipeline, strict NVIC hierarchy, lock-free SPSC** | ✅ **Recommended.** The only option where "meets 125 µs" is falsifiable with a number. |
| C. Superloop + hard-real-time ISR island | Reasonable staging point; the jitter tail is what fails on a 100-key split with RGB. |
| D. FreeRTOS / ChibiOS / Zephyr | Defensible under B for background work. Zephyr means porting the SoC — months. |
| E. Rust + Embassy | See above. |

The mechanism that makes B work: **OTG_HS routes its SOF pulse to TMR2's internal trigger IS3 in
hardware** (`TMR2_RMP[11:10]`, RM §21.2 "the SOF pulse can be output to device pins and timer 2"), and
with APB1 = /2 the TMR2 counter runs at 216 MHz (4.63 ns/tick). That gives a hardware microframe phase
reference for free: schedule the scan at a fixed offset from SOF, and *prove* the deadline by latching
`TMR2->CVAL` at report-arm time and keeping a max watermark.

> ⚠️ The obvious alternative — `PA4` / `OTGHS1_SOF` as a physical output pin — **is not available on
> this board: PA4 is already ADC1_IN4.** Use the internal TMR2 route. (This corrects the first pass of
> this analysis.)

Concrete priority hierarchy: USB > link UART (DMA + IDLE) > ADC DMA > LED DMA > housekeeping. All of
libhmk's ISRs sit at NVIC priority 0 — flat, no preemption — with 4 priority bits available. Also note
libhmk applies its EMA once per *superloop iteration* rather than per ADC sample, making its filter
time constant load-dependent, and `hid_send_reports()` busy-spins on `tud_hid_n_ready()` inside the
scan path. Do neither.

## 5. Decision 3 — where to cut the split pipeline

At **32 keys per half** the link budget is no longer the thing that decides this. UART 8N1 = 10 bit-times
per byte; payload + 4 B of header/seq/CRC-16:

| Frame | Bytes | @4 Mbaud | @6 Mbaud | @9 Mbaud (USART6) | @13.5 Mbaud |
|---|---|---|---|---|---|
| key positions only | 8 | 20 µs | 13 µs | 8.9 µs | 5.9 µs |
| **centi-mm distance ×32 (9-bit, 400 steps)** | **40** | **100 µs** | **67 µs** | **44 µs** | **30 µs** |
| 8-bit distance ×32 | 36 | 90 µs | 60 µs | 40 µs | 27 µs |
| 12-bit packed ×32 | 52 | ~~130 µs~~ **does not fit** | 87 µs | 58 µs | 39 µs |

Two corrections to my first pass. **12-bit at 4 Mbaud does not fit a 125 µs microframe** and also
destroys the inter-frame IDLE gap the framing depends on — strike it. And **8-bit is not "wrong"**: its
15.6 µm quantisation is 4.5 µm rms, which against a 25–36 µm sensor floor adds 1–8 % in quadrature. The
right unit is **centi-mm — 400 steps, 9 bits** — because it matches the pipeline's own unit, not because
8-bit is too coarse.

| Option | Verdict |
|---|---|
| QMK-shaped polled request/response | Only as the **115200 discovery/arbitration phase**, where collision-detect-by-readback is what you want. Never steady state. |
| **Semi-smart: peripheral does analog→key-position, centre does keymap/layers/HID** <br>✅ **still recommended** | 20 µs frames at 4 Mbaud, 8.9 µs at 9 Mbaud. Leaves each half functional alone, keeps layer resolution correct, leaves the microframe free for the 44–74 µs key pipeline. |
| Dumb peripheral: stream raw analog to one processing domain <br>❌ **refuted on audit** | It does **not** delete cross-half coordination. It *adds* ~1.9 KB of dual-homed config — in dual-host either half can become the owner, so both need the full config anyway — of which **~640 B is live-mutating calibration** needing a new sync path. And it is arithmetically inconsistent with the CPU budget: a 100 µs frame leaves ~25 µs of the microframe, against a 44–74 µs key pipeline. |
| Fully smart: peripheral emits keycodes | ❌ Needs the centre's layer state fed back a frame late; emits the wrong keycode across a layer change. The reason QMK, ZMK and RMK all ship positions. |
| Event-driven, COBS (RMK/ZMK shape) | ❌ Wrong premise — an 8 kHz board exists for bounded, constant latency. |

**Frame shape:** fixed-size, word-aligned, full-duplex isochronous, into a circular DMA ring, delimited by
the USART **IDLE-line interrupt** — periodic traffic with a guaranteed inter-frame gap resynchronises for
free, so no COBS and no length-prefix search. CRC-16/CCITT on the hardware CRC unit costs ~111 ns for
24 bytes. Sequence numbers for drop detection; release all keys on link loss.

**Run USART6 at 9 Mbaud.** It is an exact integer divisor of APB2, the 120 Ω resistors already support
it, and it puts even a full centi-mm stream at 36 % duty — which keeps the dumb-peripheral option open as
a later experiment without a board change.

## 6. Decision 4 — roles, and the built-in KVM

**Replace master/slave with symmetric peers plus a transferable "input-owner" token.** Master/slave
survives *only* as a power concept — who drives PC13/AP22653 into J1 — which `/LM_ST` already tells
you. Conflating power-master with input-owner is what makes topology (b) unimplementable.

Each half owns a full, permanently-enumerated USB HID stack. The token says which of the (up to two)
attached hosts currently receives the merged keyboard. Transfer it with a two-phase commit.

Topologies to auto-detect:

| # | Topology | Detection | Behaviour |
|---|---|---|---|
| a | PC → half A → half B | A has J3 VBUS, B is link-powered (`/LM_ST`) | A sources 5 V, A owns input, B tunnels positions |
| a′ | Same, roles reversed | symmetric | identical code path |
| b | PC1 → A ↔ B ← PC2 | link hail answered *and* both have own VBUS | **nobody sources 5 V**; token chooses the active host; the non-owner half tunnels |
| c | Half alone | hail times out, no peer | standalone keyboard |
| d | Link lost mid-session | frame timeout | release all keys, fall back to Independent mode |

State hygiene on a token switch — all of these are per-host, not global:
- Zero report on the **outgoing** host *before* flipping the owner variable (ZMK's ordering).
- A zero keyboard report is not enough: clear consumer, system control, mouse buttons, and zero every
  gamepad axis/trigger.
- **Change-suppression caches are per-host.** libhmk's file-scope statics in `hid_send_hid_report()`
  would suppress a needed report after a switch.
- Lock LEDs (caps/num) diverge per host — keep `leds_desired[NUM_HOSTS]`.
- Boot protocol vs report protocol, and SET_IDLE, differ per host. PC1 may be in a BIOS at 6KRO while
  PC2 is at NKRO. Keep abstract key state and *render* per host.
- **Suspend is not disconnect.** On disconnect the host drops your keys; on suspend it freezes its view
  of them. Re-send state on resume.

Latency: the remote hand costs roughly **+125 µs mean / +185 µs worst** versus the local hand — on top
of a ~125 µs local path, against a ~1 ms end-to-end budget. Irrelevant. The genuinely new problem in
topology (b) is that the two halves are clocked by **two independent SOF domains** (USB 2.0 Table 7-13:
125 µs ±500 ppm each, so up to ~1000 ppm relative drift), so the non-owner cannot phase-align its link
TX to the owner's microframe. Fix with a link sync beacon and a slow phase servo, or accept one extra
frame of jitter.

Electrical consequence to accept, not fix: in topology (b) the split cable **hard-bonds the two hosts'
USB grounds**, because J1 must carry GND for the 5 V pass-through in topology (a). DeskHop puts an
ISO7721 across its link precisely to avoid this — but its own FAQ says running without the isolator is
fine. Document it; don't pretend firmware can solve it.

Also unaddressed so far and worth budgeting: in topology (a) **one 500 mA host port powers both halves**
— two MCUs, two HS PHYs, and up to 14 SK6812s.

## 7. Decision 5 — config transport and the web UI

| Option | Verdict |
|---|---|
| A1. VIA protocol, upstreamed to usevia.app | ❌ **Blocked, permanently.** Official support requires your source merged into `qmk/qmk_firmware` *and* a `keymaps/via` keymap merged into `the-via/qmk_userspace_via`. Not possible for from-scratch firmware. |
| A2. VIA protocol + Design-tab side-load | Users must enable a hidden tab, dismiss a warning, and side-load JSON that lives only in that browser profile's IndexedDB. Secondary interface at best. |
| A3. Fork the-via/app (GPL-3.0), self-host | 3–5 weeks + a permanent merge tax. |
| B. Vial protocol | Genuinely implementable: discovery is the magic string `vial:f64c2b3c` in the USB serial descriptor, **no registry at all**, definition ships LZMA-compressed in firmware. But Vial has **no vocabulary for actuation / rapid trigger / DKS**, and vial.rocks is a PyQt5 app compiled to WASM (~17.8 MB first load). |
| **C. Own raw-HID protocol + own web app** | ✅ **Recommended.** hmkconf is a working ~11.5k-LOC proof this is weeks, not months. |
| D. C plus a thin VIA-compatible shim on a second interface | Cheap insurance *after* C ships. Never let it shape the primary data model. |
| E. ZMK Studio model (CDC-ACM + protobuf over Web Serial) | Only if Firefox support is a hard requirement (Web Serial shipped in Firefox 151; WebHID never will). |

**VIA's ceiling, concretely:** 22 command IDs, 32-byte reports on usage page 0xFF60 / usage 0x61,
buffer ops chunked at 28 bytes, and custom menus that can express exactly **eight scalar control types
carrying 1–2 bytes each**, in a nested list with no key-position awareness. Per-key actuation for 69
keys means 69×N enumerated sliders in a flat list, and a DKS entry (4 keycodes + 4 action bitmaps +
bottom-out point = 9 bytes) **does not fit in a single control at all.** This is why every HE vendor
ships their own configurator.

**The best idea to steal from libhmk:** the device describes itself. The whole `keyboard.json` is
gzipped at build time into a `KEYBOARD_METADATA[]` array in flash (~950 B for a 69-key board) and
streamed to the browser 59 bytes at a time, where native `DecompressionStream("gzip")` inflates it.
One web app then serves every board with zero per-board code — including the 6-key pad and the
full-size split.

WebHID mechanics that will bite:
- **Chromium-only** (no Firefox, no Safari). Same limitation as usevia.app and Vial; state it up front.
- **`HIDDevice` has no serial number** — add a `GET_SERIAL` command (the 96-bit UID is at `0x1FFFF7E8`)
  and key app state on VID-PID-serial.
- **Do not put config reports on the keyboard's collection.** Chromium's `IsAlwaysProtected()` strips
  all reports on usage page 0x07 and recurses into child collections. Separate USB interface, vendor
  usage page, own non-zero report ID.
- macOS additionally requires **Input Monitoring** permission to open a HID device containing keyboard
  usages — another reason for the separate interface.
- The blocklist is **remotely extensible** via a Finch param, so "check `hid_blocklist.cc`" is
  necessary but not sufficient when picking a VID/PID.
- Two physically separate halves need **two user gestures** (two `requestDevice` calls). Design the UI
  for that; don't copy hmkconf's `collections[0]`-only filter or vial-web's `devices.length != 1` bail.

**Effort, honestly:** firmware protocol + metadata pipeline 1–2 weeks; web app functional-but-plain
3–4 weeks; genuine usevia.app-class polish (layout renderer, drag/shift-click key selection, keycode
picker, per-key analog editor, DKS editor, macro editor, live depth view, RGB editor, profiles,
split/KVM UI, dark mode) **6–10 weeks** for one experienced dev. Also note hmkconf's live analog view
runs at ~13–14 Hz on a 69-key board because of a `setInterval(…, 10)` dispatch plus a serialized
queue — the transport is not the limit, its client is. Do better with a firmware-pushed telemetry
report.

## 8. Analog pipeline and feature semantics

Baseline: **clean-room centi-mm pipeline with per-key piecewise-linear calibration** (2–3 weeks, plus
1–2 weeks of capture/fit tooling). libhmk's `uint8` distance in [0,255] is the thing not to inherit —
its own fitted LUT has an 8.6:1 sensitivity ratio between bottom-out and rest.

- **Filter**: EMA per *ADC sample*, not per loop iteration. Add ISR-decimated hardware oversampling
  (3–5 days) — measure the noise floor first; if σ < 0.5 LSB keep N small.
- **Calibration**: two-sided continuous (minipad's approach) with an inward deadzone, plus a saved
  bottom-out threshold. Guard the failure mode of a key held at power-on.
- **Curve**: runtime-fitted LUT from a magnet model, or per-key PWL. PWL is the only shape flexible
  enough for a sensor whose transfer function you have not characterised yet.
- **Hysteresis**: libhmk has *none* in non-RT mode (`is_pressed = distance >= actuation_point`) and its
  non-continuous RT reset point equals the actuation point. Add real hysteresis.
- **Velocity**: compute it (free, and the configurator wants it for the live graph) but ship prediction
  **off** by default and never make RT thresholds velocity-adaptive by default.

Feature semantics to pin down in the data model up front: RT down/up sensitivity + continuous mode +
reset point + direction-reversal behaviour; actuation point + hysteresis; Rappy Snappy (deeper wins,
both only when both bottomed out); SOCD modes (last-wins / first-wins / neutral / absolute — plus the
tournament-legality flag); DKS (4 keycodes × 4 action phases + bottom-out point); mod-tap with QMK's
vocabulary (tapping term, permissive hold, hold-on-other-key-press); toggle key; layers + one-shot;
macros; analog curves (deadzone, response curve, per-key axis assignment).

**Split config model — the decision that determines whether the project works:** two-plane processing
with symmetric mirrored config. The peripheral owns calibration, actuation point and rapid trigger for
*its* keys, so per-key analog config must be mirrored to the half that owns the key, with an
epoch/generation scheme. Nail down the epoch format and handedness storage **before** writing split
code. In topology (b) this gains a genuine distributed-systems problem with no prior art: two browsers,
one per host, can both write config. Define a single owner for shared state and a rejoin policy —
last-writer-wins on a counter will silently discard one PC's edits.

Config writes to the *peer* traverse the same 4 Mbaud link that carries the 8 kHz key path. Budget it:
rate-limit config writes, and never let a config write or a live-telemetry stream perturb the key path.

## 9. Storage and the flash stall

1. **Now (dev pad):** RAM-mirrored blob + word write log (libhmk's scheme, hardened with A/B banking
   and a deferral scheduler). Correct for anything under ~8 KB.
2. **Full-size split:** banked image + hot-field journal, no full RAM mirror. libhmk's wear-levelling
   amplification is 8 flash bytes per 6 payload bytes, dropping to 5 payload bytes once
   `WL_VIRTUAL_SIZE` passes 8192 — which a 100-key, 4-profile config will.

Split config into a **hot** part (actuation points, RT thresholds — read every microframe) and a
**cold** part (keymaps, macros, RGB). Never write the hot part while the keyboard is live: stage into a
RAM shadow and apply atomically by pointer swap. Bound every critical section to a countable number of
instructions.

## 10. Bring-up plan (and the measurements that gate each step)

**Phase 0 — silicon truth (do this before writing architecture code).**
1. Does the ROM DFU enumerate on **J2** (FS port, no VBUS net)? (Decides the whole recovery story.)
2. `GPIO toggle + scope` around `flash_sector_erase()`: real stall duration, and **does a RAM-resident
   erase routine with RAM-resident vectors keep ISRs alive?**
3. Does `GINTSTS.SOF` fire every 125 µs in HS device mode? Count SOFs against a free-running timer for
   one second. Confirm the TMR2 IS3 SOF trigger route works.
4. Real mux settling on this board: drive SEL, sample the same channel at 1 µs intervals, find where it
   stops moving. (Expect ~350 ns, not libhmk's 20 µs.)
5. ADC swing in raw counts over the full 4 mm for the TMR2615F as mounted, and the noise floor σ —
   with the SK6812 chain idle *and* at full white.
6. VDD droop and rest-value shift when the AP22653 is enabled (PC13). TMR sensors are ratiometric, so
   this moves every key.
7. Link eye at 4 and 6 Mbaud at the longest cable you intend to ship, with the 120 Ω resistors (R3/R7) in place.

**Phase 1 — superloop bring-up (libhmk shape).** USB HS enumerating at bInterval=1, mux/ADC producing
sane numbers, one key registering, raw-HID config interface answering a version command.

**Phase 2 — the real skeleton.** SOF-phased pipeline, NVIC hierarchy, deadline watermark instrumentation,
role vtable + runtime role dispatch, host-side unit tests of the pure algorithm layer driven by recorded
ADC traces.

**Phase 3 — link.** Discovery/arbitration at 115200 open-drain, then push-pull isochronous frames.
Then the peer/token model and topology detection.

**Phase 4 — configurator.** Metadata pipeline first (self-describing device), then the app.

## 11. Licensing

- **Artery BSP** BSD-3-Clause, **TinyUSB** MIT → a closed or permissively-licensed product is fine.
- **libhmk, minipad, DeskHop, VIA app** are GPL-3.0; **QMK/vial-qmk** GPL-2.0. Reading them and copying
  an *approach* is fine; copying code makes the firmware GPL. Keep libhmk as a reference checkout that
  no source file is ever pasted out of, and note in the repo that this was deliberate.
- Self-hosting any GPL web app carries the source-offer obligation, and WebHID requires a secure
  context (HTTPS or localhost) either way.

## 11b. Netlist-verified board map (exported with the kicad-curved CLI, 2026-08-24)

Superseding earlier notes, which had several refs and values wrong:

| Item | Truth |
|---|---|
| **J3** | Main **USB HS host port** — OTGHS1_D+/D−, VBUS_HOST |
| **J2** | **USB FS port, data only** — OTGFS1_D+/D− (PA11/PA12), **no VBUS net**. The only path to the ROM DFU |
| **J1** | Inter-board link — VBUS_B; D+ (A6/B6) → R7 120 Ω → `/UART7_TX` = **PC7**; D− (A7/B7) → R3 120 Ω → `/UART7_RX` = **PC6**; 10 kΩ pull-ups R1/R2; SRV05-4A (U1) clamps |
| J5 / J7 | Break out **9 spare ADC channels** (IN4–IN15) — useful for prototyping a 32-key scan on the dev pad |
| J6 / J8 / J9 | I²C / SPI / IO expansion, and SWD + USART1 debug |
| Sensors | **U13, U15, U17, U19, U21, U23** — TMR2615F-AAC-1.500-500 |
| Mux | **U11** TMUX1574RSVR; SEL = PB2; outputs → ADC1_IN0..IN3 |
| LEDs | **U12** (sacrificial, on the 1N4148-dropped rail) then U14…U24 on +5 V — 7 total |
| **Analog rail** | +5 V → **U4 TPS7A4700** (ultra-low-noise LDO) → **+3.3VA** → all six sensors **and** the mux; and → **L1** (GZ1608D601TF, 600 Ω bead) → MCU **VDDA**. Digital +3.3 V is a separate XC6206 (U8). |
| **Per-key filter** | sensor VOUT → **30 pF** (C33…) to GND → **5.1 kΩ** (R29–R34) → node → **3.3 nF** (C34…C54) → mux input. τ = 16.8 µs, f_c = 9.46 kHz |
| VBUS_B sense | 10 k/10 k divider (R4/R9) = ÷2 → 5.1 kΩ (R8) + 3.3 nF (C10) → mux S4A → ADC1_IN0 |
| Hotswap sockets | SW3–SW8 pin 2 sits **directly on the analog node**, pin 1 on GND — a mechanical switch shorts the sensor node to ground (hybrid mech/magnetic). Firmware must read a hard 0 as "mechanical key closed", and the socket adds C and leakage to the analog node. |

Because the sensors and the ADC reference are the same node at DC (both `+3.3VA`, separated only by
L1's sub-ohm DCR), **ratiometric cancellation genuinely works here** — supply variation cancels between
the sensor's output and the ADC's full scale. Only >MHz disturbances see the bead, and the TPS7A4700
makes those negligible. This is a good design; do not "fix" it.

## 12. Board-level findings and respin candidates

Netlist- and audit-verified. Items 1–4 are firmware-visible facts; 5–10 are respin candidates.

| # | Finding | Action |
|---|---|---|
| 1 | **PC6 is the native TX and PC7 the native RX**; the schematic names them the other way (`/UART7_TX` on PC7). | Harmless — one half must `TRPSWAP` anyway — but the "default" half is the swapped one. Relabel. |
| 2 | **USART6 shares PC6/PC7 and is APB2-clocked** — exact ladder 13.5 / 12 / 9 / 8 / 6 Mbaud vs UART7's 6.75 ceiling. | Use USART6. Target **9 Mbaud**. No board change. |
| 3 | **PA4 is ADC1_IN4 here**, so the `OTGHS1_SOF` pin output is unavailable. | Use the internal **TMR2 IS3** SOF route instead. |
| 4 | **The F-variant sensor output is inverted** (95 % → 5 % VDD as B increases). | Counts *decrease* with travel. Calibrate sign per key; never mix magnet polarities. |
| 5 | **Charge sharing: 28 pF sampling node into 3.3 nF = 0.84 % ⇒ up to 12.9 LSB of inter-key crosstalk.** | **Respin the filter to 2.2 kΩ + 47 nF.** 47 nF is the minimum for sub-LSB; 2.2 kΩ is the minimum that respects the sensor's 1.5 mA VOUT absolute max. |
| 6 | **Do not lower the 120 Ω link resistors.** They support ~19 Mbaud over 1 m; below 47 Ω breaks an absolute maximum rating. | Leave them. *(Refutes my earlier suggestion.)* |
| 7 | **The 10 kΩ link pull-ups straddle the USB connect threshold.** With RPD 14.25–15.75 kΩ and 10 kΩ ±1 % from 3.3 V ±2 %, J1 sits at **1.893–2.067 V** against VIH = 2.0 V — the governing threshold for connect detection is VIH, not VIHZ. | **Raise the pull-ups** (or gate them) so plugging J1 into a PC cannot read as an attach. |
| 8 | **Half the converter range is structurally unused.** The field is unipolar (34 → ~190 Gs, never crossing zero) but the part is ordered at the mid-rail **500 mV/V** offset, so only 300 Gs of the ±300 Gs window is reachable. Sensitivity **and** offset are custom-orderable (0.500–7.000 mV/V/Gs, 300–700 mV/V). | Order the offset at the end the field moves *away* from. At **700 mV/V** the usable window becomes 650/SEN Gs = **433 Gs**. Trading that headroom for sensitivity: `1.800-700` → +20 % counts/Gs; restricted to the measured Gateron families (≤191 Gs), `2.500-700` → **+67 % counts/Gs**. Free — it is a trim code, not a different part. |
| 9 | **KS-20 / Lekker is at 95 % of the rail already** (286 of 300 Gs) and is a ±25 % *fitted* family — at +25 % it hard-clips before bottom-out. KS-37B and Everglide clip at +14 %. | Either bench-measure those families before promising support, or fix it with the offset change in #8. |
| 10 | **LED budget**: the SK6812MINI-E fitted is the **12 mA/channel** part → 36.65 mA/LED full white → **1.173 A for 32 LEDs**, not the 1.9 A I first quoted. The AP22653's *guaranteed* limit is **ILIM_min = 1.044 A** (1.16 A is typical). | The slave half **trips at ~78 % white**. Firmware brightness cap is mandatory, and in the daisy-chain topology a single 500 mA host port feeds both halves. |
| 11 | **`OTGHS1_R` needs an external 12 kΩ ±1 %**; the **12 MHz crystal needs ±50 ppm total**. | Verify both — classic bring-up failures. |
| 12 | **Hotswap sockets SW3–SW8 pin 2 sit on the analog node**, pin 1 on GND. | A mechanical switch shorts the sensor node. Firmware must read a hard rail as "mechanical key closed", and the socket adds C and leakage. |
| 13 | **SK6812 over SPI + DMA works** (3.375 MHz, 4-bit symbols → 296/889/593/593 ns, in spec). | Divider granularity does not force timer-PWM+DMA. SPI frees a timer. |

### Balancing the filter against the mechanical-switch short

The hotswap sockets (SW3–SW8 pin 2) sit on the same filtered node the mux reads, with pin 1 on GND — so
a mechanical switch shorts the sensor node. That looks like it fights the filter choice. It does not:

**The mech short *is* the constraint that sets R.** The "worst-case node-to-rail current" that forces
R ≥ 2.09 kΩ is exactly a mechanical switch holding the node at ground while the sensor drives into it —
and it is a *continuous* condition while the key is held, not a transient. The TMR2615's **1.5 mA V_OUT
drive is an absolute maximum with no short-circuit protection** (datasheet §4). So 2.2 kΩ is not a
compromise against the hybrid-switch feature; it is the number that feature demands. At 2.2 kΩ the
sensor sources 1.42 mA worst case.

**What genuinely conflicts is the capacitor discharging through the socket contact.** 47 nF at 1.65 V
into a ~0.1 Ω contact is a ~5 A ring (64 nJ) — a hot-switch event on a signal-level gold contact, every
single keypress. Small energy, but peak current is what pits contacts.

**Fix: one 100 Ω resistor in series with each hotswap socket.** It sits in series with R_main for the DC
path, so sensor current is unchanged, but it limits the discharge ~300×. It must stay small enough that
a closed mechanical key reads *below* the deepest legitimate analog value — which is **0.234 V**, set by
KS-20/Lekker, the highest-flux family:

| R_sock | mech level at rest | margin below deepest analog | discharge peak |
|---|---|---|---|
| 0 (as built) | 0.000 V | 0.234 V | **~5 A** |
| **100 Ω** ✅ | 0.075 V | 0.159 V | 15.8 mA |
| 150 Ω | 0.113 V | 0.122 V | 10.2 mA |
| 300 Ω | 0.225 V | 0.009 V — **ambiguous** | 4.7 mA |

Release latency is a non-issue: from the 0.075 V held level the node recrosses 0.30 V in **16 µs** and
0.60 V in 42 µs — a fraction of a microframe.

Rejected alternatives: moving the socket to the sensor-side node (only 30 pF there) shorts V_OUT
directly and blows the 1.5 mA absolute max; shrinking C to cut discharge energy brings back 12.9 LSB of
crosstalk; a large R_sock puts the mech level inside the analog band.

**Firmware**: a closed mechanical key now reads a fixed low level rather than 0, so put the
"mechanical key closed" threshold between the mech level and the analog floor (~0.15 V ≈ 186 counts) —
and express it as a fraction of the key's rest value, since everything here is ratiometric.

**Cost**: one extra 0402 per key — 64 on the full board.

### Why the big C is right regardless

At fixed τ, R and C trade directly against each other, and the crosstalk follows C:

| R_main | C | τ | crosstalk (1.23 V neighbour step) |
|---|---|---|---|
| 2.2 kΩ | 47 nF | 103 µs | **0.9 LSB** |
| 3.3 kΩ | 33 nF | 109 µs | 1.3 LSB |
| 4.7 kΩ | 22 nF | 103 µs | 1.9 LSB |

Big C also lowers the noise bandwidth, and its only cost is transport *delay* — a fixed latency, not a
position error, and ~10 % of a 1 ms end-to-end budget. R can be raised freely on noise grounds (Johnson
noise at 3.3 kΩ over 12 kHz is 0.8 µV against 1.5 mV of sensor noise), so the choice is driven entirely
by crosstalk and by the sensor's drive limit. **2.2 kΩ + 47 nF + 100 Ω socket resistor** satisfies all
three. If the noise spectrum comes back white, 2.2 kΩ + 100 nF (τ = 220 µs, 0.4 LSB) is defensible.

### The measurement that gates the filter respin

**Is the sensor's noise white or 1/f?** Under white, narrowing f_c from 9.46 kHz to 1.54 kHz cuts σ by
~2.5×. Under 1/f — which MTJ elements usually are, and MDT publishes no corner — it buys ~5 %, and you
pay 103 µs of transport delay for nothing. Capture the noise spectrum on one channel with the key at
rest before ordering the respin. Note also that "BW = 5 kHz" in the noise spec is itself ambiguous
(brickwall vs single pole swings the extrapolation by 20 %), and 10 mV_PP is a **max**, not a typ.

---

## 13. Link bring-up: what the two boards actually measured (2026-08-28)

Both dev pads, self-powered from their own J3, nothing in J1. Run with
`tools/link.py --probe`, which sweeps the GPIO MUX electrically instead of trusting a table.

| Claim | Status |
|---|---|
| PC6 = USART6_TX, PC7 = USART6_RX | **Confirmed** — DS_AT32F405_402 V2.03, pins 37/38 of the 64-pin part |
| USART6 is at **MUX8** on those pins | **Confirmed by measurement.** The datasheet does not print MUX indices. Sweeping 0–15 while shifting `0x00`, only MUX8 modulated the pad (78–80 % low against the 90 % the framing implies). MUX15 reads a flat 100 % — an undriven pad, not a signal |
| R1/R2 10k pull-ups and R3/R7 120 Ω fitted and working | **Confirmed** — both pads pull to 0 open-drain and return high through the fitted resistor, on both boards |
| **The link runs** | **Confirmed.** Two boards, straight-through USB-C on J1. Full-duplex push-pull with TRPSWAP on one half is clean at every rung of the ladder to **13.5 Mbaud** — the USART's hard ceiling — in both directions. Soaks of 163,840 bytes at 9 Mbaud and at 13.5 Mbaud: zero corrupt, zero missing, no error flag. The 9 Mbaud target has comfortable margin and 13.5 is available if it is ever wanted |
| Open-drain half-duplex is a ≤500 kbaud transport | **Confirmed, to the rung.** Clean at 115200 and at 500 kbaud; at 1 Mbaud every byte arrives and 1020 of 1024 are corrupt with FERR set. That is the 10k-into-70pF rise, and it is a genuine *wire* limit, not a CPU one. Discovery at 115200 has 4x margin |
| Single-wire half-duplex echoes to its own receiver | **REFUTED.** RM 12.2 says "TX and SW_RX are interconnected inside the USART", but with `SLBEN=1 TEN=1 REN=1` neither board raised RDBF on 256 bytes, at 115200 *and* 9 Mbaud, open-drain *and* push-pull, with `STS = 0x00C0` (TDC+TDBE, no error). The transmitter is provably running. **Consequence: the discovery bus cannot be validated by one board — it needs a peer or a TP1–TP2 jumper.** The discovery design itself is unaffected |
| `/LM_ST` (PB12) means "we are link-powered" | **Confirmed, and the earlier alarm was a bad cable.** LM66100 ST is Hi-Z (so R5 pulls it high) while that ideal diode conducts, and pulled low while it blocks — so high really does mean "our +5 V is coming in through J1". The two boards disagreed because the first cable tried was carrying 5 V into J1, powering one of them through it. With a proper cable both read low, both PC13 read low, and nothing sources VBUS. The AP22653 fitted is the **active-high** enable variant (AP22652 is the active-low one), so R10's 10k pulldown does hold it off |
| ~~`/LM_ST` is driven, not floating~~ | **Over-claimed and withdrawn.** R5's 10k pull-up beats the MCU's ~40k internal pull-down, so a Hi-Z ST reads high under both internal pulls. Only the LOW result was ever conclusive |
| ~~Old note~~ | **Open, and now suspicious.** Read with the internal pull-up and then the pull-down, the pin is genuinely *driven* on both boards — and the two boards disagree. In identical states (J3-powered, J1 empty) board `…5C13` drives it **high** and board `…5113` drives it **low**. U2 is an LM66100 ideal diode on VBUS_B; U7 is its twin on VBUS_HOST; the pair ORs into +5V. Until this is explained, **no power decision may key off PB12** |

### The identity problem, and why it came first

Two halves run one image. Before this, both enumerated as `1209:0001` serial `000000000001`, and in DFU
every AT32 reports the serial `AT32` — so no host tool could name a board, and no log line was
attributable. Fixed by taking the USB serial from the 96-bit UID at `0x1FFFF7E8` (Artery's device
electronic signature, the address their own USB middleware uses) and by adding it, plus a 16-bit
`uid_tag`, to `RSP_INFO`.

The two boards on the bench are adjacent dice: `E75C3040 00801605 05875C13` and
`E85C3040 00801605 05875113` differ in two of twelve bytes. A truncation would collide; FNV-1a over all
twelve gives 0x436A and 0xF885. That tag is the arbitration tie-break.

### Is 13.5 Mbaud enough for an 8 kHz split, and can it go higher?

**Higher is not available.** RM 12.6.1 is explicit: the receiver splits each bit into 16 oversample
steps, "so the data bit width should not be less than 16 PCLK periods, that is, the DIV value must be
greater than or equal to 16." No fractional divider, no OVER8. APB2 is already at the part's 216 MHz
ceiling, so **13.5 Mbaud is a hard wall on USART6**, not a tuning knob.

**Enough is not close.** Measured sustained rate with DMA on both ends (98.8–99.6 % of line rate, so
the wire really is saturated), against a 72-byte frame — 32 keys x u16 position, header, 32-bit mech
bitmap, CRC-16:

| Baud | Sustained | 72 B frame | Share of a 125 µs microframe |
|---|---|---|---|
| 8 Mbaud | 7.94 Mb/s | 90.7 µs | 72.5 % |
| 9 Mbaud | 8.93 Mb/s | 80.7 µs | 64.5 % |
| 12 Mbaud | 11.87 Mb/s | 60.7 µs | 48.5 % |
| **13.5 Mbaud** | **13.33 Mb/s** | **54.0 µs** | **43.2 %** |

The link is full duplex, so the return path — layer state, lock LEDs per host, the input-owner token,
RGB sync, config traffic — rides the other wire concurrently and costs the forward path nothing.

Packing positions to 12 bits (32 keys in 48 bytes) drops the frame to 56 B = 34 % at 13.5 Mbaud, if
that headroom is ever wanted. It is not needed at 32 keys per half.

#### Cable length: measured, 1.5 m and 3 m

409,600 bytes per rate, 100 runs of 4096 alternating direction, over a **3 m** USB-C cable — roughly
three times any split cable that would ship, chosen as a deliberate worst case.

| Baud | 3 m result |
|---|---|
| 9 Mbaud | 409,600 B, 0 corrupt, 0 missing, **0/100 runs raised NERR** |
| 12 Mbaud | 409,600 B, 0 corrupt, 0 missing, **0/100 runs raised NERR** |
| 13.5 Mbaud | **61/100 runs raised NERR**, one run desynced entirely and lost 4093 B |

At 1.5 m all three rungs including 13.5 Mbaud were clean.

**NERR is the leading indicator, and it is free.** The receiver oversamples 16x and majority-votes
samples 8/9/10 of each bit; NERR means those three disagreed. At 13.5 Mbaud over 3 m it fired on 61 %
of runs while **still recovering every byte correctly** — zero corruption. So the eye closes visibly
in the noise flag well before any data is lost. It is symmetric too: 27–39 of 50 on every combination
of direction and of which wire carries the traffic, so this is cable length, not a board or a net.

The a-priori estimate in §2 — 120 Ω supports ~19 Mbaud over 1 m and ~11 Mbaud over 2 m — turns out to
have been slightly conservative and essentially right.

The **open-drain discovery bus is unchanged between 1.5 m and 3 m**: clean at 115200 and 500 kbaud,
1020 of 1024 bytes corrupt with FERR at 1 Mbaud. That limit is the 10 kΩ pull-ups into the node
capacitance, not the cable, which is why length does not move it.

**Recommendation: run at 12 Mbaud, and negotiate it.** 12 is clean at three times the realistic cable
length with the leading-indicator flag never once firing, and it costs 48.5 % of a microframe against
64.5 % at 9. DIV = 18 also keeps one rung off the divider's DIV ≥ 16 floor, where the oversampler has
exactly one PCLK per step.

Then make it adaptive, because NERR makes that nearly free: after the 115200 handshake, bring the link
up at 12 Mbaud and watch NERR and the CRC-16 failure rate. If either rises, step down 9 → 8 → 6. The
link then self-tunes to whatever cable the user actually plugged in, and it downshifts *before* frames
start being lost rather than after.

If a future design genuinely needs more, the lever is not the baud rate:
- **J1's SBU1 (A8) and SBU2 (B8) are unconnected** on this board. Wiring them on the full-size respin
  gives two more conductors — enough for a real SPI, or better, a **hardware frame-sync strobe**, which
  would retire the software phase servo that topology (b)'s two independent SOF domains otherwise need.
- Push more feature computation into each half so the link carries events rather than positions. That
  is a ~10x payload cut, and the reason it was rejected is that SOCD, Rappy Snappy and Snappy Tappy are
  cross-key features whose two keys may land on opposite halves.

### Two traps that cost real time, recorded so they are not paid twice

**A charge-only USB-C cable is indistinguishable from a dead peripheral.** The first cable tried
carried VBUS and GND but neither D+ nor D−. Every link test failed identically with no error flag, and
it also fed 5 V into J1, which lit one board's `/LM_ST` and started a hunt for a phantom power fault.
`tools/link.py --continuity` now settles this in two seconds with no USART involved at all: one board
holds a link pad low open-drain, the other reads its own pad. **Run it before believing any link
failure.**

**Receive by DMA, never by polling.** A CPU loop reading RDBF gets preempted by the 16 kHz ADC/mux ISR
for longer than a byte time above roughly 8 Mbaud. The USART overruns, the lost byte shifts the
expected value for every byte after it, and reading `DT` clears `ROERR` on the way past — so the
symptom is *almost every byte arriving, almost every byte wrong, and no error flag*, which reads
exactly like a bad cable. With DMA the same hardware is clean to 13.5 Mbaud. `DMA1_CHANNEL1` is the ADC
ring; the link takes `DMA1_CHANNEL2` with `DMAMUX_DMAREQ_ID_USART6_RX`.

### Order of the remaining link work

1. ~~Connect J1↔J1 and prove the wire.~~ **Done.** Safe as built: firmware never drives PC13, so
   neither AP22653 sources 5 V and each board keeps its own J3 supply. Electrically this is topology (b).
2. ~~Half-duplex open-drain at 115200 on D−, both halves unswapped; full-duplex with TRPSWAP.~~ **Done,
   both directions, to 13.5 Mbaud.**
3. **Next:** UID-tag arbitration over the 115200 discovery bus — hail, exchange tags, loser sets
   `CTRL2.TRPSWAP`, both switch to push-pull. Tie-break is `uid_tag()`, which is FNV-1a over the whole
   96-bit UID precisely because two boards off one reel differ in only a couple of bytes.
4. Framing: fixed-size isochronous frames, DMA + IDLE-line, CRC-16, and the SOF phase servo.
5. Then the peer/token model and topology detection.

---

## 14. Updating the half that has no host (topology a)

In `PC → half A → half B`, half B has no USB connection to anything. Two questions fall out of that:
where config lives, and how firmware gets there.

### Config: mirror it, do not centralise it

The instinct is "keymap lives on the centre". That is wrong here, because **either half can be the
centre** — flip the cable and the roles swap, and in topology (b) the input-owner token moves at
runtime. Config stored only on the centre would evaporate the first time someone reverses the cable.

So the store has two regions with different rules:

| Region | Contents | Rule |
|---|---|---|
| **Shared** | keymap, layers, actuation points, rapid trigger, Rappy Snappy, Snappy Tappy, DKS, toggle keys, RGB | **mirrored on both halves**, carrying a monotonic generation counter |
| **Local** | per-key calibration (rest value, travel span, fitted magnet curve), board UID, last link rate | **never synced** — it describes that physical PCB |

On link-up each half sends `(generation, hash)` in the hail. Higher generation wins and pushes the whole
shared blob; a few KB at 12 Mbaud is under a millisecond, so there is no reason to be clever about
deltas. Equal generation with unequal hash is a genuine conflict — surface it rather than guessing.

Calibration being local is what makes `uid_tag()` matter beyond arbitration: calibration is keyed to the
board that produced it, so swapping a half never silently applies the other board's curve.

The web app only ever talks to the centre. Reaching the peripheral needs one addition to the raw-HID
protocol: a **route-to-peer wrapper** — a command that says "forward this payload to the peer and
return its reply". One frame type, and the configurator can then show and edit both halves without
caring which one is plugged in.

### Firmware: the ROM bootloader cannot be reached over J1 as wired

Datasheet Table 5 lists every interface the boot ROM will accept a new image on:

| Peripheral | Pins | On our AT32F405RBT7-7 |
|---|---|---|
| USART1 | PA9 / PA10 | yes — the J6/J9 debug header |
| USART2 | PA2 / PA3 | pins are ADC mux inputs here |
| **USART3** | **PC10 / PC11** | **yes**, listed for `AT32F405RxT7-7` specifically |
| OTGHS1 | D− / D+ | yes (J3) |
| OTGFS1 | PA11 / PA12 | yes (J2) — what `tools/flash.sh` uses |
| I2C1/2/3, CAN1, SPI1 | various | yes |

**USART6 and UART7 are absent from that list**, and USART6 on PC6/PC7 is exactly what J1 carries. So
the peripheral's ROM is unreachable over the link on this board.

Measured aside: with both J2 and J3 connected, the ROM DFU enumerates **only on J2, at full speed**,
despite OTGHS1 being a listed interface. Whether it would fall back to J3 with J2 absent is untested.

### What saves this: every half has its own USB

Because the design is symmetric — same firmware, same schematic, both halves carrying J2 and J3 — the
universal recovery is always "unplug that half and plug it into a computer". It then *is* the centre.
A link-delivered update therefore does not have to be bulletproof, which makes an in-application
updater acceptable:

1. Host DFUs the centre exactly as today.
2. Centre relays the image over the link in CRC'd chunks.
3. Peripheral writes it with a **RAM-resident** flash routine and RAM-resident vectors.

Step 3 depends on a Phase 0 measurement that is **still unverified**: whether a RAM-resident erase keeps
ISRs alive across the 6.6–8 ms stall (§2). Do that before designing around it.

And add a **firmware version field to the hail**. Two halves running different protocol versions is a
silent corruption source; the correct behaviour is to refuse to form a split and say so.

### The respin lever: move the link to USART1, and it costs nothing

**Put J1's D+/D- on PA9/PA10 (USART1) instead of PC6/PC7 (USART6), and move the debug console to
USART3 on PC10/PC11.** A straight swap of two nets, and the peripheral's ROM bootloader becomes
reachable over the link with no extra conductors.

This works because of a coincidence worth stating plainly: **USART1 is the only ROM bootloader
interface that is also APB2-clocked.**

| Candidate | Clock domain | Baud ceiling | ROM bootloader? |
|---|---|---|---|
| USART6 / UART7 — PC6/PC7, where J1 is today | APB2 216 MHz / APB1 108 MHz | 13.5 / 6.75 | **no** |
| USART3 — PC10/PC11 | APB1 108 MHz | **6.75** | yes |
| USART2 — PA2/PA3 | APB1 108 MHz | 6.75 | yes, but those pins are ADC mux outputs |
| **USART1 — PA9/PA10** | **APB2 216 MHz** | **13.5** | **yes** |

APB1 is not a free choice: the datasheet fixes `fPCLK1 = fHCLK/2` whenever `fHCLK > 120 MHz`, so at
216 MHz SCLK every APB1 UART is capped at 108/16 = 6.75 Mbaud. Measured against the frame budget that
is 108 µs for a 72-byte frame — **86 % of a microframe**, and still 67 % after packing positions to
12 bits. USART1 keeps the whole measured 12 Mbaud and its 48.5 %.

On the dev board PA9/PA10 go only to J6 pins 8/7 and J9 pins 3/5 — the debug console and the SWD
header — so freeing them costs nothing but relocating the console, and PC10/PC11 (`/SPI3_SCK` and
`/SPI3_MISO` breakouts on J6) are free to take it. A console does not care that USART3 is APB1-clocked.

Directions work out with a straight-through cable and no new hardware. The ROM has USART1_TX on PA9 and
USART1_RX on PA10; put **PA9 on D+ and PA10 on D-**. The peripheral in ROM then transmits on D+ and
listens on D-, so the centre sets `CTRL2.TRPSWAP` to transmit on D- and listen on D+ — a register write
it already controls. Normal operation is unchanged: arbitration still decides which half swaps, just on
a different USART.

Updating the peripheral becomes: centre tells it to reboot into ROM (the magic-in-`.noinit` plus
`NVIC_SystemReset()` that already works for `CMD_BOOTLOADER`), then bridges USB to USART1 and speaks
Artery's ISP protocol. **No custom updater, no RAM-resident flash writer, and unbrickable**, because the
ROM runs before user code.

*(An earlier revision of this section proposed wiring J1's unconnected SBU1/SBU2 to PC10/PC11 as a
side-channel. Rejected: USB 2.0-only Type-C cables are not required to carry SBU, so that path would
vanish under a substituted cable, and it spent two extra conductors to reach a slower UART. D+/D- is
carried by every cable that could possibly work at all.)*

One pin-map caveat to carry into the respin, since the ROM configures every bootloader interface at
once: **PA2/PA3 are USART2's ROM pins and are also this board's ADC mux outputs D2/D3.** Entering DFU
therefore puts a USART peripheral onto two analog nodes. It has been harmless in practice — the mux
R_ON plus the 5.1 kΩ filter resistor limits contention to a few hundred µA — but it is worth designing
out on a board being laid out fresh.
