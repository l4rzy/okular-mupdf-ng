#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic, original XPS/OpenXPS fixtures (stdlib only)."""
from pathlib import Path
import struct
import zlib
from xml.sax.saxutils import quoteattr
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parent
FONT = ROOT.parents[1] / "thirdparty/allura"
REL = "http://schemas.openxmlformats.org/package/2006/relationships"


def label(text, x=40, y=75, size=32, extra=""):
    return (f'<Glyphs FontUri="/Resources/Allura-Regular.ttf" FontRenderingEmSize="{size}" '
            f'OriginX="{x}" OriginY="{y}" Fill="#17324d" UnicodeString={quoteattr(text)} {extra}/>')


def rectangle(x, y, width, height, color, extra=""):
    return (f'<Path Fill="{color}" Data="M {x},{y} h {width} v {height} h {-width} Z" {extra}/>')


def diagram():
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))
    rows = b"".join(b"\0" + b"".join(
        bytes((35, 95, 170) if (x // 8 + y // 8) % 2 else (245, 190, 60))
        for x in range(64)) for y in range(64))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def write_fixture(name, pages):
    schema = ("http://schemas.openxps.org/oxps/v1.0" if name.endswith(".oxps")
              else "http://schemas.microsoft.com/xps/2005/06")
    content_schema = ("application/oxps" if name.endswith(".oxps") else "application/vnd.ms-package.xps")
    entries = {
        "[Content_Types].xml": (
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            f'<Default Extension="fdseq" ContentType="{content_schema}-fixeddocumentsequence+xml"/>'
            f'<Default Extension="fdoc" ContentType="{content_schema}-fixeddocument+xml"/>'
            f'<Default Extension="fpage" ContentType="{content_schema}-fixedpage+xml"/>'
            '<Default Extension="ttf" ContentType="application/vnd.ms-opentype"/>'
            '<Default Extension="png" ContentType="image/png"/>'
            '<Default Extension="txt" ContentType="text/plain"/></Types>'),
        "_rels/.rels": (f'<Relationships xmlns="{REL}"><Relationship Id="r1" '
                         f'Type="{schema}/fixedrepresentation" Target="/sequence.fdseq"/></Relationships>'),
        "sequence.fdseq": (f'<FixedDocumentSequence xmlns="{schema}">'
                            '<DocumentReference Source="/Documents/document.fdoc"/></FixedDocumentSequence>'),
        "Documents/document.fdoc": (f'<FixedDocument xmlns="{schema}">'
                                    + ''.join(f'<PageContent Source="Pages/{i}.fpage"><PageContent.LinkTargets>'
                                              f'<LinkTarget Name="page-{i}"/></PageContent.LinkTargets></PageContent>'
                                              for i in range(1, len(pages) + 1)) + '</FixedDocument>'),
        "Resources/Allura-Regular.ttf": (FONT / "Allura-Regular.ttf").read_bytes(),
        "Resources/OFL.txt": (FONT / "OFL.txt").read_bytes(),
        "Resources/checker.png": diagram(),
    }
    for i, (width, height, content) in enumerate(pages, 1):
        entries[f"Documents/Pages/{i}.fpage"] = (
            f'<FixedPage xmlns="{schema}" xml:lang="en-US" Width="{width}" Height="{height}">'
            + rectangle(0, 0, width, height, "#ffffff") + content + '</FixedPage>')
        entries[f"Documents/Pages/_rels/{i}.fpage.rels"] = (
            f'<Relationships xmlns="{REL}">'
            + ''.join(f'<Relationship Id="r{j}" Type="{schema}/required-resource" Target="/Resources/{resource}"/>'
                      for j, resource in enumerate(("Allura-Regular.ttf", "checker.png"), 1))
            + '</Relationships>')
    with ZipFile(ROOT / name, "w") as archive:
        for path, data in entries.items():
            info = ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)


text = [(600, 800,
         label("XPS searchable text fixture")
         + label("Hello XPS. Café résumé. 0123456789", y=135, size=28)
         + label("External link: example.com", y=205, size=28,
                 extra='FixedPage.NavigateUri="https://example.com/"')
         + label("Go to page two", y=275, size=28,
                 extra='FixedPage.NavigateUri="/Documents/Pages/2.fpage#page-2"')),
        (600, 800, label("Page two: internal destination", extra='Name="page-2"'))]
write_fixture("text-links.xps", text)
write_fixture("text-links.oxps", text)
write_fixture("mixed-pages.xps", [
    (400, 600, label("Portrait: 300 x 450 pt", size=26)),
    (800, 400, label("Landscape: 600 x 300 pt")),
    (480, 480, label("Square: 360 x 360 pt")),
])
write_fixture("graphics.xps", [(600, 800,
    label("Vectors, gradients and image")
    + rectangle(40, 110, 160, 100, "#e34b4b")
    + rectangle(140, 150, 160, 100, "#804b75e3")
    + '<Path Data="M 40,300 h 480 v 100 h -480 Z"><Path.Fill>'
      '<LinearGradientBrush StartPoint="40,300" EndPoint="520,300" MappingMode="Absolute">'
      '<LinearGradientBrush.GradientStops><GradientStop Color="#17324d" Offset="0"/>'
      '<GradientStop Color="#f5be3c" Offset="1"/></LinearGradientBrush.GradientStops>'
      '</LinearGradientBrush></Path.Fill></Path>'
    + '<Canvas Clip="M 40,450 h 240 v 160 h -240 Z">'
      '<Path Data="M 40,450 h 320 v 240 h -320 Z"><Path.Fill>'
      '<ImageBrush ImageSource="/Resources/checker.png" Viewbox="0,0,64,64" '
      'ViewboxUnits="Absolute" Viewport="40,450,320,240" ViewportUnits="Absolute"/>'
      '</Path.Fill></Path></Canvas>'
    + label("Clipped checkerboard image", y=670, size=28))])
