# okular-mupdf-ng

[![GitHub license](https://img.shields.io/github/license/l4rzy/okular-mupdf-ng)](https://github.com/l4rzy/okular-mupdf-ng/blob/main/COPYING)
[![GitHub commit activity (branch)](https://img.shields.io/github/commit-activity/m/l4rzy/okular-mupdf-ng)](https://github.com/l4rzy/okular-mupdf-ng/commits)
[![GitHub Workflow Status (with event)](https://img.shields.io/github/actions/workflow/status/l4rzy/okular-mupdf-ng/ci.yml)](https://github.com/l4rzy/okular-mupdf-ng/actions)

A secure and fast PDF and EPUB generator for Okular.
> Beta: Real-world testers are welcome. Please report any issue via Github Issues.

![Screenshot](screenshot.png)
---

## How to install
Prebuilt packages for common KDE distros are available for download [here](https://github.com/l4rzy/okular-mupdf-ng/releases).
Alternatively, you can [build it yourself](#building-and-testing). Installing this plugin will override the default backend for PDF and ePUB. You can select the backend of your choice every time you open a document by enabling "Show backend selection dialog" option in Okular.

---

## Motivation
Okular is an amazing piece of software: it's packed with good features; it's also extendable with a flexible plugin system. However, there are caveats that hold it back. This plugin solves the following issues with the current Okular's PDF and Epub backends, with the hope of making it more complete:
 - PDF is a complex format, it should not be parsed and rendered in Okular's memory space unconfined.
 - PDF backend with Poppler is rich in features, but sluggish on big PDFs. This is a known weakness of Poppler.
 - Rendering quality of the default Epub backend is terrible, plus rendering speed is also painstakingly slow even on a high-end CPU.

---

## Architecture

The document engine runs in an isolated worker process, where potentially unsafe
documents are parsed and rendered. See [ARCHITECTURE.md](ARCHITECTURE.md) for
component responsibilities, IPC, shared rendering buffers, sandboxing, caches,
and the source-tree layout.

---

## Feature Comparison

| Feature Category | Capabilities | okular-mupdf-ng | Okular official PDF & ePUB backends |
|---|---|:---:|:---:|
| **Safety & Isolation** | Sandboxed out-of-process worker (Landlock, Seccomp, namespaces, resource limits) | ✓ | ✗ (in-process execution) |
| **Formats** | PDF, ePUB | ✓ | ✓ |
| **ePUB customisation** | Custom CSS, Pagesizes | ✓ | ✗ |
| **PDF Forms** | AcroForm text inputs, checkboxes, radio buttons, and choices | ✓ | ✓ (plus basic XFA & Js support) |
| **Signatures** | Verification and creation (NSS crypto) | ✓ | ✓ (plus GPG) |
| **Cert Manager** | NSS Certificate Manager | ✓ | ✗ |
| **Annotations** | Text, highlight, line, shape, ink, stamp, caret | ✓ | ✓ |
| **Document Tools** | Text search, outline/TOC, links, fonts, metadata, embedded files | ✓ | ✓ |
| **Printing & Exporting** | Print & Export | ✓ | ✓ |
| **OCR** | Built-in Tesseract page OCR engine | ✓ | ✗ |


## Requirements to build

- A C++23 compiler (Clang preferred), CMake 3.20 or newer, Ninja, mold, `pkg-config`, and the usual
  build tools.
- Qt 6, KDE Frameworks 6, and Okular 6 development packages.
- NSS/NSPR for certificate and signature operations.
- Build dependencies required by MuPDF, including FreeType, HarfBuzz, JPEG,
  JBIG2, OpenJPEG, Brotli, Leptonica, and Zlib.
- `python3 >=3.12` when using the bundled MuPDF source for the first time (verified with sha256).

## Building and testing

The Makefile delegates to `scripts/build.sh`; both interfaces are equivalent.
The script uses Ninja and builds MuPDF with all available CPUs.

```bash
# Debug build and full test suite
make dev
# or: ./scripts/build.sh dev

# Optimized release build
make release
# or: ./scripts/build.sh release

# Configure, build, and run the debug test suite
make test

# Format src/ and tests/
make format

# Remove build directories
make clean
```

For an already configured build tree, run all tests with:

```bash
ctest --test-dir build --output-on-failure
```

## Arch Linux

The package recipe lives in `dist/` and downloads the pinned MuPDF source in
its `prepare()` step:

```bash
cd dist
makepkg -si
```

## Credits
- [Okular Poppler Backend](https://invent.kde.org/graphics/okular/-/tree/master/generators/poppler)
- [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf)
- [Zathura MuPDF Backend](https://github.com/pwmt/zathura-pdf-mupdf)

## License

Licensed under the [GNU General Public License v3.0 or later](COPYING)
(`GPL-3.0-or-later`).
