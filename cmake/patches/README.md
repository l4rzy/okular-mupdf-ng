# Bundled MuPDF patches

`mupdf-1.28.5-mobi-toc.patch` targets the source archive pinned in `cmake/mupdf.version`.
It adds anchors for legacy MOBI byte-offset links before text transcoding, converts
`filepos` links to anchor links, and reads the guide's explicit TOC. Nested lists and
blockquotes form the outline hierarchy; a page-break ends the TOC. Existing heading
outlines remain the fallback.

CMake applies the patch with `patch -p1 --fuzz=0` for bundled builds. Reconfiguration
verifies an already-applied patch using a reverse dry run. Unexpected source changes
fail configuration rather than silently skipping the fix. The patch and affected
source files are build dependencies so incremental builds refresh the library.
`USE_SYSTEM_MUPDF=ON` leaves the installed library untouched.

The changes modify MuPDF code licensed under AGPL-3.0-or-later; the original file
headers and licensing terms remain in the source tree. Regression coverage lives in
`tests/worker/mobi.cpp` and `tests/generator/test_layers_model.cpp`.
