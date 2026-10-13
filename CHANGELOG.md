# Release notes

## 0.4.0

Changes since 0.3.0, including the 0.3.1–0.3.8 releases.

### Added

- Support for unencrypted legacy MOBI documents, including search, navigation,
  and PDF export. Modern KF8 and encrypted MOBI are unsupported.
- Experimental PDF table-of-contents generation from printed contents pages
  and heading typography, with persistent caching and a setting to disable it.
- Optional cancellation of obsolete page renders through shared memory,
  configurable at runtime.
- Tools menu actions for OCR settings and the NSS certificate manager.
- Manual cache cleanup in the backend settings.
- Signature emblem choices: Okular, MuPDF, or none. PDF signatures now include
  the supplied signing location and reason in their metadata.

### Improved and fixed

- Fixed EPUB inline-image vertical alignment that could overlap captions or
  adjacent text. Invalidated older EPUB layout caches to refresh pagination
  and outline positions. This fix requires the bundled, patched MuPDF.
- Improved legacy MOBI table-of-contents links and hierarchy in bundled MuPDF.
- Improved PDF text selection and dehyphenation.
- Fixed blank pages caused by incomplete pixmap refreshes and corrected worker
  transport ordering, stale-client handling, and lifetime issues.
- Tightened worker extraction budgets and protocol limits, and improved render
  frame ownership and cancellation handling.
- OCR now processes image regions; improved cancellation accounting and only
  reports success when results are available.
- Fixed an OCR font leak and a tagged-PDF page-rearrangement use-after-free in
  bundled MuPDF.
- XFDF import and export now reject PDFs containing rotated pages, including
  inherited rotation, to prevent incorrect annotation placement.
- Added Qt 6.12 compatibility fixes and reduced unnecessary generator locking.
- Improved backend diagnostics, including reporting bundled versus system
  MuPDF, and refreshed artwork, documentation, and Vietnamese translations.

### Build and packaging

- Enabled supported LTO for optimized CMake configurations; debug and sanitizer
  builds remain excluded. Debian 13 CI disables LTO, while Arch CI enables it
  through makepkg.
- Renamed the worker executable to `mupdfng-worker` and updated cache paths.
- Removed KDE Frameworks dependencies from the command-line utility.
- Release CI now requires the sanitizer job to pass.
- Arch CI builds and tests both bundled and system MuPDF variants, but publishes
  only the bundled package so releases include the project's MuPDF patches.

### Test coverage

- Added actual Okular-plugin form tests for editing, validation, JavaScript
  calculations, dependent fields, undo/redo, and save/reopen.
- Added repeated worker-recovery tests for unsaved forms, pending redo,
  encrypted documents, saved baselines, and JavaScript dependencies.
- Added redistributable EPUB fixtures covering images, captions, tables,
  embedded fonts, and chapter links through plugin rendering, caches, PDF
  export, and reopen, plus 288 inline-image alignment cases.
- Expanded legacy MOBI, generated PDF outlines, OCR, cache cleanup, signatures,
  XFDF, and worker-boundary regression coverage.

Worker recovery discards unsaved form edits;
JavaScript forms, generated PDF outlines, signatures, and XFDF remain
experimental. See [MATURITY.md](MATURITY.md) for feature status and coverage.
