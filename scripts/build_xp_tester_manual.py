from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.style import WD_STYLE_TYPE
from docx.enum.table import WD_CELL_VERTICAL_ALIGNMENT, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "Ruisi-USB-DLP-XP-Tester-Manual.docx"

NAVY = "17324D"
BLUE = "2E74B5"
DARK_BLUE = "1F4D78"
MUTED = "5F6B76"
LIGHT_BLUE = "E8EEF5"
LIGHT_GRAY = "F2F4F7"
CAUTION_FILL = "FFF3CD"
CAUTION_BORDER = "B7791F"
RISK_FILL = "FDECEC"
RISK_BORDER = "9B1C1C"
GREEN_FILL = "EAF4EA"
GREEN_BORDER = "2D6A4F"
CONTENT_DXA = 9360
TABLE_INDENT_DXA = 120


def set_cell_margins(cell, top=80, start=120, bottom=80, end=120):
    tc_pr = cell._tc.get_or_add_tcPr()
    tc_mar = tc_pr.first_child_found_in("w:tcMar")
    if tc_mar is None:
        tc_mar = OxmlElement("w:tcMar")
        tc_pr.append(tc_mar)
    for name, value in (("top", top), ("start", start), ("bottom", bottom), ("end", end)):
        node = tc_mar.find(qn(f"w:{name}"))
        if node is None:
            node = OxmlElement(f"w:{name}")
            tc_mar.append(node)
        node.set(qn("w:w"), str(value))
        node.set(qn("w:type"), "dxa")


def set_cell_width(cell, width_dxa):
    tc_pr = cell._tc.get_or_add_tcPr()
    tc_w = tc_pr.first_child_found_in("w:tcW")
    if tc_w is None:
        tc_w = OxmlElement("w:tcW")
        tc_pr.append(tc_w)
    tc_w.set(qn("w:w"), str(width_dxa))
    tc_w.set(qn("w:type"), "dxa")


def shade_cell(cell, fill):
    tc_pr = cell._tc.get_or_add_tcPr()
    shd = tc_pr.find(qn("w:shd"))
    if shd is None:
        shd = OxmlElement("w:shd")
        tc_pr.append(shd)
    shd.set(qn("w:fill"), fill)


def set_table_geometry(table, widths):
    table.alignment = WD_TABLE_ALIGNMENT.LEFT
    table.autofit = False
    table_pr = table._tbl.tblPr
    tbl_w = table_pr.first_child_found_in("w:tblW")
    if tbl_w is None:
        tbl_w = OxmlElement("w:tblW")
        table_pr.append(tbl_w)
    tbl_w.set(qn("w:w"), str(sum(widths)))
    tbl_w.set(qn("w:type"), "dxa")
    tbl_ind = OxmlElement("w:tblInd")
    tbl_ind.set(qn("w:w"), str(TABLE_INDENT_DXA))
    tbl_ind.set(qn("w:type"), "dxa")
    table_pr.append(tbl_ind)
    layout = OxmlElement("w:tblLayout")
    layout.set(qn("w:type"), "fixed")
    table_pr.append(layout)
    grid = table._tbl.tblGrid
    for child in list(grid):
        grid.remove(child)
    for width in widths:
        col = OxmlElement("w:gridCol")
        col.set(qn("w:w"), str(width))
        grid.append(col)
    for row in table.rows:
        for index, cell in enumerate(row.cells):
            set_cell_width(cell, widths[index])
            set_cell_margins(cell)
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER


def set_run_font(run, name="Calibri", size=None, color=None, bold=None, italic=None):
    run.font.name = name
    run._element.get_or_add_rPr().rFonts.set(qn("w:ascii"), name)
    run._element.get_or_add_rPr().rFonts.set(qn("w:hAnsi"), name)
    if size is not None:
        run.font.size = Pt(size)
    if color is not None:
        run.font.color.rgb = RGBColor.from_string(color)
    if bold is not None:
        run.bold = bold
    if italic is not None:
        run.italic = italic


