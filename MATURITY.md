# Feature Maturity

This page summarizes the maturity of the features users can access in
okular-mupdf-ng and the kind of automated testing currently present for them.

## Ratings

- 🟢 **Stable** — intended for regular use within the listed capability; well tested
and observed stable on different Linux platforms.
- 🟡 **Partial** — usable, but not thoroughly tested with a large PDF corpus.
- 🟣 **Experimental** — available for evaluation; tested locally with a very small
corpus; might missing some features.

The test column describes the kind of automated coverage present, not its
thoroughness. Worker tests exercise document-engine behavior; integration tests
exercise communication across the worker boundary. A test label is not a
promise that every input or workflow has been tested.

| Feature | Maturity | Automated test coverage |
|---|---|---|
| Open PDF and ePUB documents | 🟢 Stable | Worker and IPC integration |
| Render PDF pages | 🟢 Stable | Worker and IPC integration |
| Text extraction and search | 🟢 Stable | Worker tests; text-page conversion tests |
| Metadata, fonts, and embedded files | 🟢 Stable | Worker and generator tests |
| Built-in Linux sandbox | 🟢 Stable | Security and IPC integration tests |
| PDF layer viewing and toggling | 🟢 Stable | Rendering and text visibility, locked and radio-group layers, IPC validation, and selection replay after worker recovery |
| Flattened PDF export | 🟢 Stable | Annotation and form appearance after reopening, source isolation, atomic IPC output, and CLI export tests |
| Optional Bubblewrap launcher on Linux | 🟡 Partial | Filesystem and network isolation, worker IPC, sandbox reporting, and startup failure tests |
| Render ePUB pages | 🟡 Partial | Worker and IPC integration; loaded-generator mixed-layout fixtures, cold/cached rendering and text geometry |
| ePUB custom CSS and page sizes | 🟡 Partial | Worker tests; settings tests |
| Export ePUB to PDF | 🟡 Partial | Worker and IPC integration; loaded-generator cold/cached export and PDF reopen, images, embedded font, captions, tables, and chapter navigation |
| Unencrypted legacy MOBI support | 🟡 Partial | Synthetic corpus; detection, header rejection, descriptor cleanup, file/memory IPC, reopen, TOC, and PDF export tests; generator open/search tests |
| XPS/OpenXPS viewing | 🟡 Partial | Synthetic fixtures for rendering, searchable text, links, outlines, fonts, and mixed page sizes, plus worker IPC coverage |
| OCR for scanned PDF pages | 🟡 Partial | Worker and plugin integration tests |
| Outlines, links, and navigation | 🟡 Partial | Worker tests; EPUB link and outline tests |
| Link preview on hover | 🟡 Partial | Generator sizing, hover/dismissal, scrolling/dragging, cancellation/cache, navigation, and worker rendering integration tests |
| PDF annotations | 🟡 Partial | Generator proxy and worker tests |
| Printing and document export | 🟡 Partial | Export integration; print-output appearance, visibility, geometry and source-isolation tests; printer submission untested |
| Basic PDF AcroForm filling | 🟡 Partial | Worker and proxy tests; loaded-generator edit, undo/redo, save/reopen, reset, print-to-PDF, and repeated worker recovery (unsaved edit loss, history clearing, proxy reuse, and post-recovery save/reopen) tests with synthetic and LibreOffice fixtures |
| Document themes (PDF, ePUB, MOBI) | 🟡 Partial | Worker palette, image dimming/alpha/mask, dark-page/image detection, blend-mode fallback, tile/stride, print isolation, and theme-switching tests; generator menu/settings/shortcuts and native/legacy placeholder tests |
| Command-line utility (mupdfng-cli) | 🟡 Partial | Argument parsing and end-to-end tests for OCR, PDF flattening, XFDF, ebook conversion, Markdown export, and sandbox reporting |
| XFDF annotation import and export (PDFs without rotated pages) | 🟡 Partial | Parser tests, Edit-menu import, undo/redo, save/reopen, view rotation, annotation geometry, and CLI round trips |
| DRM-free AZW3 and single-container KFX support | 🟣 Experimental | Worker and IPC integration; format detection, invalid input, rendering, text extraction, PDF export, and AZW3 styling/navigation fixtures |
| NSS certificate manager | 🟣 Experimental | NSS runtime, RSA/ECDSA certificate creation, PKCS#12 round trips, and rollback on failed imports |
| PDF signature verification and signing | 🟣 Experimental | NSS and OpenSSL verification, invalid byte ranges, OCSP revocation, and signing through worker IPC |
| PDF JS support | 🟣 Experimental | Worker calculation and control-event tests; loaded-generator validation, cross-page dependent updates, undo/redo, and save/reopen tests |
| Generated PDF TOC (printed contents and typography; persistent cache) | 🟣 Experimental | Heading/hierarchy, wrapped titles, contents columns/page mapping, and cache tests; rotated worker fixtures and IPC reopen/precedence tests |

## Technical notes

Document themes are viewing-only and have mainly synthetic rendering coverage;
complex documents may need further tuning. Dusk dims raster images to 80%
brightness, including text in scanned pages. Vector illustrations follow the
text palette. Dark always recolors vectors, inverts lighter images, and dims
mostly dark images to 80% without inversion. Image decisions use the whole image independently
of zoom or tiles, with transparency taken into account. Dusk preserves vector colors
only when both the page and its border are mostly dark, but still dims images.
Light vector text over a preserved dark image may lose contrast in Dark mode.

Dark falls back to transforming the completed page when non-normal blend modes
or knockout groups are present. This preserves native
compositing, but selective image inversion and image dimming are unavailable on
those pages. Multiply blending in PDF highlights does not trigger this fallback,
so highlighted text retains contrast. Other blends on the same page can still
require the fallback.

On Okular versions without the per-page placeholder color API, Dusk and Dark
use temporary accessibility **Change Paper Color** settings. The workaround
restores user preferences when no document needs it; Twilight placeholders may
remain white. See the guide for theme choices and controls.
[Document Theming](https://l4rzy.github.io/okular-mupdf-ng/guide.html#document-themes)

XFDF import and export reject PDFs containing rotated pages, including rotation
inherited from the page tree. Correct conversion to unrotated PDF coordinates
is not implemented yet.

See the [README limitations](README.md#limitations) for current feature restrictions
and recovery behavior.
