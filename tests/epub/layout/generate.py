#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate deterministic, original EPUB 3 layout fixtures (stdlib only)."""
from pathlib import Path
import struct
import zlib
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED, ZIP_STORED

ROOT = Path(__file__).resolve().parent
FONT = ROOT.parents[2] / "thirdparty/allura"


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    # Original blue diagram, with two white boxes and a connecting line.
    rows = bytearray()
    for y in range(96):
        rows.append(0)
        for x in range(180):
            white = (15 <= y < 80 and (12 <= x < 65 or 115 <= x < 168)) or (46 <= y < 50 and 65 <= x < 115)
            rows.extend((255, 255, 255) if white else (35, 95, 170))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 180, 96, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def chapter(title, body):
    return f'''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{title}</title>
<link rel="stylesheet" href="style.css"/></head><body><h1 id="start">{title}</h1>{body}</body></html>'''


for alignment in ("middle", "top"):
    entries = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '''<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>''',
        "EPUB/package.opf": '''<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:mupdfng:layout:ALIGN</dc:identifier><dc:title>Layout ALIGN</dc:title><dc:language>en</dc:language><meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="one" href="one.xhtml" media-type="application/xhtml+xml"/><item id="two" href="two.xhtml" media-type="application/xhtml+xml"/><item id="css" href="style.css" media-type="text/css"/><item id="image" href="diagram.png" media-type="image/png"/><item id="font" href="Allura-Regular.ttf" media-type="font/ttf"/><item id="license" href="OFL.txt" media-type="text/plain"/></manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>'''.replace("ALIGN", alignment),
        "EPUB/nav.xhtml": '''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="one.xhtml#start">Diagram chapter</a></li><li><a href="two.xhtml#start">Table chapter</a></li></ol></nav></body></html>''',
        "EPUB/style.css": '''@font-face { font-family: Fixture; src: url(Allura-Regular.ttf); }
body { font-family: serif; } h1 { font-family: Fixture; font-weight: normal; }
p { margin: 0; } img { width: 180px; height: 96px; vertical-align: ALIGN; }
table { border-collapse: collapse; } td, th { border: 1px solid black; padding: 4px; }
'''.replace("ALIGN", alignment),
        "EPUB/one.xhtml": chapter("Diagram chapter", '''<p>Before diagram.</p><p>Figure <img src="diagram.png" alt="Two connected boxes"/></p><p>Caption beneath diagram.</p><p>After caption.</p><p><a href="two.xhtml#start">Continue to table</a></p>'''),
        "EPUB/two.xhtml": chapter("Table chapter", '''<table><caption>Table caption</caption><tr><th>Item</th><th>Count</th></tr><tr><td><img src="diagram.png" alt="Connected boxes"/></td><td>Seven</td></tr><tr><td>Summary</td><td>Complete</td></tr></table><p>After table.</p><p><a href="one.xhtml#start">Return to diagram</a></p>'''),
        "EPUB/diagram.png": png(),
        "EPUB/Allura-Regular.ttf": (FONT / "Allura-Regular.ttf").read_bytes(),
        "EPUB/OFL.txt": (FONT / "OFL.txt").read_bytes(),
    }
    with ZipFile(ROOT / f"{alignment}.epub", "w") as archive:
        for name, data in entries.items():
            info = ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = ZIP_STORED if name == "mimetype" else ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
