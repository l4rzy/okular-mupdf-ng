#!/usr/bin/env python3
"""Generate synthetic legacy MOBI TOC fixtures; no third-party book content."""
from pathlib import Path
import struct

root = Path(__file__).parent
html = '''<html><head><guide><reference type="toc" filepos="TOC_OFFSET"/></guide></head>
<body><p>Intro café.</p><mbp:pagebreak/>
<p id="first" style="page-break-before:always"><font size="7"><b>First café</b></font></p><p>First chapter text.</p>
<mbp:pagebreak/><p id="detail" style="page-break-before:always"><b>Detail</b></p><p>Nested section text.</p>
<mbp:pagebreak/><p id="second" style="page-break-before:always"><b>Second</b></p><p>Second chapter text.</p>
<mbp:pagebreak/><p id="contents" style="page-break-before:always">Contents</p>
<ul><li><a filepos="FIRST_POS_"><b>First café</b></a><ul><li><a filepos="DETAIL_POS">Detail</a></li></ul></li>
<li><a filepos="SECOND_POS">Second</a></li>
<li><a filepos="9999999999">Invalid overflow</a></li><li><a filepos="0000999999">Outside text</a></li>
<li><a filepos="bad-offset">Invalid number</a></li></ul>
<mbp:pagebreak/><p><a href="#second">Not part of the TOC</a></p></body></html>'''
html = html.replace('First chapter text.', 'First chapter text. ' + 'Book text for pagination. ' * 300)
html = html.replace('Nested section text.', 'Nested section text. ' + 'Section text for pagination. ' * 300)

for name, encoding, codepage, compression in [
    ('toc-uncompressed.mobi', 'utf-8', 65001, 1),
    ('toc-palmdoc.mobi', 'utf-8', 65001, 2),
    ('toc-cp1252.mobi', 'cp1252', 1252, 2),
    ('toc-link-start.mobi', 'utf-8', 65001, 2),
]:
    text = html.encode(encoding)
    toc_start = b'<a filepos="FIRST_POS_' if name == 'toc-link-start.mobi' else b'<p id="contents"'
    for marker, target in [('TOC_OFFSET', toc_start), ('FIRST_POS_', b'<p id="first"'),
                           ('DETAIL_POS', b'<p id="detail"'), ('SECOND_POS', b'<p id="second"')]:
        text = text.replace(marker.encode(), f'{text.index(target):010d}'.encode())
    seed = (root / 'legacy-uncompressed.mobi').read_bytes()
    database = bytearray(seed[:78])
    header = bytearray(seed[96:344])
    chunks = [text[i:i + 4096] for i in range(0, len(text), 4096)]
    struct.pack_into('>H', header, 0, compression)
    struct.pack_into('>I', header, 4, len(text))
    struct.pack_into('>H', header, 8, len(chunks))
    struct.pack_into('>I', header, 28, codepage)
    if compression == 2:
        # Encode non-literal bytes with PalmDOC's literal-run escape.
        chunks = [b''.join(bytes([1, c]) if c < 9 or c >= 128 else bytes([c]) for c in chunk)
                  for chunk in chunks]
    records = [bytes(header)] + chunks
    struct.pack_into('>H', database, 76, len(records))
    offset = 78 + len(records) * 8 + 2
    table = bytearray()
    for record in records:
        table.extend(struct.pack('>II', offset, 0))
        offset += len(record)
    (root / name).write_bytes(database + table + b'\0\0' + b''.join(records))
