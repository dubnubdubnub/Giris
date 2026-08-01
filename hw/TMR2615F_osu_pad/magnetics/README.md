# Magnetics: switch magnet → TMR2615F sensor model

Answers "which MX magnetic switches work with this board, and what does the sensor
see?" for the osu!pad's per-key TMR2615F-AAC-1.500-500 sensors.

## Files

| File | What |
|---|---|
| `magfield.py` | Field of an axially magnetized cylinder magnet at an off-axis point. Equivalent-solenoid model (stack of current loops), complete elliptic integrals via AGM. Pure stdlib. |
| `test_magfield.py` | Checks: K/E against known values, loop closed forms, on-axis analytic face field, dipole far-field limit, symmetry. `python3 test_magfield.py` |
| `fit_switches.py` | Per-switch magnet models (measured or spec-fitted), predicts the in-plane field at this board's sensor across travel. `python3 fit_switches.py` |
| `measure_switch.py` | Measures magnet size / rest height off vendor engineering-drawing PDFs (600 dpi render + dark-pixel edge voting, calibrated against two dimensioned widths per view). Used on `hw/refs/switches/*.pdf`. |
| `tmr-magnet-model.html` | Interactive visualization of the same model (self-contained JS port — no build, open in a browser). Sliders for press depth, sensor offset, PCB thickness, magnet size, sensitivity code. |

## Geometry (measured from `TMR2615F_osu_pad.kicad_pcb`)

- Sensor is **3.09 mm radially offset** from the switch center, rotated so its in-plane
  (Bx) sensing axis points through the switch axis → it reads the stem magnet's
  **radial** field component.
- Single-side assembly on F; the switch inserts from the B side, so the magnet sees the
  die **through the 1.62 mm board** (+0.3 mm die depth in the pads-down DFN3L).
- Switch footprint is standard MX 5-pin + Kailh hotswap; the socket is unused by
  (pinless) magnetic switches — board is hybrid mech/magnetic capable.

## Key results (SEN 1.500 → output rails at ±300 Gs)

All twelve modeled switch families land in range with margin: ~34–48 Gs at rest,
~165–286 Gs at bottom-out vs the ±300 Gs rail — **full-travel linearity for every
switch**, with Gateron KS-20 (905 Gs class) closest to the rail.

Five Gateron switches were *measured* off their dimensioned engineering drawings
(`hw/refs/switches/`): Jade, Jade Pro, Jade Ultra, Jade Attraction, and the MCHOSE
Apollo all use the **same Ø2.77 × 3.41 mm magnet cylinder**; families differ only in
carry height (rest height 4.13–4.73 mm above PCB) and magnet grade (implied Br
1.16–1.66 T). The published initial flux (quoted at 0.3 mm pressed, on their 1.2 mm
reference PCB) validates the model within 2–14 % as a held-out check. Every modern
Gateron drawing states **"Magnet N-pole is facing down"** — community lore calling
Gateron "S-down" is wrong per primary source. The bipolar TMR2615F handles either
polarity, but don't mix polarities on one board unless firmware calibrates sign per
key.

Unpublished specs: Wooting Lekker (community-assumed = KS-20), Raesha/DrunkDeer and
Geon Raptor HE (no flux figures anywhere), TTC/Kailh/Akko (flux specs but no
drawings). Kailh Flame/Mistral quote "2000 Gs trigger flux" at an unknown measurement
point — excluded from the calibrated set; bench-test first. MonsGeek sells no switches
of its own (all Akko-made).

## Caveats

- Vendors publish flux at the sensor, not magnet dimensions. Magnet size is a **fit**
  to the published rest/bottom-out pairs (converges on Ø3.0–3.6 × 1.6–2.2 mm,
  N52-class); treat absolute field values as ±25 %. The published rest flux is held
  out as a validation and reported as "rest chk".
- Verify on hardware before bulk-ordering sensors: log the ADC across full travel with
  one real switch — one bench trace collapses all magnet-fit uncertainty.
- Research provenance (manufacturer specs, polarity clusters, compatibility notes) is
  summarized in the July 2026 conversation that produced this; primary sources are
  Gateron/TTC/Kailh/MonsGeek product pages and the MonsGeek magnetic-switch
  knowledge base.