def add_page_field(paragraph):
    run = paragraph.add_run()
    begin = OxmlElement("w:fldChar")
    begin.set(qn("w:fldCharType"), "begin")
    instruction = OxmlElement("w:instrText")
    instruction.set(qn("xml:space"), "preserve")
    instruction.text = " PAGE "
    separate = OxmlElement("w:fldChar")
    separate.set(qn("w:fldCharType"), "separate")
    end = OxmlElement("w:fldChar")
    end.set(qn("w:fldCharType"), "end")
    run._r.extend((begin, instruction, separate, end))
    set_run_font(run, size=9, color=MUTED)


def paragraph_shading(paragraph, fill, border):
    p_pr = paragraph._p.get_or_add_pPr()
    shd = OxmlElement("w:shd")
    shd.set(qn("w:fill"), fill)
    p_pr.append(shd)
    borders = OxmlElement("w:pBdr")
    left = OxmlElement("w:left")
    left.set(qn("w:val"), "single")
    left.set(qn("w:sz"), "18")
    left.set(qn("w:space"), "8")
    left.set(qn("w:color"), border)
    borders.append(left)
    p_pr.append(borders)
    ind = OxmlElement("w:ind")
    ind.set(qn("w:left"), "160")
    ind.set(qn("w:right"), "120")
    p_pr.append(ind)


def add_callout(doc, label, text, kind="note"):
    colors = {
        "note": (LIGHT_BLUE, BLUE),
        "caution": (CAUTION_FILL, CAUTION_BORDER),
        "risk": (RISK_FILL, RISK_BORDER),
        "pass": (GREEN_FILL, GREEN_BORDER),
    }
    fill, border = colors[kind]
    paragraph = doc.add_paragraph()
    paragraph.paragraph_format.space_before = Pt(5)
    paragraph.paragraph_format.space_after = Pt(8)
    paragraph.paragraph_format.line_spacing = 1.2
    paragraph_shading(paragraph, fill, border)
    label_run = paragraph.add_run(label + ": ")
    set_run_font(label_run, bold=True, color=NAVY)
    text_run = paragraph.add_run(text)
    set_run_font(text_run, color=NAVY)
    return paragraph


class Numbering:
    def __init__(self, document):
        self.root = document.part.numbering_part.element
        self.next_abstract = self._next_id("w:abstractNum", "w:abstractNumId")
        self.next_num = self._next_id("w:num", "w:numId")
        self.bullet_abstract = self._add_abstract("bullet")
        self.decimal_abstract = self._add_abstract("decimal")
        self.bullet_num = self._add_num(self.bullet_abstract)

    def _next_id(self, tag, attribute):
        values = [int(item.get(qn(attribute))) for item in self.root.findall(qn(tag))]
        return max(values, default=0) + 1

    def _add_abstract(self, kind):
        abstract_id = self.next_abstract
        self.next_abstract += 1
        abstract = OxmlElement("w:abstractNum")
        abstract.set(qn("w:abstractNumId"), str(abstract_id))
        multi = OxmlElement("w:multiLevelType")
        multi.set(qn("w:val"), "singleLevel")
        abstract.append(multi)
        level = OxmlElement("w:lvl")
        level.set(qn("w:ilvl"), "0")
        start = OxmlElement("w:start")
        start.set(qn("w:val"), "1")
        level.append(start)
        num_fmt = OxmlElement("w:numFmt")
        num_fmt.set(qn("w:val"), kind)
        level.append(num_fmt)
        level_text = OxmlElement("w:lvlText")
        level_text.set(qn("w:val"), "\u2022" if kind == "bullet" else "%1.")
        level.append(level_text)
        justification = OxmlElement("w:lvlJc")
        justification.set(qn("w:val"), "left")
        level.append(justification)
        p_pr = OxmlElement("w:pPr")
        tabs = OxmlElement("w:tabs")
        tab = OxmlElement("w:tab")
        tab.set(qn("w:val"), "num")
        tab.set(qn("w:pos"), "540")
        tabs.append(tab)
        p_pr.append(tabs)
        ind = OxmlElement("w:ind")
        ind.set(qn("w:left"), "540")
        ind.set(qn("w:hanging"), "270")
        p_pr.append(ind)
        spacing = OxmlElement("w:spacing")
        spacing.set(qn("w:after"), "80")
        spacing.set(qn("w:line"), "300")
        spacing.set(qn("w:lineRule"), "auto")
        p_pr.append(spacing)
        level.append(p_pr)
        if kind == "bullet":
            r_pr = OxmlElement("w:rPr")
            fonts = OxmlElement("w:rFonts")
            fonts.set(qn("w:ascii"), "Calibri")
            fonts.set(qn("w:hAnsi"), "Calibri")
            r_pr.append(fonts)
            level.append(r_pr)
        abstract.append(level)
        self.root.append(abstract)
        return abstract_id

    def _add_num(self, abstract_id):
        num_id = self.next_num
        self.next_num += 1
        num = OxmlElement("w:num")
        num.set(qn("w:numId"), str(num_id))
        abstract = OxmlElement("w:abstractNumId")
        abstract.set(qn("w:val"), str(abstract_id))
        num.append(abstract)
        level_override = OxmlElement("w:lvlOverride")
        level_override.set(qn("w:ilvl"), "0")
        start_override = OxmlElement("w:startOverride")
        start_override.set(qn("w:val"), "1")
        level_override.append(start_override)
        num.append(level_override)
        self.root.append(num)
        return num_id

    def apply(self, paragraph, kind="bullet", restart=False):
        if kind == "bullet":
            num_id = self.bullet_num
        elif restart:
            num_id = self._add_num(self.decimal_abstract)
        else:
            num_id = self._add_num(self.decimal_abstract)
        p_pr = paragraph._p.get_or_add_pPr()
        num_pr = OxmlElement("w:numPr")
        ilvl = OxmlElement("w:ilvl")
        ilvl.set(qn("w:val"), "0")
        num_id_node = OxmlElement("w:numId")
        num_id_node.set(qn("w:val"), str(num_id))
        num_pr.extend((ilvl, num_id_node))
        p_pr.append(num_pr)


