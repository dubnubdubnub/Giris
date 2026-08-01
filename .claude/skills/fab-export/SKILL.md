---
name: fab-export
description: Generate a complete fabrication + assembly package (gerber zip, Excellon drill, BOM, pick-and-place/CPL) for NextPCB or JLCPCB from a KiCad project in this repo, using the KiCad 10.99 fork's kicad-cli. Use whenever the user asks to export fab files, gerbers, BOM, CPL/centroid/placement files, prepare a board for manufacturing or assembly, or "send this to NextPCB/JLCPCB".
---

# Fab export (NextPCB / JLCPCB)

One script does everything. Run it and report the results — do not hand-roll
kicad-cli invocations.

```bash
python3 .claude/skills/fab-export/scripts/fab_export.py hw/<BOARD_DIR>
```

That produces `hw/<BOARD_DIR>/nextpcb/` and `hw/<BOARD_DIR>/jlcpcb/`, each a
self-contained, upload-ready folder:

| File | Purpose |
|---|---|
| `<board>-gerbers-<vendor>.zip` | flat zip: gerbers + `.drl` (+ `.gbrjob` for JLCPCB) — upload as-is |
| `BOM-<board>-<vendor>.csv` | vendor's exact column layout |
| `CPL-<board>-<vendor>.csv` | `Designator,Mid X,Mid Y,Rotation,Layer`, mm |
| `…-nextpcb.xlsx` | same two tables as `.xlsx` — **the files to upload to NextPCB** |
| `gerbers/` | same plot files unzipped, for review |
| `docs/` | drill maps, board stats, DRC report |
| `README.md` | order sheet: size, stackup, min track/drill, warnings |

**After running, read the generated `README.md` and relay its Checks section** —
DRC violations and BOM lines missing part numbers are the things that actually
block an order.

## Options

```
--vendor nextpcb|jlcpcb|both   default: both
--outdir DIR                   parent for the vendor folders (default: board dir)
--variant NAME                 export a design variant
--smd-only                     CPL: SMD footprints only
--include-dnp                  keep DNP parts in BOM and CPL (default: excluded)
--protel                       force .gtl/.gbl gerber extensions for every vendor
--separate-th / --merge-th     force PTH+NPTH into two drill files, or one
--skip-drc                     skip the DRC pass (~6 s on a 4-layer board)
--no-mpn-from-value            don't infer an MPN from a part-number-looking Value
--kicad-cli PATH               override the binary
--sch PATH                     schematic path if not the sibling .kicad_sch
```

The positional argument accepts a project directory, a `.kicad_pcb`, or a
`.kicad_pro`.

## What the script already handles

Don't re-derive these — they are baked in and verified against this fork:

- **Fork binary.** Defaults to
  `~/Documents/GitHub/kicad/build/kicad/KiCad.app/Contents/MacOS/kicad-cli`
  (10.99). Release KiCad cannot open these boards — file format 20260623.
  Override with `$KICAD_CLI` or `--kicad-cli`.
- **Layer list** is read from the board's own `(layers)` block and ordered
  F.Cu → In1 → …  → B.Cu, so 2/4/6-layer boards all work unchanged.
- **Zones are refilled** before plotting (`--check-zones`).
- **Drill**: Excellon, mm, decimal, absolute origin. Gerbers also use absolute
  origin, so the two always agree. The fork's first-class NPTH holes land in
  the NPTH set correctly.
- **Empty layers are dropped from the zip.** KiCad happily plots a layer with
  no objects (B.Paste on a board with no bottom paste is the usual one), and
  several portals report a generic **"file parsing failed"** on it. Detected by
  the absence of any `%AD` aperture or `G36` region; the drop is listed in the
  README's Checks section.
- **Drill maps are excluded from the zip** (they live in `docs/`) so the
  vendor's layer auto-detector can't mistake one for a copper layer.
- **BOM grouping** collapses by value/footprint/part-number, and reference
  ranges are disabled (`R1,R2,R3`, never `R1-R3`) — both portals reject ranges.
- **Part-number fields are auto-detected** across every `.kicad_sch` sheet and
  coalesced, so mixed conventions in one design (this repo has both `LCSC` and
  `LCSC ID`) resolve to one column.
- **Footprint library prefixes are stripped** (`Capacitor_SMD:C_0805…` →
  `C_0805…`) for the vendors' footprint matchers.

## Vendor differences the script encodes

Packaging (`PROFILES` in the script):

| | JLCPCB | NextPCB |
|---|---|---|
| Gerber extensions | KiCad `.gbr` | Protel `.gtl/.gbl/.g1…` |
| Drill | PTH + NPTH separate | merged into one `.drl` |
| `.gbrjob` in zip | yes | **no** |
| BOM / CPL upload format | `.csv` | `.xlsx` (plus `.csv`) |

NextPCB gets the conservative set: their uploader is fussier than JLCPCB's, they
recommend merged PTH+NPTH in their own KiCad guide so neither file gets
overlooked, and a JSON `.gbrjob` can trip a parser that treats every archive
member as gerber. JLCPCB documents the plain KiCad output and reads the job file.

BOM and CPL:

- **JLCPCB** BOM: `Comment, Designator, Footprint, LCSC Part #`. Sources by LCSC
  code; a blank part number means manual matching in their part selector.
- **NextPCB** BOM: `Item, Designator, Quantity, Comment, Footprint, Description,
  Manufacturer, Manufacturer Part Number, Supplier, Supplier Part Number`.
  Sources by MPN, with LCSC codes passed through as supplier part numbers.
- CPL content is identical for both — same five columns, mm, KiCad's native
  sign convention (negative Y), which both portals expect.

`.xlsx` is written by a small hand-rolled OOXML writer in the script (inline
strings, no styles) because neither `openpyxl` nor `xlsxwriter` is installed and
the `hw/` tooling stays dependency-free. Timestamps inside the archive are
pinned so regenerating an unchanged BOM produces identical bytes in git. The
`.csv` twin is always written too — it is the diffable form, and `.xlsx` is a
binary zip that git cannot merge.

## If a portal still rejects the zip

Work down this list; each step is a real cause seen in the wild:

1. Re-run — empty-layer pruning and the NextPCB profile above are already applied
   and cover the two most common failures.
2. `--merge-th` / `--separate-th` — try the other drill arrangement.
3. `--protel` — force Protel extensions for the vendor that is failing.
4. Check `gerbers/` for any file that is header-only (`M02*` right after the
   aperture list). That is an empty layer the heuristic missed; delete and re-zip.

## Adding a vendor

Write a `write_bom_<vendor>` function, register it in `BOM_WRITERS`, add an
entry to `VENDOR_NOTES`, and extend the `--vendor` choices. Gerbers, drill and
CPL are already vendor-neutral.
