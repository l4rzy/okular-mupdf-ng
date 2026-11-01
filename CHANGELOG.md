Changes since 0.5.0.

### Highlights

- Added experimental support for DRM-free AZW3 and single-container KFX books,
  with ebook export options in Okular and the command-line tool.
- Added configurable previews for internal links, including resizable and
  pinnable preview windows.
- Added per-document theme, ebook layout, and PDF OCR settings, with an option
  to restore global defaults.
- Added XFDF/XML annotation import with undo and redo support.

### Fixes

- Improved ebook opening speed, image and text layout, navigation, and
  asynchronous PDF export.
- Fixed tiled rendering artifacts and improved native PDF printing.
- Preserved passwords when reopening encrypted documents after sandbox changes.
- Improved cache handling and resource cleanup in ebook export, PDF signing,
  OCR, and sandbox setup.

### Build and documentation

- Added Arch Linux OBS packaging and Rust dependencies for Kindle support.
- Added LLVM coverage reports and SonarCloud analysis, and expanded CI and
  regression tests.
- Updated distribution recipes, documentation, and translations. Debian and
  Ubuntu packages are now distributed through OBS.