def add_bullets(doc, numbering, items):
    for item in items:
        paragraph = doc.add_paragraph()
        paragraph.paragraph_format.space_after = Pt(4)
        paragraph.paragraph_format.line_spacing = 1.25
        paragraph.add_run(item)
        numbering.apply(paragraph, "bullet")


def add_steps(doc, numbering, items):
    num_id = numbering._add_num(numbering.decimal_abstract)
    for item in items:
        paragraph = doc.add_paragraph()
        paragraph.paragraph_format.space_after = Pt(4)
        paragraph.paragraph_format.line_spacing = 1.25
        if isinstance(item, tuple):
            title, detail = item
            title_run = paragraph.add_run(title + ". ")
            title_run.bold = True
            paragraph.add_run(detail)
        else:
            paragraph.add_run(item)
        p_pr = paragraph._p.get_or_add_pPr()
        num_pr = OxmlElement("w:numPr")
        ilvl = OxmlElement("w:ilvl")
        ilvl.set(qn("w:val"), "0")
        num_node = OxmlElement("w:numId")
        num_node.set(qn("w:val"), str(num_id))
        num_pr.extend((ilvl, num_node))
        p_pr.append(num_pr)


def add_checklist(doc, items):
    for item in items:
        paragraph = doc.add_paragraph()
        paragraph.paragraph_format.left_indent = Inches(0.18)
        paragraph.paragraph_format.first_line_indent = Inches(-0.18)
        paragraph.paragraph_format.space_after = Pt(4)
        paragraph.paragraph_format.line_spacing = 1.25
        paragraph.add_run(item)


def add_code(doc, lines):
    paragraph = doc.add_paragraph()
    paragraph.paragraph_format.left_indent = Inches(0.18)
    paragraph.paragraph_format.right_indent = Inches(0.08)
    paragraph.paragraph_format.space_before = Pt(3)
    paragraph.paragraph_format.space_after = Pt(7)
    paragraph.paragraph_format.line_spacing = 1.0
    paragraph_shading(paragraph, LIGHT_GRAY, "CDD5DF")
    run = paragraph.add_run("\n".join(lines))
    set_run_font(run, name="Courier New", size=9, color=NAVY)


