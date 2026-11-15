# Bundled MuPDF patches

These patches target the MuPDF version pinned in `cmake/mupdf.version`.

- `mupdf-1.28.5-mobi-toc.patch`: Resolves legacy MOBI byte-offset links and
  builds outlines from explicit tables of contents. Fixes cleanup on import errors.
- `mupdf-1.28.5-ocr-font-release.patch`: Releases the OCR device's owned font
  reference when the device is destroyed.
- `mupdf-1.28.5-parent-tree-lifetime.patch`: Keeps the tagged-PDF parent tree
  alive while page rearrangement copies its entries, preventing use-after-free.
- `mupdf-1.28.5-html-layout.patch`: Corrects inline image alignment, inline-block
  flow, `rem` units, table cell sizing, and image width measurement. Applies
  `max-width` during block, image, and table layout.
- `mupdf-1.28.6-html-max-width.patch`: Adds CSS `max-width` parsing and its
  generated property lookup table. Layout support lives in the HTML layout patch.
- `mupdf-1.28.5-html-table-pagination.patch`: Realigns collapsed-border cells
  after a row moves to another page, keeping backgrounds and borders aligned.
- `mupdf-1.28.5-html-media.patch`: Filters linked and inline stylesheets by screen
  media type. Viewport feature queries and CSS `@media` rules remain unsupported.
- `mupdf-1.28.5-html-image-width.patch`: Preserves natural image aspect ratios
  and constrains block images to the available width.
- `mupdf-1.28.5-html-image-float.patch`: Adds left/right image floats, text
  wrapping, clearing, and pagination. Tables isolate external floats. Arbitrary
  floated containers and floats in restartable stories remain unsupported.
- `mupdf-1.28.5-html-background.patch`: Parses background colors in shorthand
  declarations without mistaking other components for colors. Background image
  painting and the complete shorthand grammar remain unsupported.
- `mupdf-1.28.5-css-attribute-selectors.patch`: Adds prefix, suffix, and substring
  selectors and corrects token and dash-prefix matching. Attribute namespaces
  and explicit case flags remain unsupported.
- `mupdf-1.28.5-form-autosize.patch`: Fits auto-sized plain form text to widget
  bounds using wrapping and line breaks. Preserves explicit font sizes.
- `mupdf-1.28.5-radio-unison.patch`: Honors `RadiosInUnison` when selecting and
  saving radio buttons. Undo across mirrored UI groups still requires host support.

`mupdf-1.28.5-css-calc.patch` is kept for reference and is not applied.
CSS `calc()` and viewport units remain unsupported.

CMake applies patches with `patch -p1 --fuzz=0` and verifies existing patches
with a reverse dry run. Unexpected source changes fail configuration. Patches
and affected sources are build dependencies, so incremental builds refresh MuPDF.
`USE_SYSTEM_MUPDF=ON` leaves the installed library untouched.

Regression tests live in `tests/worker/`, with additional generator and integration
coverage. EPUB layout changes require a cache revision update to discard stale
pagination and outlines.

The patched MuPDF code is licensed under AGPL-3.0-or-later. Original license
headers remain in the source tree.
