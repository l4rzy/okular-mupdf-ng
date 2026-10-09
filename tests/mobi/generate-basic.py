#!/usr/bin/env python3
"""Generate small MOBI fixtures for our adapter and worker tests."""
from pathlib import Path
import struct

root = Path(__file__).parent
seed = (root / "legacy-uncompressed.mobi").read_bytes()
first_record = struct.unpack_from(">I", seed, 78)[0]
second_record = struct.unpack_from(">I", seed, 86)[0]
html = seed[second_record:].decode("ascii").replace(
    "Searchable legacy book text.", "Searchable legacy book text. Café résumé."
)

for name, encoding, codepage, compression, version in [
    ("basic-utf8.mobi", "utf-8", 65001, 2, 6),
    ("basic-version7.mobi", "cp1252", 1252, 1, 7),
]:
    text = html.encode(encoding)
    header = bytearray(seed[first_record:second_record])
    struct.pack_into(">H", header, 0, compression)
    struct.pack_into(">I", header, 4, len(text))
    struct.pack_into(">I", header, 28, codepage)
    struct.pack_into(">I", header, 36, version)
    if compression == 2:
        # Escape non-literal bytes using PalmDOC literal runs.
        text = b"".join(bytes([1, c]) if c < 9 or c >= 128 else bytes([c]) for c in text)
    (root / name).write_bytes(seed[:first_record] + header + text)

# Each malformed fixture targets a check in MobiDocument::openFd, before MuPDF.
for name, offset, data in [
    ("magic", 60, b"BADMAGIC"),
    ("record-count", 76, struct.pack(">H", 1)),
    ("record-offset", 78, struct.pack(">I", 80)),
    ("record-order", 86, struct.pack(">I", first_record - 1)),
    ("short-record", 86, struct.pack(">I", first_record + 39)),
    ("record-magic", first_record + 16, b"BAD!"),
    ("compression", first_record, struct.pack(">H", 17480)),
    ("encrypted", first_record + 12, struct.pack(">H", 1)),
    ("kf8", first_record + 36, struct.pack(">I", 8)),
]:
    malformed = bytearray(seed)
    malformed[offset:offset + len(data)] = data
    (root / f"invalid-{name}.mobi").write_bytes(malformed)

(root / "invalid-truncated.mobi").write_bytes(seed[:90])
(root / "invalid-truncated-record.mobi").write_bytes(seed[:first_record + 39])
