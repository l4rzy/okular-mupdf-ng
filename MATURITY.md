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
| Linux sandboxed document processing | 🟢 Stable | Security and IPC integration tests |
| Render ePUB pages | 🟡 Partial | Worker and IPC integration; loaded-generator mixed-layout fixtures, cold/cached rendering and text geometry |
| ePUB custom CSS and page sizes | 🟡 Partial | Worker tests; settings tests |
| Export ePUB to PDF | 🟡 Partial | Worker and IPC integration; loaded-generator cold/cached export and PDF reopen, images, embedded font, captions, tables, and chapter navigation |
| Unencrypted legacy MOBI support | 🟡 Partial | Synthetic corpus; detection, header rejection, descriptor cleanup, file/memory IPC, reopen, TOC, and PDF export tests; generator open/search tests |
| OCR for scanned PDF pages | 🟡 Partial | Worker and plugin integration tests |
| Outlines, links, and navigation | 🟡 Partial | Worker tests; EPUB link and outline tests 
| PDF layers (optional content) | 🟡 Partial | Worker visibility and text tests; Qt model, IPC, and plugin response validation tests |
| PDF annotations | 🟡 Partial | Generator proxy and worker tests |
| Printing and document export | 🟡 Partial | Export integration; print-output appearance, visibility, geometry and source-isolation tests; printer submission untested |
| Flattened PDF export | 🟡 Partial | Worker appearance/source-isolation tests; IPC and CLI export integration |
| Basic PDF AcroForm filling | 🟡 Partial | Worker and proxy tests; loaded-generator edit, undo/redo, save/reopen, reset, print-to-PDF, and repeated worker recovery (unsaved edit loss, history clearing, proxy reuse, and post-recovery save/reopen) tests with synthetic and LibreOffice fixtures |
| Document themes (PDF, ePUB, MOBI) | 🟡 Partial | Worker palette, image dimming/alpha/mask, dark-page detection, tile/stride, print isolation, and theme-switching tests; generator menu/settings/shortcuts and native/legacy placeholder tests |
| NSS certificate manager | 🟣 Experimental | NSS runtime; RSA/ECDSA creation, signing, PKCS#12 roundtrip, and rejection tests |
| PDF signature verification and signing | 🟣 Experimental | NSS and OpenSSL signature verification; worker tests |
| Command-line utility (mupdfng-cli) | 🟣 Experimental | CLI argument-parsing tests; XFDF end-to-end tests |
| XFDF import and export | 🟣 Experimental | Parser and generator tests; CLI integration |
| PDF JS support | 🟣 Experimental | Worker calculation and control-event tests; loaded-generator validation, cross-page dependent updates, undo/redo, and save/reopen tests |
| Generated PDF TOC (printed contents and typography; persistent cache) | 🟣 Experimental | Heading/hierarchy, wrapped titles, contents columns/page mapping, and cache tests; rotated worker fixtures and IPC reopen/precedence tests |

## Technical notes

Document themes are viewing-only and have mainly synthetic rendering coverage;
complex documents may need further tuning. Dusk dims raster images to 80%
brightness, including text in scanned pages. Vector illustrations follow the
text palette. Dark leaves mostly dark pages unchanged. Dusk leaves their vector
colors unchanged but still dims their images.

On Okular versions without the per-page placeholder color API, Dusk and Dark
use temporary accessibility **Change Paper Color** settings. The workaround
restores user preferences when no document needs it; Twilight placeholders may
remain white. See [Document Theming](README.md#document-theming) for theme choices
and controls.

XFDF import and export reject PDFs containing rotated pages, including rotation
inherited from the page tree. Correct conversion to unrotated PDF coordinates
is not implemented yet.

See the [README limitations](README.md#limitations) for current feature restrictions
and recovery behavior.

## Tracked Okular issues

| Issue | Status |
|---|---|
| [Bug 525809](https://bugs.kde.org/show_bug.cgi?id=525809) | Fix Merged |
| [Bug 525842](https://bugs.kde.org/show_bug.cgi?id=525842) | MR submitted |
| [Bug 526370](https://bugs.kde.org/show_bug.cgi?id=526370) | MR submitted |
