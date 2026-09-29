# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""Export a schematic PDF that draws each repeated sheet only once.

kicad-cli plots one page per sheet instance, so a key sheet placed 32 times
becomes 32 near-identical pages. This keeps the first page of every repeated
sheet file, stamps a note on it, and appends a table of the reference
designators each instance gets, read from the schematic's (instances ...)
blocks, so no information is lost.

Assumes a single level of hierarchy (the root sheet places the child sheets
directly), which is what hw/giris and hw/TMR2615F_osu_pad use. Anything else
is an error rather than a silently wrong table.

Needs pypdf and reportlab. Uses the fork's kicad-cli (file format 20260623);
override with $KICAD_CLI.

Usage: python3 sch_pdf.py board.kicad_sch out.pdf
"""
import io
import os
import re
import subprocess
import sys
import tempfile

try:
    from pypdf import PdfReader, PdfWriter
    from reportlab.lib import colors
    from reportlab.lib.pagesizes import landscape, A4
    from reportlab.pdfgen import canvas
    from reportlab.platypus import SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer
    from reportlab.lib.styles import getSampleStyleSheet
except ImportError as e:
    sys.exit(f"sch_pdf.py needs pypdf and reportlab ({e}); pip install pypdf reportlab")

KICAD_CLI = os.environ.get(
    "KICAD_CLI",
    "/Users/isaacchiu/Documents/GitHub/kicad/build/kicad/KiCad.app/Contents/MacOS/kicad-cli")


def natural(s):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def root_sheets(text):
    """[(uuid, name, file)] for every sheet the root places, in file order."""
    root_uuid = re.search(r'^\t\(uuid "([^"]+)"\)', text, re.M).group(1)
    sheets = []
    for block in re.split(r"\n\t\(sheet\n", text)[1:]:
        uuid = re.search(r'^\t\t\(uuid "([^"]+)"\)', block, re.M).group(1)
        name = re.search(r'\(property "Sheetname" "([^"]*)"', block).group(1)
        file = re.search(r'\(property "Sheetfile" "([^"]*)"', block).group(1)
        sheets.append((uuid, name, file))
    return root_uuid, sheets


def instance_refs(child_text, root_uuid):
    """{sheet_uuid: {symbol_uuid: ref}} for the real (non-#) symbols."""
    out = {}
    for block in re.split(r"\n\t\(symbol\n", child_text)[1:]:
        if re.search(r'\(lib_id "power:', block):
            continue
        sym = re.search(r'^\t\t\(uuid "([^"]+)"\)', block, re.M).group(1)
        for path, ref in re.findall(r'\(path "([^"]+)"\s*\(reference "([^"]+)"\)', block):
            if ref.startswith("#"):
                continue
            parts = path.strip("/").split("/")
            if len(parts) != 2 or parts[0] != root_uuid:
                continue  # an instance belonging to another project
            out.setdefault(parts[1], {})[sym] = ref
    return out


def stamp(page, lines):
    w, h = float(page.mediabox.width), float(page.mediabox.height)
    buf = io.BytesIO()
    c = canvas.Canvas(buf, pagesize=(w, h))
    size = h / 70
    x, y = w * 0.06, h * 0.88
    box_h = size * 1.5 * len(lines) + size
    box_w = max(c.stringWidth(l, "Helvetica", size) for l in lines) + 2 * size
    c.setFillColor(colors.Color(1, 0.97, 0.8))
    c.setStrokeColor(colors.Color(0.7, 0.5, 0))
    c.rect(x - size, y - box_h + size, box_w, box_h, fill=1)
    c.setFillColor(colors.black)
    for i, l in enumerate(lines):
        c.setFont("Helvetica-Bold" if i == 0 else "Helvetica", size)
        c.drawString(x, y - i * size * 1.5, l)
    c.save()
    buf.seek(0)
    page.merge_page(PdfReader(buf).pages[0])


def table_pdf(sections):
    buf = io.BytesIO()
    doc = SimpleDocTemplate(buf, pagesize=landscape(A4), leftMargin=28, rightMargin=28,
                            topMargin=28, bottomMargin=28)
    styles = getSampleStyleSheet()
    flow = []
    for title, note, header, rows in sections:
        flow += [Paragraph(title, styles["Heading2"]), Paragraph(note, styles["BodyText"]),
                 Spacer(0, 8)]
        t = Table([header] + rows, repeatRows=1)
        t.setStyle(TableStyle([
            ("FONT", (0, 0), (-1, -1), "Helvetica", 7),
            ("FONT", (0, 0), (-1, 0), "Helvetica-Bold", 7),
            ("FONT", (0, 1), (0, -1), "Helvetica-Bold", 7),
            ("BACKGROUND", (0, 0), (-1, 0), colors.Color(0.9, 0.9, 0.9)),
            ("BACKGROUND", (0, 1), (-1, 1), colors.Color(1, 0.97, 0.8)),
            ("GRID", (0, 0), (-1, -1), 0.25, colors.grey),
        ]))
        flow += [t, Spacer(0, 16)]
    doc.build(flow)
    buf.seek(0)
    return PdfReader(buf)


def main(sch, out):
    sch = os.path.abspath(sch)
    here = os.path.dirname(sch)
    text = open(sch).read()
    root_uuid, sheets = root_sheets(text)

    by_file = {}
    for uuid, name, file in sheets:
        by_file.setdefault(file, []).append((uuid, name))

    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "raw.pdf")
        subprocess.run([KICAD_CLI, "sch", "export", "pdf", "-o", raw, sch], check=True,
                       cwd=here, stdout=subprocess.DEVNULL)
        reader = PdfReader(raw)

        # Page -> sheet name, from the title block's "Sheet: /Name/" line.
        page_sheet = []
        for p in reader.pages:
            m = re.search(r"Sheet:\s*/([^/\s]*)/?", p.extract_text() or "")
            if not m:
                sys.exit(f"no 'Sheet:' title-block line on a page of {raw}")
            page_sheet.append(m.group(1))
        if len(page_sheet) != len(sheets) + 1:
            sys.exit(f"{len(page_sheet)} pages for {len(sheets)} sheets + root: "
                     "nested hierarchy is not supported")

        name_to_page = {n: i for i, n in enumerate(page_sheet) if n}
        writer = PdfWriter()
        sections = []
        drop = set()
        stamp_on = {}
        for file, insts in by_file.items():
            if len(insts) < 2:
                continue
            child = open(os.path.join(here, file)).read()
            if "\n\t(sheet\n" in child:
                sys.exit(f"{file} places its own sheets: nested hierarchy is not supported")
            insts.sort(key=lambda u: name_to_page[u[1]])
            shown_uuid, shown = insts[0]
            refs = instance_refs(child, root_uuid)
            for uuid, name in insts:
                if uuid not in refs:
                    sys.exit(f"no reference designators for sheet {name} in {file}")
            syms = sorted(refs[shown_uuid], key=lambda s: natural(refs[shown_uuid][s]))
            rows = [[name] + [refs[uuid].get(s, "?") for s in syms] for uuid, name in insts]
            names = [n for _, n in insts]
            for n in names[1:]:
                drop.add(name_to_page[n])
            listed = names if len(names) <= 6 else names[:3] + ["..."] + names[-2:]
            stamp_on[name_to_page[shown]] = [
                f"{file} is used {len(insts)} times; only /{shown}/ is drawn here.",
                f"The other instances ({', '.join(listed[1:])}) are the same circuit",
                "with different reference designators, listed per instance at the end of this PDF.",
            ]
            sections.append((
                f"{file}: reference designators per instance",
                f"This sheet is placed {len(insts)} times. The PDF draws only /{shown}/ "
                f"(highlighted row); every other instance has the same circuit with the "
                f"designators in its row. Columns follow the drawn instance's designators.",
                ["Sheet"] + [refs[shown_uuid][s] for s in syms], rows))

        for i, p in enumerate(reader.pages):
            if i in drop:
                continue
            page = writer.add_page(p)
            if i in stamp_on:
                stamp(page, stamp_on[i])
        if sections:
            for p in table_pdf(sections).pages:
                writer.add_page(p)
        with open(out, "wb") as f:
            writer.write(f)
    print(f"{out}: {len(reader.pages)} sheet pages -> {len(reader.pages) - len(drop)}"
          f" (+{len(writer.pages) - len(reader.pages) + len(drop)} designator table page(s))")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
