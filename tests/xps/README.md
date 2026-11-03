# XPS fixtures

These small, synthetic documents are ready to open in Okular with the MuPDF backend.

| File | Pages | Coverage |
|---|---:|---|
| `text-links.xps` | 2 | Embedded font, searchable text, accented characters, external HTTPS link, internal link to page two |
| `text-links.oxps` | 2 | The same content using the OpenXPS namespace and content types |
| `mixed-pages.xps` | 3 | Portrait (300 × 450 pt), landscape (600 × 300 pt), and square (360 × 360 pt) pages |
| `graphics.xps` | 1 | Vector rectangles, transparency, linear gradient, clipping, and an embedded PNG checkerboard |

Regenerate from the repository root:

```sh
python3 tests/xps/generate.py
```

The generator uses only the Python standard library and produces deterministic ZIP packages. Text and graphics are original. Every package includes the repository's Allura font and its SIL Open Font License from `thirdparty/allura/`.

The worker XPS suite opens each fixture, renders every page, checks text extraction, verifies link targets, and checks mixed page dimensions. These fixtures exercise specific features; they are not a format conformance corpus.
