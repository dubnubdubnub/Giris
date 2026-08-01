#!/usr/bin/env python3
"""Generate vendor-ready fabrication + assembly packages from a KiCad project.

Targets NextPCB and JLCPCB. Drives the KiCad `kicad-cli` binary only (no pcbnew
Python bindings), so it works against the 10.99 fork used for the hw/ boards.

Outputs, per vendor, a self-contained directory:

    <outdir>/<vendor>/
        gerbers/                      loose plot + drill files
        <board>-gerbers-<vendor>.zip  flat zip, ready to upload
        BOM-<board>-<vendor>.csv      vendor column layout
        CPL-<board>-<vendor>.csv      Designator,Mid X,Mid Y,Rotation,Layer
        docs/                         drill maps, board stats, DRC report
        README.md                     order sheet: stackup, sizes, warnings

Usage:
    fab_export.py <board.kicad_pcb | project-dir> [--vendor nextpcb|jlcpcb|both]
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

# --------------------------------------------------------------------------
# kicad-cli discovery
# --------------------------------------------------------------------------

FORK_CLI = Path.home() / "Documents/GitHub/kicad/build/kicad/KiCad.app/Contents/MacOS/kicad-cli"
STOCK_CLI_CANDIDATES = [
    Path("/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"),
    Path("/usr/bin/kicad-cli"),
    Path("/usr/local/bin/kicad-cli"),
]


def resolve_cli(explicit: str | None) -> Path:
    """Pick a kicad-cli. The fork wins by default: hw/ boards use file format
    20260623, which release KiCad refuses to open (FUTURE_FORMAT_ERROR)."""
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("KICAD_CLI"):
        candidates.append(Path(os.environ["KICAD_CLI"]))
    candidates.append(FORK_CLI)
    which = shutil.which("kicad-cli")
    if which:
        candidates.append(Path(which))
    candidates.extend(STOCK_CLI_CANDIDATES)

    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    die("no kicad-cli found; pass --kicad-cli PATH or set $KICAD_CLI")


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------


def die(msg: str) -> "None":
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def info(msg: str) -> None:
    print(f"  {msg}")


def run(cmd: list) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True)


def run_checked(cmd: list, what: str) -> str:
    proc = run(cmd)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        die(f"{what} failed (exit {proc.returncode})")
    return proc.stdout


# --------------------------------------------------------------------------
# project resolution
# --------------------------------------------------------------------------


def resolve_project(target: str, sch_override: str | None) -> tuple[Path, Path | None]:
    """Return (pcb_path, sch_path|None) from a .kicad_pcb, .kicad_pro or a directory."""
    p = Path(target).expanduser().resolve()

    if p.is_dir():
        pcbs = sorted(p.glob("*.kicad_pcb"))
        if not pcbs:
            die(f"no .kicad_pcb in {p}")
        if len(pcbs) > 1:
            die(f"multiple .kicad_pcb in {p}; name one explicitly")
        pcb = pcbs[0]
    elif p.suffix == ".kicad_pro":
        pcb = p.with_suffix(".kicad_pcb")
        if not pcb.exists():
            die(f"{pcb} not found")
    elif p.suffix == ".kicad_pcb":
        pcb = p
    else:
        die(f"expected a .kicad_pcb, .kicad_pro or project directory, got {p}")

    if sch_override:
        sch: Path | None = Path(sch_override).expanduser().resolve()
        if sch and not sch.exists():
            die(f"{sch} not found")
    else:
        cand = pcb.with_suffix(".kicad_sch")
        sch = cand if cand.exists() else None

    return pcb, sch


# --------------------------------------------------------------------------
# board introspection (s-expression text scan; the fork has no pcbnew bindings)
# --------------------------------------------------------------------------

PLOT_NON_COPPER = [
    "F.Paste",
    "B.Paste",
    "F.SilkS",
    "B.SilkS",
    "F.Mask",
    "B.Mask",
    "Edge.Cuts",
]


def read_layer_block(pcb: Path) -> list[tuple[int, str, str]]:
    """Parse the top-level (layers ...) block -> [(ordinal, name, type)]."""
    head = pcb.read_text(errors="replace")[:400_000]
    start = head.find("\n\t(layers\n")
    if start < 0:
        start = head.find("(layers")
    if start < 0:
        die("could not locate the (layers ...) block in the board file")

    out: list[tuple[int, str, str]] = []
    for line in head[start : start + 8000].splitlines()[1:]:
        s = line.strip()
        if s.startswith(")"):
            break
        m = re.match(r'\((\d+)\s+"([^"]+)"\s+(\w+)', s)
        if m:
            out.append((int(m.group(1)), m.group(2), m.group(3)))
    if not out:
        die("(layers ...) block parsed empty")
    return out


def copper_layers(layers: list[tuple[int, str, str]]) -> list[str]:
    """Copper layers in physical stackup order: F.Cu, In1..InN, B.Cu."""
    names = [n for _, n, t in layers if n.endswith(".Cu")]

    def key(n: str) -> tuple[int, int]:
        if n == "F.Cu":
            return (0, 0)
        if n == "B.Cu":
            return (2, 0)
        m = re.match(r"In(\d+)\.Cu", n)
        return (1, int(m.group(1)) if m else 999)

    return sorted(names, key=key)


def plot_layer_list(layers: list[tuple[int, str, str]]) -> tuple[list[str], list[str]]:
    present = {n for _, n, _ in layers}
    cu = copper_layers(layers)
    other = [n for n in PLOT_NON_COPPER if n in present]
    return cu, cu + other


def sexpr_at(text: str, start: int) -> str:
    """Slice the single balanced s-expression beginning at `start` (a '(')."""
    depth, in_str, esc = 0, False, False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    return text[start:]


def parse_stackup(pcb: Path) -> list[str]:
    """Human-readable stackup lines from the (stackup ...) block, for the README."""
    head = pcb.read_text(errors="replace")[:400_000]
    i = head.find("(stackup")
    if i < 0:
        return []
    blob = sexpr_at(head, i)
    lines: list[str] = []
    for m in re.finditer(r'\(layer "([^"]+)"(.*?)\n\t*\)', blob, re.S):
        name, body = m.group(1), m.group(2)
        typ = re.search(r'\(type "([^"]+)"\)', body)
        thk = re.search(r"\(thickness ([\d.]+)\)", body)
        mat = re.search(r'\(material "([^"]+)"\)', body)
        er = re.search(r"\(epsilon_r ([\d.]+)\)", body)
        bits = [name]
        if typ:
            bits.append(typ.group(1))
        if thk:
            bits.append(f"{float(thk.group(1)):.4g} mm")
        if mat:
            bits.append(mat.group(1))
        if er:
            bits.append(f"Er {er.group(1)}")
        lines.append(" | ".join(bits))
    return lines


def schematic_fields(sch: Path) -> set[str]:
    """All symbol property names across every sheet of the design."""
    found: set[str] = set()
    for f in sorted(sch.parent.glob("*.kicad_sch")):
        try:
            text = f.read_text(errors="replace")
        except OSError:
            continue
        found.update(re.findall(r'\(property\s+"([^"]+)"', text))
    return found


# --------------------------------------------------------------------------
# BOM field detection
# --------------------------------------------------------------------------

LCSC_ALIASES = ["LCSC Part #", "LCSC", "LCSC ID", "LCSC#", "LCSC Part Number", "JLCPCB Part #"]
MPN_ALIASES = ["MPN", "Manufacturer Part Number", "MP", "Part Number", "PN", "MFR Part #"]
MFR_ALIASES = ["Manufacturer", "MF", "MFR", "Mfg", "Brand"]

UNITY = re.compile(r"^\s*[\d.]+\s*(nF|uF|pF|µF|mF|F|nH|uH|µH|mH|H|k|K|M|R|m|Ω|ohm|V|A|mA|%)?\s*$", re.I)


def detect(aliases: list[str], present: set[str]) -> list[str]:
    """Every alias actually used in the schematic, in preference order."""
    lower = {p.lower(): p for p in present}
    return [lower[a.lower()] for a in aliases if a.lower() in lower]


def looks_like_mpn(value: str) -> bool:
    """Heuristic: is this Value string a real part number rather than '10k'/'100nF'?"""
    v = value.strip()
    if len(v) < 6 or UNITY.match(v):
        return False
    return bool(re.search(r"[A-Za-z]", v)) and bool(re.search(r"\d", v))


def first_nonempty(row: dict, keys: list[str]) -> str:
    for k in keys:
        val = (row.get(k) or "").strip()
        if val:
            return val
    return ""


def strip_lib(footprint: str) -> str:
    """'Capacitor_SMD:C_0805_2012Metric' -> 'C_0805_2012Metric'."""
    return footprint.split(":", 1)[-1] if ":" in footprint else footprint


# --------------------------------------------------------------------------
# export steps
# --------------------------------------------------------------------------


GERBER_SUFFIXES = {".gbr", ".gtl", ".gbl", ".gts", ".gbs", ".gto", ".gbo", ".gtp", ".gbp", ".gm1"}
# Protel names inner copper .g1 .g2 ... .gN
INNER_SUFFIX = re.compile(r"^\.g\d+$", re.I)


def is_gerber(path: Path) -> bool:
    s = path.suffix.lower()
    return s in GERBER_SUFFIXES or bool(INNER_SUFFIX.match(s))


def is_empty_gerber(path: Path) -> bool:
    """A plot with no aperture definitions and no regions draws nothing.

    KiCad emits these happily (e.g. B.Paste on a board with no bottom SMD paste),
    but several fab portals -- NextPCB's among them -- fail to parse a layer file
    that contains no objects, which surfaces as a generic 'file parsing failed'.
    """
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return False
    return "%AD" not in text and "G36" not in text


def prune_empty_gerbers(dest: Path) -> list[str]:
    dropped = []
    for f in sorted(dest.iterdir()):
        if is_gerber(f) and is_empty_gerber(f):
            dropped.append(f.name)
            f.unlink()
    return dropped


def export_gerbers(cli: Path, pcb: Path, dest: Path, layers: list[str], prof: dict, args) -> list[str]:
    dest.mkdir(parents=True, exist_ok=True)
    cmd = [
        cli, "pcb", "export", "gerbers",
        "-o", dest,
        "--layers", ",".join(layers),
        "--check-zones",
    ]
    if not prof["protel"]:
        cmd.append("--no-protel-ext")
    if args.variant:
        cmd += ["--variant", args.variant]
    cmd.append(pcb)
    run_checked(cmd, "gerber export")

    if not prof["gbrjob"]:
        # A .gbrjob is JSON, not gerber. Parsers that try every archive member as
        # gerber choke on it; JLCPCB reads it, NextPCB does not need it.
        for f in dest.glob("*.gbrjob"):
            f.unlink()

    dropped = prune_empty_gerbers(dest)
    kept = sum(1 for f in dest.iterdir() if is_gerber(f))
    note = f", dropped {len(dropped)} empty" if dropped else ""
    info(f"gerbers: {kept} layers{note} ({'protel' if prof['protel'] else 'kicad'} ext)")
    return dropped


def export_drill(cli: Path, pcb: Path, dest: Path, docs: Path, prof: dict, args) -> None:
    cmd = [
        cli, "pcb", "export", "drill",
        "-o", dest,
        "--format", "excellon",
        "--drill-origin", "absolute",
        "--excellon-zeros-format", "decimal",
        "--excellon-units", "mm",
        "--generate-map",
        "--map-format", "pdf",
    ]
    if prof["separate_th"]:
        cmd.append("--excellon-separate-th")
    cmd.append(pcb)
    run_checked(cmd, "drill export")

    # Drill maps are documentation, not fab data -- keep them out of the upload zip
    # so the vendor's layer auto-detector never mistakes one for a copper layer.
    docs.mkdir(parents=True, exist_ok=True)
    moved = 0
    for f in list(dest.glob("*drl_map*")):
        shutil.move(str(f), str(docs / f.name))
        moved += 1
    drls = sorted(p.name for p in dest.glob("*.drl"))
    info(f"drill: {', '.join(drls) or 'none'} (+{moved} map -> docs/)")


def export_cpl(cli: Path, pcb: Path, args) -> list[dict]:
    """Run pos export and normalise to the Designator/Mid X/Mid Y/Rotation/Layer schema."""
    with tempfile.TemporaryDirectory() as td:
        raw = Path(td) / "pos.csv"
        cmd = [
            cli, "pcb", "export", "pos",
            "-o", raw,
            "--format", "csv",
            "--units", "mm",
            "--side", "both",
            pcb,
        ]
        if not args.include_dnp:
            cmd.insert(-1, "--exclude-dnp")
        if args.smd_only:
            cmd.insert(-1, "--smd-only")
        if args.variant:
            cmd = cmd[:-1] + ["--variant", args.variant, cmd[-1]]
        run_checked(cmd, "position file export")
        text = raw.read_text()

    rows: list[dict] = []
    for r in csv.DictReader(io.StringIO(text)):
        side = (r.get("Side") or "").strip().lower()
        rows.append(
            {
                "Designator": (r.get("Ref") or "").strip(),
                "Mid X": f"{float(r['PosX']):.4f}",
                "Mid Y": f"{float(r['PosY']):.4f}",
                "Rotation": f"{float(r['Rot']):.4f}",
                "Layer": "Bottom" if side.startswith("b") else "Top",
            }
        )
    rows.sort(key=lambda r: natural_ref(r["Designator"]))
    return rows


def natural_ref(ref: str) -> tuple[str, int]:
    m = re.match(r"([A-Za-z_]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def export_bom(cli: Path, sch: Path, args) -> tuple[list[dict], dict]:
    """Run the grouped BOM export and return (rows, detected-field-map)."""
    present = schematic_fields(sch)
    lcsc_fields = detect(LCSC_ALIASES, present)
    mpn_fields = detect(MPN_ALIASES, present)
    mfr_fields = detect(MFR_ALIASES, present)

    base = ["Reference", "Value", "Footprint", "Description"]
    extra = lcsc_fields + mpn_fields + mfr_fields
    fields = base + extra + ["${QUANTITY}", "${DNP}"]
    labels = base + extra + ["Qty", "DNP"]

    group_by = [f for f in (["Value", "Footprint"] + lcsc_fields + mpn_fields) if f]

    with tempfile.TemporaryDirectory() as td:
        raw = Path(td) / "bom.csv"
        cmd = [
            cli, "sch", "export", "bom",
            "-o", raw,
            "--fields", ",".join(fields),
            "--labels", ",".join(labels),
            "--group-by", ",".join(group_by),
            "--sort-field", "Reference",
            # JLCPCB and NextPCB both reject collapsed ranges ("R1-R4"); force
            # every designator to be listed out.
            "--ref-range-delimiter", "",
            "--ref-delimiter", ",",
        ]
        if not args.include_dnp:
            cmd.append("--exclude-dnp")
        if args.variant:
            cmd += ["--variant", args.variant]
        cmd.append(sch)
        run_checked(cmd, "BOM export")
        text = raw.read_text()

    rows: list[dict] = []
    for r in csv.DictReader(io.StringIO(text)):
        value = (r.get("Value") or "").strip()
        mpn = first_nonempty(r, mpn_fields)
        if not mpn and args.mpn_from_value and looks_like_mpn(value):
            mpn = value
        rows.append(
            {
                "refs": (r.get("Reference") or "").strip(),
                "value": value,
                "footprint": (r.get("Footprint") or "").strip(),
                "description": (r.get("Description") or "").strip(),
                "qty": (r.get("Qty") or "").strip(),
                "dnp": (r.get("DNP") or "").strip(),
                "lcsc": first_nonempty(r, lcsc_fields),
                "mpn": mpn,
                "mfr": first_nonempty(r, mfr_fields),
            }
        )

    detected = {"lcsc": lcsc_fields, "mpn": mpn_fields, "mfr": mfr_fields}
    return rows, detected


# --------------------------------------------------------------------------
# vendor writers
# --------------------------------------------------------------------------


def write_csv(path: Path, header: list[str], rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, quoting=csv.QUOTE_ALL)
        w.writerow(header)
        w.writerows(rows)


# --- minimal .xlsx writer ---------------------------------------------------
# No openpyxl/xlsxwriter on this machine, and the hw/ tooling is deliberately
# dependency-free, so emit the OOXML package by hand. Inline strings only, no
# styles or shared-string table -- enough for a BOM/CPL that a portal parses.

XLSX_NUMERIC = re.compile(r"^-?(0|[1-9]\d*)(\.\d+)?$")


def col_name(idx: int) -> str:
    """0-based column index -> A, B, ... Z, AA."""
    name = ""
    idx += 1
    while idx:
        idx, rem = divmod(idx - 1, 26)
        name = chr(65 + rem) + name
    return name


def xml_escape(v: str) -> str:
    return v.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def cell_xml(ref: str, value: str) -> str:
    # Only plain integers/decimals become numbers; anything with a leading zero
    # or a letter stays text so part numbers survive intact.
    if value and XLSX_NUMERIC.match(value):
        return f'<c r="{ref}"><v>{value}</v></c>'
    return f'<c r="{ref}" t="inlineStr"><is><t xml:space="preserve">{xml_escape(value)}</t></is></c>'


def write_xlsx(path: Path, header: list[str], rows: list[list[str]], sheet: str = "Sheet1") -> None:
    body = []
    for r_i, row in enumerate([header] + rows, 1):
        cells = "".join(cell_xml(f"{col_name(c_i)}{r_i}", v) for c_i, v in enumerate(row))
        body.append(f'<row r="{r_i}">{cells}</row>')

    parts = {
        "[Content_Types].xml": (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="xml" ContentType="application/xml"/>'
            '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
            '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
            "</Types>"
        ),
        "_rels/.rels": (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
            "</Relationships>"
        ),
        "xl/workbook.xml": (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
            'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            f'<sheets><sheet name="{xml_escape(sheet)}" sheetId="1" r:id="rId1"/></sheets>'
            "</workbook>"
        ),
        "xl/_rels/workbook.xml.rels": (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
            "</Relationships>"
        ),
        "xl/worksheets/sheet1.xml": (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
            f"<sheetData>{''.join(body)}</sheetData>"
            "</worksheet>"
        ),
    }

    # Fixed timestamps: these get committed, and a churning mtime would make
    # every regeneration look like a content change.
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, content in parts.items():
            zi = zipfile.ZipInfo(name, date_time=(2000, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, content)


# --- vendor table layouts ---------------------------------------------------


def bom_table_jlcpcb(rows: list[dict], include_dnp: bool) -> tuple[list[str], list[list[str]]]:
    header = ["Comment", "Designator", "Footprint", "LCSC Part #"]
    out = [[r["value"], r["refs"], strip_lib(r["footprint"]), r["lcsc"]] for r in rows]
    return header, out


def bom_table_nextpcb(rows: list[dict], include_dnp: bool) -> tuple[list[str], list[list[str]]]:
    header = [
        "Item",
        "Designator",
        "Quantity",
        "Comment",
        "Footprint",
        "Description",
        "Manufacturer",
        "Manufacturer Part Number",
        "Supplier",
        "Supplier Part Number",
    ]
    if include_dnp:
        header.append("DNP")
    out = []
    for i, r in enumerate(rows, 1):
        line = [
            str(i),
            r["refs"],
            r["qty"],
            r["value"],
            strip_lib(r["footprint"]),
            r["description"],
            r["mfr"],
            r["mpn"],
            "LCSC" if r["lcsc"] else "",
            r["lcsc"],
        ]
        if include_dnp:
            line.append(r["dnp"])
        out.append(line)
    return header, out


BOM_TABLES = {"jlcpcb": bom_table_jlcpcb, "nextpcb": bom_table_nextpcb}

CPL_HEADER = ["Designator", "Mid X", "Mid Y", "Rotation", "Layer"]


def cpl_table(rows: list[dict]) -> tuple[list[str], list[list[str]]]:
    return CPL_HEADER, [[r[h] for h in CPL_HEADER] for r in rows]


def zip_dir(src: Path, dest_zip: Path) -> int:
    """Flat zip -- every file at the archive root, which both portals accept."""
    files = sorted(p for p in src.iterdir() if p.is_file())
    with zipfile.ZipFile(dest_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f, arcname=f.name)
    return len(files)


# --------------------------------------------------------------------------
# checks + README
# --------------------------------------------------------------------------


def board_stats(cli: Path, pcb: Path, docs: Path) -> str:
    out = docs / "board-stats.txt"
    proc = run([cli, "pcb", "export", "stats", "-o", out, "--units", "mm", pcb])
    if proc.returncode != 0 or not out.exists():
        return ""
    return out.read_text(errors="replace")


def run_drc(cli: Path, pcb: Path, docs: Path) -> tuple[int, int, str]:
    """Returns (error-severity violations, unconnected items, report path or '')."""
    rpt = docs / "drc.rpt"
    proc = run(
        [cli, "pcb", "drc", "--format", "report", "-o", rpt, "--severity-error", pcb]
    )
    text = proc.stdout + proc.stderr
    v = re.search(r"Found (\d+) violations", text)
    u = re.search(r"Found (\d+) unconnected", text)
    return (
        int(v.group(1)) if v else -1,
        int(u.group(1)) if u else -1,
        str(rpt) if rpt.exists() else "",
    )


def grab(stats: str, label: str) -> str:
    m = re.search(rf"^- {re.escape(label)}: (.+)$", stats, re.M)
    return m.group(1).strip() if m else "?"


# Per-vendor packaging. JLCPCB documents the plain KiCad output and reads the
# X2 job file. NextPCB gets the conservative Protel set with PTH+NPTH merged;
# the merge is their own KiCad guide's recommendation, the rest is just a
# verified-working combination -- the KiCad-native set was never shown to fail
# there, so don't treat these as requirements.
PROFILES = {
    "jlcpcb": {"protel": False, "separate_th": True, "gbrjob": True, "xlsx": False},
    "nextpcb": {"protel": True, "separate_th": False, "gbrjob": False, "xlsx": True},
}


VENDOR_NOTES = {
    "jlcpcb": [
        "Upload the gerber zip under *PCB*, then enable *PCB Assembly* and upload",
        "`BOM-*.csv` and `CPL-*.csv` on the parts step.",
        "Rows with an empty `LCSC Part #` must be matched by hand in their part selector",
        "(or marked Do Not Place).",
    ],
    "nextpcb": [
        "Upload the gerber zip on the *PCB Instant Quote* page; for assembly add the",
        "`BOM-*.csv` and `CPL-*.csv` under *PCB Assembly*.",
        "NextPCB sources by MPN. Rows with neither a `Manufacturer Part Number` nor a",
        "`Supplier Part Number` will come back as a quoting query.",
    ],
}


def write_readme(
    path: Path,
    vendor: str,
    board: str,
    pcb: Path,
    stats: str,
    stack: list[str],
    cu: list[str],
    bom_rows: list[dict],
    cpl_rows: list[dict],
    zipname: str,
    gerber_count: int,
    drc: tuple[int, int, str],
    detected: dict,
    dropped: list[str],
    prof: dict,
    args,
) -> None:
    stamp = _dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    no_pn = [r for r in bom_rows if not r["lcsc"] and not r["mpn"]]
    no_lcsc = [r for r in bom_rows if not r["lcsc"]]

    L: list[str] = []
    L.append(f"# {board} — {vendor} fabrication package")
    L.append("")
    L.append(f"Generated {stamp} from `{pcb.name}` by `.claude/skills/fab-export`.")
    L.append("")
    L.append("## Board")
    L.append("")
    L.append("| | |")
    L.append("|---|---|")
    L.append(f"| Size | {grab(stats,'Width')} × {grab(stats,'Height')} |")
    L.append(f"| Copper layers | {len(cu)} ({', '.join(cu)}) |")
    L.append(f"| Finished thickness | {grab(stats,'Board stackup thickness')} |")
    L.append(f"| Min track width | {grab(stats,'Min track width')} |")
    L.append(f"| Min clearance | {grab(stats,'Min track clearance')} |")
    L.append(f"| Min drill | {grab(stats,'Min drill diameter')} |")
    L.append(f"| Through vias | {grab(stats,'Through vias')} |")
    L.append(f"| NPTH pads | {grab(stats,'NPTH')} |")
    L.append("")

    if stack:
        L.append("## Stackup as designed")
        L.append("")
        L.append("Non-default materials — confirm the fab's equivalent when ordering.")
        L.append("")
        L.append("```")
        L.extend(stack)
        L.append("```")
        L.append("")

    drill_desc = "PTH/NPTH separate" if prof["separate_th"] else "PTH+NPTH merged"
    ext_desc = "Protel extensions" if prof["protel"] else "KiCad `.gbr` extensions"
    L.append("## Files")
    L.append("")
    L.append(f"- `{zipname}` — {gerber_count} files, flat zip: gerbers ({ext_desc}), Excellon drill ({drill_desc}, mm, absolute origin){', `.gbrjob`' if prof['gbrjob'] else ''}")
    if prof["xlsx"]:
        L.append(f"- **`BOM-{board}-{vendor}.xlsx`** — {len(bom_rows)} line items — *upload this one*")
        L.append(f"- **`CPL-{board}-{vendor}.xlsx`** — {len(cpl_rows)} placements ({sum(1 for r in cpl_rows if r['Layer']=='Top')} top / {sum(1 for r in cpl_rows if r['Layer']=='Bottom')} bottom) — *upload this one*")
        L.append("- `BOM-*.csv` / `CPL-*.csv` — identical content, kept as the diffable form in git")
    else:
        L.append(f"- `BOM-{board}-{vendor}.csv` — {len(bom_rows)} line items")
        L.append(f"- `CPL-{board}-{vendor}.csv` — {len(cpl_rows)} placements ({sum(1 for r in cpl_rows if r['Layer']=='Top')} top / {sum(1 for r in cpl_rows if r['Layer']=='Bottom')} bottom)")
    L.append("- `gerbers/` — the same plot files, unzipped, for review")
    L.append("- `docs/` — drill maps, board statistics, DRC report")
    L.append("")

    L.append("## Ordering")
    L.append("")
    for line in VENDOR_NOTES[vendor]:
        L.append(line)
    L.append("")

    L.append("## Checks")
    L.append("")
    errs, unconn, rpt = drc
    if errs < 0:
        L.append("- DRC: **not run**")
    elif errs == 0 and unconn == 0:
        L.append("- DRC: clean (0 error-severity violations, 0 unconnected)")
    else:
        L.append(f"- DRC: **{errs} error-severity violations, {unconn} unconnected** — see `docs/drc.rpt`")
    if no_lcsc:
        L.append(f"- {len(no_lcsc)}/{len(bom_rows)} BOM lines have no LCSC part number: {', '.join(r['refs'].split(',')[0] for r in no_lcsc[:12])}{' …' if len(no_lcsc)>12 else ''}")
    if no_pn:
        L.append(f"- {len(no_pn)}/{len(bom_rows)} BOM lines have **neither MPN nor LCSC** — these cannot be sourced automatically")
    if dropped:
        L.append(f"- Dropped {len(dropped)} empty layer(s) — nothing to plot, and empty gerbers make some portals report a parse failure: {', '.join(dropped)}")
    if not args.include_dnp:
        L.append("- DNP parts excluded from both BOM and CPL")
    det = ", ".join(f"{k}→{v[0]}" for k, v in detected.items() if v) or "none"
    L.append(f"- Schematic part-number fields detected: {det}")
    L.append("")

    path.write_text("\n".join(L) + "\n")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help=".kicad_pcb, .kicad_pro, or project directory")
    ap.add_argument("--vendor", choices=["nextpcb", "jlcpcb", "both"], default="both")
    ap.add_argument("--outdir", help="parent for the vendor folders (default: board directory)")
    ap.add_argument("--sch", help="schematic path (default: sibling .kicad_sch)")
    ap.add_argument("--kicad-cli", dest="kicad_cli", help="kicad-cli binary to use")
    ap.add_argument("--variant", help="design variant name to export")
    ap.add_argument("--skip-drc", action="store_true", help="don't run DRC")
    ap.add_argument("--smd-only", action="store_true", help="CPL: SMD footprints only")
    ap.add_argument("--include-dnp", action="store_true", help="keep DNP parts in BOM and CPL")
    ap.add_argument("--protel", action="store_true", help="force Protel gerber extensions (.gtl/.gbl) for every vendor")
    ap.add_argument(
        "--separate-th",
        dest="separate_th",
        action="store_true",
        default=None,
        help="force PTH and NPTH into separate drill files",
    )
    ap.add_argument(
        "--merge-th",
        dest="separate_th",
        action="store_false",
        help="force PTH and NPTH into one drill file",
    )
    ap.add_argument(
        "--no-mpn-from-value",
        dest="mpn_from_value",
        action="store_false",
        help="don't infer an MPN from the Value field when it looks like a part number",
    )
    ap.set_defaults(mpn_from_value=True)
    args = ap.parse_args()

    cli = resolve_cli(args.kicad_cli)
    pcb, sch = resolve_project(args.board, args.sch)
    board = pcb.stem
    outdir = Path(args.outdir).expanduser().resolve() if args.outdir else pcb.parent
    vendors = ["nextpcb", "jlcpcb"] if args.vendor == "both" else [args.vendor]

    version = run([cli, "version"]).stdout.strip()
    print(f"fab-export: {board}")
    info(f"kicad-cli {version} ({cli})")
    if sch is None:
        info("no schematic found — BOM will be skipped")

    layers = read_layer_block(pcb)
    cu, plot = plot_layer_list(layers)
    stack = parse_stackup(pcb)

    # --- build the vendor-neutral artifacts once ----------------------------
    staging = Path(tempfile.mkdtemp(prefix="fab-export-"))
    try:
        ddir = staging / "docs"
        ddir.mkdir(parents=True, exist_ok=True)

        cpl_rows = export_cpl(cli, pcb, args)
        info(f"placements: {len(cpl_rows)}")

        bom_rows: list[dict] = []
        detected: dict = {}
        if sch is not None:
            bom_rows, detected = export_bom(cli, sch, args)
            info(f"BOM: {len(bom_rows)} line items, {sum(int(r['qty'] or 0) for r in bom_rows)} parts")

        stats = board_stats(cli, pcb, ddir)
        if args.skip_drc:
            drc = (-1, -1, "")
        else:
            drc = run_drc(cli, pcb, ddir)
            info(f"DRC: {drc[0]} violations, {drc[1]} unconnected")

        # --- fan out to each vendor -----------------------------------------
        for vendor in vendors:
            prof = dict(PROFILES[vendor])
            if args.protel:
                prof["protel"] = True
            if args.separate_th is not None:
                prof["separate_th"] = args.separate_th

            print(f"  [{vendor}]")
            vdir = outdir / vendor
            if vdir.exists():
                shutil.rmtree(vdir)
            vdir.mkdir(parents=True)

            shutil.copytree(ddir, vdir / "docs")
            gdir = vdir / "gerbers"
            dropped = export_gerbers(cli, pcb, gdir, plot, prof, args)
            export_drill(cli, pcb, gdir, vdir / "docs", prof, args)

            zipname = f"{board}-gerbers-{vendor}.zip"
            n = zip_dir(gdir, vdir / zipname)

            tables = [("CPL", *cpl_table(cpl_rows))]
            if bom_rows:
                tables.append(("BOM", *BOM_TABLES[vendor](bom_rows, args.include_dnp)))
            for kind, hdr, body in tables:
                stem = vdir / f"{kind}-{board}-{vendor}"
                # CSV always: it is the diffable form that lives in git.
                write_csv(stem.with_suffix(".csv"), hdr, body)
                if prof["xlsx"]:
                    write_xlsx(stem.with_suffix(".xlsx"), hdr, body, kind)

            write_readme(
                vdir / "README.md", vendor, board, pcb, stats, stack, cu,
                bom_rows, cpl_rows, zipname, n, drc, detected, dropped, prof, args,
            )
            print(f"  -> {vdir}")
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