def add_table(doc, headers, rows, widths):
    table = doc.add_table(rows=1, cols=len(headers))
    table.style = "Table Grid"
    for index, header in enumerate(headers):
        cell = table.rows[0].cells[index]
        shade_cell(cell, LIGHT_BLUE)
        paragraph = cell.paragraphs[0]
        paragraph.paragraph_format.space_after = Pt(0)
        run = paragraph.add_run(header)
        set_run_font(run, size=9.5, color=NAVY, bold=True)
    for row_values in rows:
        cells = table.add_row().cells
        for index, value in enumerate(row_values):
            paragraph = cells[index].paragraphs[0]
            paragraph.paragraph_format.space_after = Pt(0)
            paragraph.paragraph_format.line_spacing = 1.1
            run = paragraph.add_run(value)
            set_run_font(run, size=9.5, color=NAVY)
    header_properties = table.rows[0]._tr.get_or_add_trPr()
    header_repeat = OxmlElement("w:tblHeader")
    header_repeat.set(qn("w:val"), "true")
    header_properties.append(header_repeat)
    for row in table.rows:
        row._tr.get_or_add_trPr().append(OxmlElement("w:cantSplit"))
    set_table_geometry(table, widths)
    doc.add_paragraph().paragraph_format.space_after = Pt(0)
    return table


def keep_with_next(paragraph):
    paragraph.paragraph_format.keep_with_next = True


def configure_styles(doc):
    doc.settings.odd_and_even_pages_header_footer = True
    section = doc.sections[0]
    section.page_width = Inches(8.5)
    section.page_height = Inches(11)
    section.top_margin = Inches(1)
    section.bottom_margin = Inches(1)
    section.left_margin = Inches(1)
    section.right_margin = Inches(1)
    section.header_distance = Inches(0.492)
    section.footer_distance = Inches(0.492)

    normal = doc.styles["Normal"]
    normal.font.name = "Calibri"
    normal._element.rPr.rFonts.set(qn("w:ascii"), "Calibri")
    normal._element.rPr.rFonts.set(qn("w:hAnsi"), "Calibri")
    normal.font.size = Pt(11)
    normal.font.color.rgb = RGBColor.from_string(NAVY)
    normal.paragraph_format.space_before = Pt(0)
    normal.paragraph_format.space_after = Pt(6)
    normal.paragraph_format.line_spacing = 1.25

    heading_tokens = {
        "Heading 1": (16, BLUE, 18, 10),
        "Heading 2": (13, BLUE, 14, 7),
        "Heading 3": (12, DARK_BLUE, 10, 5),
    }
    for name, (size, color, before, after) in heading_tokens.items():
        style = doc.styles[name]
        style.font.name = "Calibri"
        style._element.rPr.rFonts.set(qn("w:ascii"), "Calibri")
        style._element.rPr.rFonts.set(qn("w:hAnsi"), "Calibri")
        style.font.size = Pt(size)
        style.font.bold = True
        style.font.color.rgb = RGBColor.from_string(color)
        style.paragraph_format.space_before = Pt(before)
        style.paragraph_format.space_after = Pt(after)
        style.paragraph_format.keep_with_next = True

    if "Code Block" not in [style.name for style in doc.styles]:
        code_style = doc.styles.add_style("Code Block", WD_STYLE_TYPE.PARAGRAPH)
        code_style.font.name = "Courier New"
        code_style.font.size = Pt(9)


def populate_header_footer(header, footer):
    header.clear()
    footer.clear()
    header.alignment = WD_ALIGN_PARAGRAPH.LEFT
    header.paragraph_format.space_after = Pt(0)
    left = header.add_run("RUISI USB DLP")
    set_run_font(left, size=9, bold=True, color=BLUE)
    right = header.add_run("  |  Windows XP SP3 x86 test handoff")
    set_run_font(right, size=9, color=MUTED)

    footer.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    footer.paragraph_format.space_before = Pt(0)
    run = footer.add_run("Version 1.1.0  |  UNVERIFIED  |  Page ")
    set_run_font(run, size=9, color=MUTED)
    add_page_field(footer)


def add_header_footer(section):
    populate_header_footer(
        section.header.paragraphs[0],
        section.footer.paragraphs[0],
    )
    populate_header_footer(
        section.even_page_header.paragraphs[0],
        section.even_page_footer.paragraphs[0],
    )


def add_heading(doc, text, level=1):
    paragraph = doc.add_heading(text, level=level)
    keep_with_next(paragraph)
    return paragraph


