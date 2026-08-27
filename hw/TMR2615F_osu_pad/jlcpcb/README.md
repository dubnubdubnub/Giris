# TMR2615F_osu_pad — jlcpcb fabrication package

Generated 2026-08-04 04:05 from `TMR2615F_osu_pad.kicad_pcb` by `.claude/skills/fab-export`.

## Board

| | |
|---|---|
| Size | 70.1000 mm × 60.0000 mm |
| Copper layers | 4 (F.Cu, In1.Cu, In2.Cu, B.Cu) |
| Finished thickness | 1.6167 mm |
| Min track width | 0.1600 mm |
| Min clearance | 0.1600 mm |
| Min drill | 0.3000 mm |
| Through vias | 636 |
| NPTH pads | 27 |

## Stackup as designed

Non-default materials — confirm the fab's equivalent when ordering.

```
F.SilkS | Top Silk Screen
F.Paste | Top Solder Paste
F.Mask | Top Solder Mask | 0.01524 mm | JLCPCB Soldermask | Er 3.8
F.Cu | copper | 0.035 mm
dielectric 1 | prepreg | 0.2104 mm | Nan Ya Plastics NP-155F 7628 | Er 4.4
In1.Cu | copper | 0.0152 mm
dielectric 2 | core | 1.065 mm | Nan Ya Plastics NP-155F Core | Er 4.43
In2.Cu | copper | 0.0152 mm
dielectric 3 | prepreg | 0.2104 mm | Nan Ya Plastics NP-155F 7628 | Er 4.4
B.Cu | copper | 0.035 mm
B.Mask | Bottom Solder Mask | 0.01524 mm | JLCPCB Soldermask | Er 3.8
B.Paste | Bottom Solder Paste
B.SilkS | Bottom Silk Screen
```

## Files

- `TMR2615F_osu_pad-gerbers-jlcpcb.zip` — 13 files, flat zip: gerbers (KiCad `.gbr` extensions), Excellon drill (PTH/NPTH separate, mm, absolute origin), `.gbrjob`
- `BOM-TMR2615F_osu_pad-jlcpcb.csv` — 34 line items
- `CPL-TMR2615F_osu_pad-jlcpcb.csv` — 151 placements (128 top / 23 bottom)
- `gerbers/` — the same plot files, unzipped, for review
- `docs/` — drill maps, board statistics, DRC report

## Ordering

Upload the gerber zip under *PCB*, then enable *PCB Assembly* and upload
`BOM-*.csv` and `CPL-*.csv` on the parts step.
Rows with an empty `LCSC Part #` must be matched by hand in their part selector
(or marked Do Not Place).

## Checks

- DRC: **96 error-severity violations, 0 unconnected** — see `docs/drc.rpt`
- 1/34 BOM lines have no LCSC part number: TP1
- 1/34 BOM lines have **neither MPN nor LCSC** — these cannot be sourced automatically
- Dropped 1 empty layer(s) — nothing to plot, and empty gerbers make some portals report a parse failure: TMR2615F_osu_pad-B_Paste.gbr
- DNP parts excluded from both BOM and CPL
- Schematic part-number fields detected: lcsc→LCSC, mpn→MPN, mfr→Manufacturer

