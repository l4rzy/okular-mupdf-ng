#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate an original, two-page AcroForm fixture without third-party content."""
from pathlib import Path


def stream(body, extra=""):
    return f"<< /Length {len(body.encode('ascii'))} {extra} >>\nstream\n{body}\nendstream"


def widget(field, rect, page=3):
    return f"<< /Type /Annot /Subtype /Widget /F 4 /P {page} 0 R /Rect [{rect}] {field} >>"


objects = [
    "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
    "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /Helv 6 0 R >> >> /Contents 17 0 R /Annots [7 0 R 9 0 R 10 0 R 11 0 R 13 0 R 14 0 R 16 0 R] >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 90 /Resources << /Font << /Helv 6 0 R >> >> /Contents 18 0 R /Annots [8 0 R 12 0 R] >>",
    "<< /Fields [7 0 R 8 0 R 9 0 R 10 0 R 13 0 R 14 0 R 15 0 R 16 0 R] /DR << /Font << /Helv 6 0 R >> >> /DA (/Helv 12 Tf 0 g) /NeedAppearances true >>",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    widget("/FT /Tx /T (Name) /V (Original) /DV (Original) /MaxLen 16", "50 660 300 690"),
    widget("/FT /Tx /T (Notes) /Ff 4096 /V (Initial notes)", "50 500 350 600", 4),
    widget("/FT /Tx /T (Reference) /Ff 1 /V (Protected)", "50 600 300 630"),
    widget("/FT /Btn /T (Agree) /V /Off /AS /Off /AP << /N << /Off 19 0 R /Yes 20 0 R >> >>", "50 550 70 570"),
    widget("/Parent 15 0 R /AS /Email /AP << /N << /Off 19 0 R /Email 20 0 R >> >>", "50 500 70 520"),
    widget("/Parent 15 0 R /AS /Off /AP << /N << /Off 19 0 R /Post 20 0 R >> >>", "50 400 70 420", 4),
    widget("/FT /Ch /T (Languages) /Ff 2097152 /Opt [[(en) (English)] [(fr) (French)] [(de) (German)]] /V [(en)] /I [0]", "50 350 250 440"),
    widget("/FT /Ch /T (City) /Ff 393216 /Opt [(London) (Paris)] /V (London)", "50 280 250 310"),
    "<< /FT /Btn /T (Delivery) /Ff 32768 /V /Email /DV /Email /Kids [11 0 R 12 0 R] >>",
    widget("/FT /Btn /T (Reset) /Ff 65536 /A << /S /ResetForm >> /MK << /CA (Reset) >>", "50 220 150 250"),
    stream("BT /Helv 18 Tf 50 740 Td (Application form) Tj ET"),
    stream("BT /Helv 18 Tf 50 740 Td (Additional details) Tj ET"),
    stream("0 0 20 20 re S", "/Type /XObject /Subtype /Form /BBox [0 0 20 20] /Resources << >>"),
    stream("0 0 20 20 re S 2 2 m 18 18 l 2 18 m 18 2 l S", "/Type /XObject /Subtype /Form /BBox [0 0 20 20] /Resources << >>"),
]
data = bytearray(b"%PDF-1.7\n")
offsets = []
for number, body in enumerate(objects, 1):
    offsets.append(len(data))
    data.extend(f"{number} 0 obj\n{body}\nendobj\n".encode("ascii"))
xref = len(data)
data.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode("ascii"))
for offset in offsets:
    data.extend(f"{offset:010} 00000 n \n".encode("ascii"))
data.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
Path(__file__).with_name("application.pdf").write_bytes(data)