def build_manual():
    doc = Document()
    configure_styles(doc)
    add_header_footer(doc.sections[0])
    numbering = Numbering(doc)

    spacer = doc.add_paragraph()
    spacer.paragraph_format.space_after = Pt(74)
    kicker = doc.add_paragraph()
    kicker.alignment = WD_ALIGN_PARAGRAPH.CENTER
    kicker.paragraph_format.space_after = Pt(16)
    run = kicker.add_run("TEST HANDOFF  |  WINDOWS XP SP3 X86")
    set_run_font(run, size=10.5, bold=True, color=CAUTION_BORDER)

    title = doc.add_paragraph()
    title.alignment = WD_ALIGN_PARAGRAPH.CENTER
    title.paragraph_format.space_after = Pt(8)
    run = title.add_run("Ruisi USB DLP")
    set_run_font(run, size=30, bold=True, color=NAVY)
    subtitle = doc.add_paragraph()
    subtitle.alignment = WD_ALIGN_PARAGRAPH.CENTER
    subtitle.paragraph_format.space_after = Pt(36)
    run = subtitle.add_run("Installation and Hardware Qualification Manual")
    set_run_font(run, size=16, color=DARK_BLUE)

    metadata = add_table(
        doc,
        ["Package", "Target", "Status"],
        [["1.1.0", "Windows XP Professional SP3, x86, NTFS", "UNVERIFIED compatibility build"]],
        [1800, 3960, 3600],
    )
    metadata.rows[1].cells[2].paragraphs[0].runs[0].font.color.rgb = RGBColor.from_string(RISK_BORDER)
    add_callout(
        doc,
        "LAB USE ONLY",
        "This package enables live default-deny USB enforcement. Use only in an isolated test environment, on disposable test media, and from a VM snapshot. It is unsigned and is not approved for production deployment.",
        "risk",
    )
    paragraph = doc.add_paragraph()
    paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
    paragraph.paragraph_format.space_before = Pt(34)
    run = paragraph.add_run("Prepared 29 August 2026")
    set_run_font(run, size=10, color=MUTED)
    paragraph.add_run().add_break(WD_BREAK.PAGE)

    add_heading(doc, "1. Purpose and supported scope")
    doc.add_paragraph(
        "This manual gives an external qualification team a repeatable procedure for installing the native XP server and workstation agent, proving default-deny behavior with physical USB storage, exercising request approval and revocation, and returning usable evidence to the development team."
    )
    add_bullets(doc, numbering, [
        "Supported target: 32-bit Windows XP Professional SP3 installed on NTFS.",
        "Unsupported targets: XP x64, XP before SP3, and any newer Windows release using this XP-specific MSI.",
        "Server and agent are separate products. A small lab may install both on one XP VM; a representative test uses separate server and workstation VMs.",
        "The agent installs with DryRun=0 and default deny. Unapproved USB storage can be disabled immediately after installation.",
        "The legacy transport is HTTP with Basic authentication. It is permitted only on an isolated test network.",
    ])
    add_callout(
        doc,
        "Qualification boundary",
        "The build, MSI installation, service startup, enrollment, and policy download have been exercised on XP. Physical USB blocking, notifier behavior, approval, revocation, service-down behavior, and multi-volume media remain external hardware qualification items.",
        "caution",
    )

    add_heading(doc, "2. Package contents")
    add_table(
        doc,
        ["Path", "Purpose"],
        [
            ("client\\RuisiUsbDlpXpAgent-1.1.0-x86.msi", "Native XP agent and on-demand notifier"),
            ("client\\install-client.cmd", "Validated live-mode agent installation wrapper"),
            ("server\\RuisiUsbDlpXpServer-1.1.0-x86.msi", "Native XP management/API server"),
            ("server\\install-server.cmd", "Validated server configuration and installation wrapper"),
            ("server\\admin-server.cmd", "Safe local request/approval/revocation wrapper"),
            ("config\\deployment.ini", "Editable server URL, port, and profile"),
            ("config\\enrollment-token.txt", "Shared bootstrap token; replace before installation"),
            ("config\\admin-password.txt", "Administrator password; replace before installation"),
            ("SHA256SUMS", "Integrity hashes for every file in the kit"),
        ],
        [4320, 5040],
    )

    add_heading(doc, "3. Lab preparation")
    add_steps(doc, numbering, [
        ("Create restore points", "Take powered-off VM snapshots of the XP server and workstation before installation."),
        ("Isolate the network", "Use a host-only or otherwise isolated test segment. Do not bridge port 5080 to an untrusted network."),
        ("Prepare test media", "Use a USB stick with no valuable data. Safely eject it and leave it disconnected during installation."),
        ("Confirm prerequisites", "Verify XP Professional SP3, 32-bit architecture, NTFS system volume, local Administrator access, and correct date/time."),
        ("Prepare passthrough", "Verify the hypervisor can attach the physical USB device to XP before relying on any DLP result."),
        ("Copy and verify", "Copy the entire kit to each VM. Verify SHA256SUMS on a modern trusted machine before execution."),
    ])
    add_callout(
        doc,
        "USB passthrough",
        "A hypervisor capture failure is not an agent result. First prove that XP receives a USB or USBSTOR Plug and Play node. VirtualBox on some Windows 11 xHCI hosts may claim a device without delivering it to the guest; a USB 2.0 hub or different host may be required.",
        "note",
    )

    add_heading(doc, "4. Configure the kit")
    add_heading(doc, "4.1 Replace both placeholders", level=2)
    add_bullets(doc, numbering, [
        "Replace config\\enrollment-token.txt with one random ASCII line of at least 32 characters. The server and every workstation in this test deployment must use the identical file.",
        "Replace config\\admin-password.txt with one unique ASCII line of at least 14 characters. Do not reuse a production password.",
        "Keep both files access-controlled. Do not paste their values into command lines, screenshots, tickets, or test logs.",
    ])
    add_callout(doc, "Important", "The shipped values are deliberate placeholders and must not be used as credentials.", "risk")
    doc.add_paragraph().add_run().add_break(WD_BREAK.PAGE)
    add_heading(doc, "4.2 Edit deployment.ini", level=2)
    add_table(
        doc,
        ["Key", "Required value"],
        [
            ("ServerUrl", "HTTP URL reachable from the XP workstation, for example http://192.168.77.20:5080"),
            ("Port", "Server listener port; keep 5080 unless the isolated lab requires another port"),
            ("AdminUsername", "admin"),
            ("Profile", "legacy-xp-x86; do not change"),
        ],
        [2160, 7200],
    )
    add_code(doc, [
        "[Deployment]",
        "ServerUrl=http://192.168.77.20:5080",
        "Port=5080",
        "AdminUsername=admin",
        "Profile=legacy-xp-x86",
    ])

    add_heading(doc, "5. Install the server")
    add_steps(doc, numbering, [
        ("Open an elevated prompt", "Sign in as a local Administrator and open Command Prompt."),
        ("Open the server folder", "Change directory to the kit's server folder."),
        ("Run the wrapper", "Execute install-server.cmd. Do not launch the MSI directly because the wrapper validates and protects the configuration files."),
        ("Open the firewall only in the lab", "If client and server are separate machines, allow the configured TCP port for the isolated subnet."),
        ("Verify the service", "Confirm UsbDlp.LegacyServer reports STATE 4 RUNNING."),
    ])
    add_code(doc, [
        "cd C:\\RuisiUsbDlp-legacy-xp-x86-1.1.0\\server",
        "install-server.cmd",
        "netsh firewall add portopening protocol=TCP port=5080 name=\"Ruisi USB DLP Lab\" mode=ENABLE scope=SUBNET",
        "sc query UsbDlp.LegacyServer",
    ])
    add_callout(
        doc,
        "Expected result",
        "The wrapper prints that the server was installed on the configured HTTP port. The service is RUNNING and the data directory is protected under All Users Application Data\\UsbDlp.",
        "pass",
    )

    add_heading(doc, "6. Install the workstation agent")
    add_steps(doc, numbering, [
        ("Keep USB storage disconnected", "No removable test media should be attached while live enforcement is installed."),
        ("Confirm server reachability", "From the workstation, ping the server address and confirm the isolated firewall path is correct."),
        ("Run the wrapper as Administrator", "Execute client\\install-client.cmd. The wrapper installs the exact XP MSI, protects agent data, stages the token, writes DryRun=0, and restarts the service."),
        ("Verify service and enrollment", "Confirm the agent is RUNNING, then inspect the log for dry_run=false, enrollment_succeeded, and policy_applied version=1."),
    ])
    add_code(doc, [
        "cd C:\\RuisiUsbDlp-legacy-xp-x86-1.1.0\\client",
        "install-client.cmd",
        "sc query UsbDlp.LegacyAgent",
        "type \"C:\\Documents and Settings\\All Users\\Application Data\\UsbDlp\\legacy-agent.log\"",
    ])
    add_callout(
        doc,
        "Fail closed at startup",
        "The agent begins with default deny even before a policy is downloaded. A missing server does not turn the agent into dry-run mode.",
        "caution",
    )

    add_heading(doc, "7. Execute the core hardware workflow")
    add_heading(doc, "7.1 Block and request access", level=2)
    add_steps(doc, numbering, [
        ("Start evidence capture", "Record time, VM snapshot name, USB manufacturer/model, VID/PID, serial number if available, and whether the device has multiple partitions."),
        ("Insert the unapproved USB stick", "Attach it to the XP workstation through the hypervisor. Do not open or write files."),
        ("Verify the block", "Within the expected interval, the notifier should appear with device details and no usable removable drive should remain."),
        ("Submit a request", "Select 1, 7, 30, or 90 days; enter a 10-2000 character business justification; select Submit request."),
        ("Verify dismissal", "The notifier should report submission and close automatically. A duplicate pending request should state that a recent request exists and also close."),
    ])
    add_code(doc, [
        "type \"C:\\Documents and Settings\\All Users\\Application Data\\UsbDlp\\legacy-agent.log\"",
        "rem Expected: blocked instance_id=USBSTOR\\...",
    ])

    add_heading(doc, "7.2 Approve the request", level=2)
    add_steps(doc, numbering, [
        ("List requests on the server", "Run server\\admin-server.cmd --requests from an elevated prompt."),
        ("Copy the pending request ID", "Match the workstation, device, and user; do not approve an unrelated request."),
        ("Approve", "Run admin-server.cmd --approve REQUEST_ID 30, replacing REQUEST_ID with the exact value."),
        ("Wait for convergence", "Allow up to one 30-second policy poll. If XP does not surface the device while attached, unplug and replug it once."),
        ("Verify use", "The device should be enabled and its expected partition or partitions should become accessible."),
    ])
    add_code(doc, [
        "admin-server.cmd --requests",
        "admin-server.cmd --approve REQUEST_ID 30",
    ])
    add_callout(
        doc,
        "Why the wrapper restarts the server",
        "The local compatibility CLI updates encrypted persisted state. admin-server.cmd briefly stops the service, applies the command, and restarts it so agents immediately receive the new policy version.",
        "note",
    )

    add_heading(doc, "7.3 Revoke and deny", level=2)
    add_steps(doc, numbering, [
        ("Revoke the approved request", "Run admin-server.cmd --revoke REQUEST_ID."),
        ("Wait for policy convergence", "Allow up to 30 seconds. If necessary, unplug and replug once."),
        ("Verify renewed blocking", "No partition from the revoked device should remain usable. Multi-volume media must expose no usable partition."),
        ("Exercise denial", "Create a fresh pending request with another device or after test-state reset, then run admin-server.cmd --deny REQUEST_ID and confirm access remains blocked."),
    ])
    add_code(doc, [
        "admin-server.cmd --revoke REQUEST_ID",
        "admin-server.cmd --deny REQUEST_ID",
    ])

    add_heading(doc, "8. Acceptance checklist")
    checklist = [
        "[ ] Both MSI files install only on XP SP3 x86 and reject the wrong OS profile.",
        "[ ] Server and agent services are RUNNING after fresh install and after rerunning each wrapper.",
        "[ ] Agent log reports dry_run=false, default_deny=true, enrollment_succeeded, and policy_applied.",
        "[ ] An unapproved USB storage device is disabled and exposes no usable partition.",
        "[ ] The notifier appears only for a newly blocked device and displays friendly name, instance ID, VID, and PID.",
        "[ ] Submit remains disabled for fewer than 10 justification characters and works at 10-2000 characters.",
        "[ ] The notifier closes after successful or duplicate-pending submission.",
        "[ ] Approval is scoped to the exact workstation and device instance and converges within one policy poll or one replug.",
        "[ ] Revocation blocks the attached device or blocks it on the first replug after policy convergence.",
        "[ ] Multi-volume USB media exposes no usable partition while blocked.",
        "[ ] Keyboard, mouse, webcam, audio, internal disks, hubs, and controllers remain usable.",
        "[ ] Stopping the XP agent is recorded as legacy_backstop_unavailable; no full service-down fail-closed claim is made.",
        "[ ] No password or enrollment token appears in process listings, command output, or collected logs.",
    ]
    add_checklist(doc, checklist)

    add_heading(doc, "9. Diagnostics and evidence collection")
    add_table(
        doc,
        ["Evidence", "Location or command"],
        [
            ("Agent log", "C:\\Documents and Settings\\All Users\\Application Data\\UsbDlp\\legacy-agent.log"),
            ("Agent configuration", "...\\Application Data\\UsbDlp\\legacy-agent.ini; redact paths if needed"),
            ("Services", "sc query UsbDlp.LegacyAgent and sc query UsbDlp.LegacyServer"),
            ("Device state", "Device Manager screenshots before insertion, blocked, approved, and revoked"),
            ("Requests", "admin-server.cmd --requests"),
            ("Workstations", "admin-server.cmd --workstations"),
            ("Windows logs", "Event Viewer System and Application entries at the test timestamps"),
            ("Package integrity", "SHA-256 verification output for SHA256SUMS"),
        ],
        [2520, 6840],
    )
    add_callout(
        doc,
        "Secret handling",
        "Do not collect admin-password.txt, enrollment-token.txt, bootstrap-token.txt, identity.dat, policy.dat, or server-state.dat in an ordinary ticket. If encrypted state is required, transfer it through an approved secure channel.",
        "risk",
    )
    add_bullets(doc, numbering, [
        "Record exact timestamps and time zone for each insertion, request, decision, and observed state change.",
        "Record whether the physical device reached XP. A missing USB or USBSTOR node is a hypervisor passthrough failure, not a DLP enforcement result.",
        "For each failure, include the previous successful step and whether unplug/replug, service restart, or VM restart changed the outcome.",
        "Return the completed acceptance checklist, screenshots, redacted logs, package hash results, VM details, and device inventory.",
    ])

    add_heading(doc, "10. Uninstall and rollback")
    add_steps(doc, numbering, [
        ("Disconnect test media", "Safely eject or unplug all removable storage before uninstalling."),
        ("Uninstall the agent", "Use Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Agent."),
        ("Uninstall the server", "Use Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Server."),
        ("Verify services are gone", "Run both sc query commands and confirm each named service no longer exists."),
        ("Preserve evidence", "The protected Application Data directory may remain for audit and reinstall. Do not delete it until evidence has been secured."),
        ("Restore the snapshot", "For a clean test rerun, restore the powered-off pre-install VM snapshots."),
    ])
    add_callout(
        doc,
        "XP service-down limitation",
        "XP lacks the modern layered installation backstop. A newly attached device can become usable while the agent service is stopped. This limitation is expected and must be reported, not hidden or described as fully fail-closed.",
        "caution",
    )

    add_heading(doc, "11. Test report summary")
    add_table(
        doc,
        ["Field", "Tester entry"],
        [
            ("Tester / organization", ""),
            ("Date, time zone", ""),
            ("XP edition / service pack", ""),
            ("Hypervisor / version", ""),
            ("Server and workstation topology", ""),
            ("USB device model / VID / PID / serial", ""),
            ("Single- or multi-volume", ""),
            ("Overall result", "PASS / FAIL / BLOCKED BY LAB"),
            ("Failed checklist item IDs", ""),
            ("Evidence archive name / SHA-256", ""),
        ],
        [3000, 6360],
    )
    paragraph = doc.add_paragraph()
    paragraph.paragraph_format.space_before = Pt(12)
    paragraph.paragraph_format.space_after = Pt(0)
    run = paragraph.add_run("End of manual")
    set_run_font(run, size=9, italic=True, color=MUTED)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    doc.save(OUTPUT)
    print(OUTPUT)


if __name__ == "__main__":
    build_manual()
