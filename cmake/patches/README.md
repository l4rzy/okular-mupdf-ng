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

`mupdf-1.28.5-html-layout.patch` measures inline images using the same
baseline offsets as placement, reserving both ascent and descent for vertical
alignment. Line-relative top/bottom images impose a minimum line height. This
prevents images aligned to the middle, text edges, or explicit offsets from
overlapping adjacent blocks or extending beyond page boundaries. Table-driven
geometry coverage lives in `tests/worker/epub.cpp` (`testImageVerticalAlignment`)
and opens synthetic HTML directly without plugin CSS overrides.
The plugin's EPUB layout cache revision also changes so cached page counts and
outline positions from the unpatched engine are not reused.

The same patch preserves CSS `rem` as a distinct unit until layout, resolving
it against the computed font size of the document element rather than a fixed
16 points. The root's own `rem` font size uses the initial layout font size.
The synthetic page box is kept separate from the document root, and each
chapter/layout owns its root size. Font sizes, spacing, dimensions, line heights
and vertical alignment share the corrected conversion; `em`, percentages and
absolute lengths retain their existing behavior. The two HTML fixes are combined
because they touch the same conversion sites and must remain independently
verifiable from the other patches on reconfiguration.
Worker coverage includes inline document roots, repeated relayout, page margins,
images, tables and restartable stories. A small original EPUB covers separate chapter roots through
the actual plugin, cold/warm caches, PDF export and reopen. Cache revision 2
invalidates accelerators and outlines laid out before the `rem` correction.

`mupdf-1.28.5-form-autosize.patch` fits auto-sized plain form values to the
widget width and height using the renderer's word wrapping and CR, LF and CRLF
line breaks. Multiline values retain the existing 12-point ceiling and shrink
only when the wrapped layout does not fit. Explicit PDF font
sizes, single-line fields and rich text styles retain their existing behavior.
Worker regression coverage uses synthetic Latin and Japanese text to check line
endings, empty lines, paragraphs with and without room to wrap, long values,
single-line fitting and explicit sizes.

`mupdf-1.28.5-radio-unison.patch` makes radio-button clicks honor
`RadiosInUnison`: without it, only the clicked widget is selected; with it,
all widgets sharing the selected on-state are selected. Checkboxes retain
their existing behavior. Worker tests cover duplicate values on one or more
pages, toggle-to-off rules, idempotent updates, and save/reopen. The generator
separates repeated in-unison options by occurrence instead of page, so Qt's
exclusive groups never contain two copies of the same selected option.
IPC compatibility 0.4.2 carries the flag to the generator.
Okular records undo per UI group, so uneven sets of mirrored options can still
omit the previous selection in another group from undo history. Selection and
persistence follow the PDF flag; fixing that undo limitation requires host support.

`mupdf-1.28.5-html-image-float.patch` supports left/right floating HTML images,
including their margins, line-by-line text wrapping across paragraph and container
boundaries, `clear`, and image pagination. Float overflow contributes to document
height and remains drawable when the enclosing paragraph ends on an earlier page.
This fixes decorative chapter initials in Gutenberg EPUBs such as *Round the Globe*.
The scope is static image elements in non-restarting ebook layout. Images in
tables or positioned containers use the existing block-image path: table rows may
be laid out repeatedly, and positioned boxes move after layout. Tables isolate
external float exclusions. Arbitrary floated containers and floats in restartable
stories remain unsupported. Multiple image floats stack vertically. Missing-image
placeholders keep their normal-flow height, and hidden images are skipped before
loading. Non-finite image/margin geometry is rejected; clearance requires strict
forward progress. The list borrows tree-owned boxes and resets for every layout.
Table-driven tests cover both sides, inline/block display, clearing, pagination,
repeated reflow, row retries, positioned/hidden/missing images, zero dimensions,
negative and overflowing margins, and multiple floats. The EPUB layout cache
revision changes to invalidate pagination from the previous engine.

`mupdf-1.28.5-html-background.patch` expands a lone `background: none` to a
transparent background color, matching the keyword without regard to case.
Previously the image keyword reached the color parser's fallback and became
opaque black, hiding black text in tables such as
Cloud Native Data Center Networking's table 10-1. Table-driven rendering tests
in `tests/worker/epub.cpp` cover transparency, resetting an earlier color,
keyword case, declaration precedence, `!important`, and explicit named/hex colors.
Pagination is unchanged.

`mupdf-1.28.5-css-attribute-selectors.patch` adds prefix (`^=`), suffix (`$=`),
and substring (`*=`) attribute selectors. It also fixes dash-prefix (`|=`)
matching and whitespace-token (`~=`) matching, including class selectors,
and accepts whitespace before attribute values. Empty substring needles never
match. Worker tests cover missing/empty attributes, case-sensitive matching,
Unicode, compound/descendant selectors, specificity, malformed-selector recovery,
long values, and the book's footnote selector. Cache revision 6 invalidates layouts
from before these rules applied.
Attribute namespaces and the explicit `i`/`s` case flags remain unsupported.

`mupdf-1.28.5-css-calc.patch` is retained for reference but is not applied by
CMake. CSS `calc()` and viewport units remain unsupported by this build.

CMake applies the patches with `patch -p1 --fuzz=0` for bundled builds. Reconfiguration
verifies an already-applied patch using a reverse dry run. Unexpected source changes
fail configuration rather than silently skipping the fix. The patch and affected
source files are build dependencies so incremental builds refresh the library.
`USE_SYSTEM_MUPDF=ON` leaves the installed library untouched.

The changes modify MuPDF code licensed under AGPL-3.0-or-later; the original file
headers and licensing terms remain in the source tree. Regression coverage lives in
`tests/worker/mobi.cpp` and `tests/generator/test_layers_model.cpp`.
