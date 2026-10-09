The `legacy-*.mobi` fixtures contain two Palm database records: a MOBI 6 header and ASCII HTML text. The uncompressed fixture uses compression 1; the PalmDOC fixture uses compression 2 with space/character pairs compressed. Both contain a title, searchable text, and an external link. All fixtures are synthetic; no third-party book content is included.

Run `python3 tests/mobi/generate-basic.py` to regenerate the following adapter corpus from `legacy-uncompressed.mobi`:

| Fixtures | Purpose |
|---|---|
| `basic-utf8.mobi` | Version 6, PalmDOC compression, UTF-8; detection, file opening, and asynchronous export |
| `basic-version7.mobi` | Version 7, uncompressed Windows-1252; detection, memory opening, and synchronous export |
| `invalid-magic.mobi`, `invalid-truncated.mobi` | Database header rejection |
| `invalid-record-count.mobi`, `invalid-record-offset.mobi`, `invalid-record-order.mobi`, `invalid-short-record.mobi` | Record table rejection |
| `invalid-record-magic.mobi`, `invalid-truncated-record.mobi` | First record header rejection |
| `invalid-compression.mobi`, `invalid-encrypted.mobi`, `invalid-kf8.mobi` | Unsupported compression, DRM, and newer format rejection |

The existing worker suite checks our error messages, descriptor ownership, clearing an already-open document after rejection, and successful reopening. File and memory IPC tests reject an encrypted fixture before opening and exporting a valid book. These fixtures exercise our adapter and transport paths; they do not establish MuPDF rendering fidelity or real-book compatibility.

The `toc-*.mobi` fixtures contain multiple text records, styled paragraphs without semantic headings, and a guide pointing to a nested TOC with `filepos` links. Invalid destinations and a link after the TOC's page-break boundary exercise filtering. They cover uncompressed UTF-8, PalmDOC UTF-8, and PalmDOC Windows-1252 text, including a multibyte character before the destinations. Regenerate them with `python3 tests/mobi/generate-toc.py`.
