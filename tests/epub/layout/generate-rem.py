#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate an original, deterministic EPUB for root-relative CSS lengths."""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED, ZIP_STORED


def chapter(prefix, root_size, other):
    return f'''<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{prefix}Heading</title>
<link rel="stylesheet" href="style.css"/><style>html {{ font-size: {root_size}; }}</style>
</head><body><h1 id="start">{prefix}Heading</h1><p>{prefix}Paragraph</p>
<ul><li>{prefix}List</li></ul><div style="font-size: 2em"><p>{prefix}Nestedrem</p>
<p style="font-size: 1.5em">{prefix}Nestedem</p></div>
<p style="font-size: 16pt">{prefix}Fixed</p>
<p><a href="{other}.xhtml#start">{prefix}Link</a></p></body></html>'''


entries = {
    "mimetype": "application/epub+zip",
    "META-INF/container.xml": '''<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>''',
    "EPUB/package.opf": '''<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:mupdfng:layout:rem</dc:identifier><dc:title>Root relative lengths</dc:title><dc:language>en</dc:language><meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="one" href="one.xhtml" media-type="application/xhtml+xml"/><item id="two" href="two.xhtml" media-type="application/xhtml+xml"/><item id="css" href="style.css" media-type="text/css"/></manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>''',
    "EPUB/nav.xhtml": '''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="one.xhtml#start">OneHeading</a></li><li><a href="two.xhtml#start">TwoHeading</a></li></ol></nav></body></html>''',
    "EPUB/style.css": '''body { font-family: serif; }
p, h1 { margin: 0; line-height: 2rem; } p { font-size: 1rem; }
h1 { font-size: 1.35rem; } ul { margin: 0; padding: 0; list-style: none; }
''',
    "EPUB/one.xhtml": chapter("One", "85%", "two"),
    "EPUB/two.xhtml": chapter("Two", "125%", "one"),
}

with ZipFile(Path(__file__).with_name("rem.epub"), "w") as archive:
    for name, data in entries.items():
        info = ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = ZIP_STORED if name == "mimetype" else ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, data)
