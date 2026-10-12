# Bundled MuPDF patches

`mupdf-1.28.5-mobi-toc.patch` targets the source archive pinned in `cmake/mupdf.version`.
It adds anchors for legacy MOBI byte-offset links before text transcoding, converts
`filepos` links to anchor links, and reads the guide's explicit TOC. Nested lists and
blockquotes form the outline hierarchy; a page-break ends the TOC. Existing heading
outlines remain the fallback.
Skipped nesting levels preserve the preceding parent's children, and offsets
inside comments wait until the comment closes before inserting a destination.
The reader also clears dropped record streams, frees extracted text on
transcoding failure, and releases the entry tree if archive construction fails.
Synthetic TOC fixtures and allocation-failure tests in `tests/worker/mobi.cpp`
cover these paths, including under AddressSanitizer and LeakSanitizer.

`mupdf-1.28.5-ocr-font-release.patch` releases the OCR device's owned Courier
font reference during destruction. Text objects and target devices retain their
own references. Existing image OCR cases in `tests/worker/ocr.cpp` exercise this
cleanup under LeakSanitizer.

`mupdf-1.28.5-parent-tree-lifetime.patch` retains the original tagged-PDF parent
number tree while page rearrangement replaces `/ParentTree` and copies retained
pages' entries. This prevents use-after-free of the borrowed dictionary value.
Coverage lives in `tests/worker/document.cpp` (`flattenTaggedPageSelection`).

`mupdf-1.28.5-image-vertical-align.patch` measures inline images using the same
baseline offsets as placement, reserving both ascent and descent for vertical
alignment. Line-relative top/bottom images impose a minimum line height. This
prevents images aligned to the middle, text edges, or explicit offsets from
overlapping adjacent blocks or extending beyond page boundaries. Table-driven
geometry coverage lives in `tests/worker/epub.cpp` (`testImageVerticalAlignment`)
and opens synthetic HTML directly without plugin CSS overrides.
The plugin's EPUB layout cache revision also changes so cached page counts and
outline positions from the unpatched engine are not reused.

CMake applies the patches with `patch -p1 --fuzz=0` for bundled builds. Reconfiguration
verifies an already-applied patch using a reverse dry run. Unexpected source changes
fail configuration rather than silently skipping the fix. The patch and affected
source files are build dependencies so incremental builds refresh the library.
`USE_SYSTEM_MUPDF=ON` leaves the installed library untouched.

The changes modify MuPDF code licensed under AGPL-3.0-or-later; the original file
headers and licensing terms remain in the source tree. Regression coverage lives in
`tests/worker/mobi.cpp` and `tests/generator/test_layers_model.cpp`.
