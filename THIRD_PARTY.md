# Third-party inventory

Everything in this repository that someone else wrote, what it is licensed under,
whether Giris *redistributes* it, and what that obliges. `NOTICE` carries the
attributions that must travel with a distributed binary; this file is the
reasoning behind them.

Two questions are kept apart throughout, because they have different answers:

- **Publishing this source repository.** Only things actually committed here matter.
- **Distributing a compiled firmware binary.** Things fetched at build time and
  linked in matter too, even though they are `.gitignore`d.

## Giris' own licences

| Path | Licence |
|---|---|
| everything not listed below | Apache-2.0 (`LICENSE`) |
| `hw/` — schematics, PCB, magnet models | CERN-OHL-P v2 (`hw/LICENSE`) |

## Fetched at build time — not in this repository

`fw/vendor/` is `.gitignore`d. `fw/tools/fetch-deps.sh` populates it. Neither
dependency is redistributed by the source repo; **both are linked into firmware
binaries**, so their notices must accompany any binary release.

| Dependency | Licence | Notes |
|---|---|---|
| TinyUSB | MIT | Clean. Attribution only. |
| ArteryTek AT32F402/405 BSP | **disputed — see below** | |

### The Artery licence discrepancy

The BSP ships two licence statements that do not agree.

- `fw/vendor/at32f402_405/LICENSE` is a plain, unmodified **BSD 3-Clause**.
- The notice at the head of every BSP source file grants use, copying and
  distribution *"for the purpose of design and development **in conjunction with
  Artery microcontrollers**."*

A field-of-use restriction of that kind is not an open-source grant; it fails the
OSI definition's "no discrimination against fields of endeavour". So the BSP
should not be described as straightforwardly BSD-3-Clause, which is how
`hw/TMR2615F_osu_pad/firmware_architecture.md` §11 records it.

**Practical impact on Giris: none.** This firmware runs only on an AT32F405, so
the narrower reading is satisfied either way. It is recorded because anyone
reusing Giris' code on non-Artery silicon inherits the question, and because a
downstream compliance scan will flag it.

## Committed, and derived from Artery

Three files are Artery's work, committed here because a build needs them. They
keep Artery's notice and are **not** covered by Giris' Apache-2.0 grant.

| File | State |
|---|---|
| `fw/include/at32f402_405_conf.h` | unmodified |
| `fw/src/startup_at32f402_405.s` | unmodified |
| `fw/ld/at32f405rbt7.ld` | **modified** — `LENGTH` corrected from Artery's declared 256K to the part's real 128K, and a `.noinit` section added for the bootloader-entry magic |

## Submodules

| Submodule | Upstream | Licence |
|---|---|---|
| `hw/marbastlib` | `ebastler/marbastlib` | CERN-OHL-P v2 — same as `hw/`, no friction |
| `hw/library` | `uwrealitylabs/library` | CERN-OHL-P v2 — added 2026-09 (see Open questions) |

A submodule is a reference, not a copy: cloning Giris does not itself
redistribute either library. That is a weaker protection than it sounds, though,
since the board designs are unusable without them.

## Deliberately not used

Giris is written from scratch. These were read as references and **no source file
was ever pasted out of any of them**, which is what keeps the firmware
permissively licensed rather than copyleft.

| Project | Licence |
|---|---|
| libhmk | GPL-3.0 |
| VIA (`the-via/app`) | GPL-3.0 |
| minipad, DeskHop | GPL-3.0 |
| QMK, vial-qmk, Keychron's QMK fork | GPL-2.0 |

This matters most for the **USB XUSB (Xbox) interface**. The two available
reference implementations — libhmk and a Keychron QMK branch — are both copyleft.

The distinction to hold onto: interface *facts* — a class triple of
0xFF/0x5D/0x01, an endpoint count, the field order of a 20-byte report — describe
how to interoperate with someone else's system and are not owned by whoever wrote
them down. Another author's expression of those facts, their structs, macros,
naming and comments, is. So the implementation must be written independently, and
the source cited should be documentation of the interface rather than a
GPL-licensed implementation of it.

**The source to cite is [MS-XUSBI]**, *Xbox Universal Serial Bus Protocol (XUSB)
Interface Extension*, Microsoft Open Specifications, published 2024-09-16:

  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xusbi/c79474e7-3968-43d1-8d2f-175d47bef43e

It is first-party, normative, and covers precisely this interface — §3.2.1.1.1
Table 45 gives the 0xFF / 0x5D / 0x01 class triple, and §3.2.5.1.1 Table 52 gives
the 20-byte input report field by field, with Table 53's button bit assignment.
It also covers the MS OS descriptor compatible ID, rumble and LED output reports,
`GET_CAPABILITIES`, and battery reporting. It downloads anonymously as PDF or
DOCX with no agreement to accept.

Its Open Specifications IPR notice grants **copyright** permission to copy the
documentation "in order to develop implementations of the technologies that are
described in this documentation" — which is exactly the permission a clean-room
implementation needs, and it is why writing from this spec is strictly better
founded than working from community reverse engineering such as the Linux `xpad`
driver's comments.

Two things it does **not** do, recorded so nobody assumes otherwise:

- **No patent grant is established.** The IPR notice says delivery of the
  documentation grants no patent licence, and that a document *might* be covered
  by the Open Specifications Promise. MS-XUSBI is not listed in Microsoft's
  Patent and Program Map. That map updates twice a year and MS-XUSBI postdates
  parts of it, so this may be lag rather than exclusion — but as of today no
  patent promise covering it has been published.
- **Microsoft considers XUSB deprecated** — its own overview says it "is no
  longer the recommended input mechanism." It remains what `xusb22.sys` binds,
  and therefore what actually reaches XInput. The successor protocol is
  [MS-GIPUSB], which is the Xbox One/Series interface, not this one.

## Open questions

### 1. `hw/library` is unlicensed — STILL BLOCKING

`github.com/uwrealitylabs/library` supplies footprints, symbols and 3D models the
boards depend on, and has no `LICENSE` file upstream. Absent a licence the default
is all rights reserved: no grant to use, modify or redistribute.

A CERN-OHL-P v2 text has been written into the local checkout at
`hw/library/LICENSE`, but **it is untracked and unpushed, so it grants nothing
yet.** A licence file in your own working copy of someone else's repository is
not a licence. This is resolved only when that commit lands on
`uwrealitylabs/library` upstream and the submodule pointer here is updated.

Two things make this more urgent than a submodule reference usually is:

- **KiCad inlines geometry.** The committed board and schematic files already
  contain full copies of the library's footprints and symbols — 1221 graphic
  elements across 168 footprint definitions in
  `hw/TMR2615F_osu_pad/TMR2615F_osu_pad.kicad_pcb` alone. Dropping the submodule
  would not undo that; the copies are in the tracked files.
- **The committed STEP assemblies** merge third-party 3D bodies from
  `hw/library/packages3d` whose headers show vendor/EasyEDA origin. UWRL cannot
  relicense those even by licensing its own library. See §4.

The repository also has more than one contributor, and licensing a work needs the
agreement of everyone holding copyright in it. `hw/library/README.md` now invites
objections. Normally a formality, but it is not automatic.

### 2. Silkscreen artwork — resolved as to authorship

`hw/TMR2615F_osu_pad/TMR2615F_osu_pad.kicad_pcb` carries three footprints named
`LOGO`. Their anchors sit on B.Cu but none of the geometry is on copper: two are
character illustrations on `B.SilkS` (31.1 x 41.2 mm and 35.4 x 43.8 mm) and one
is a small monogram on `B.Mask`. All three were drawn by the repository owner, so
they carry the same copyright as the rest of the work and redistribute under
CERN-OHL-P v2 with the board files.

They travel further than the `.kicad_pcb`: the silkscreen gerber
(`TMR2615F_osu_pad-B_Silkscreen.gbr`, the largest file in the fab set), its
NextPCB twin, the mask gerbers, both fab archives, and the committed STEP
assemblies — `mech/Keyboard.step` includes a `TMR2615F_osu_pad_silkscreen` body.
Worth knowing if the art is ever revised: it must be regenerated in all of them.

Authorship of the rendition is settled. Whether the *characters* depicted are
original is a separate question — one that would matter before manufacturing
boards for sale, and not at all if they are original.

### 3. Committed vendor documents — 147 MB

`hw/refs/` holds 42 manufacturer PDFs plus 7 verbatim `.txt` extractions of the
same documents. Datasheets are free to *download* and essentially never free to
*redistribute*; extracting the text does not change that. Widespread practice in
open hardware and rarely challenged, but it is unlicensed redistribution, so it
deserves a decision rather than a default.

Two items still stand out:

- `pins_raw.txt`, at the repository root, is a verbatim text extraction of the
  Artery datasheet — it still carries `===PAGE` markers and the document header.
- `rm0440-...-stm32g4-...pdf` (37 MB) is an **ST** reference manual for a
  different vendor's MCU family.

**Removed 2026-09:** `hw/refs/CH32V307WCU6-R0/*.SchDoc` and `*.PcbDoc` — WCH's
editable Altium reference-design source (2.6 MB) for a part this project does not
use. Redistributing a vendor's *design source* is a stronger claim than sharing a
datasheet. Gone from `HEAD`; still in history.

If the rest is to go, replace it with a manifest of part numbers and source URLs
and fetch on demand. Note that `hw/TMR2615F_osu_pad/magnetics/measure_switch.py`
reads the switch drawings directly, so that workflow changes with them.

### 4. Committed STEP assemblies carry third-party bodies

`hw/TMR2615F_osu_pad/TMR2615F_osu_pad.step`, `output/osupad.step` and
`mech/Keyboard.step` (which despite its name is a full board assembly) merge 3D
bodies originating from `hw/library/packages3d`, whose headers show vendor and
EasyEDA origin. Those cannot be relicensed by UWRL even once §1 is resolved, and
a STEP export bakes them in rather than referencing them.

`mech/combined.step`, `mech/Giris Plate.step` and `mech/osu_pad_dev_frame.step`
are first-party and unaffected.

### 5. History

Everything above is in the commit history, so removing a file from `HEAD` leaves
it recoverable. If any of it should not be published, the cheap moment to fix it
— a history rewrite or a fresh squashed initial commit — is **before the first
public push**, and only then.
