# Installation

## Prebuilt packages

Prebuilt packages for supported distros are attached to each
[GitHub release](https://github.com/l4rzy/okular-mupdf-ng/releases). The commands
below install v0.3.3.

Installing this plugin will override the default backend for PDF and ePUB. You
can select the backend of your choice every time you open a document by enabling
the "Show backend selection dialog" option in Okular.

### Arch Linux

```bash
yay -U https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng-0.3.3-1-x86_64.pkg.tar.zst
```

### Fedora

```bash
# Fedora 44 x86_64
sudo dnf install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng-0.3.3-1.fc44.x86_64.rpm

# Fedora 43 aarch64/Asahi Linux
sudo dnf install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng-0.3.3-1.fc43.aarch64.rpm
```

### openSUSE Tumbleweed

```bash
sudo zypper install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng-0.3.3-1.tumbleweed.x86_64.rpm
```

### Debian 13

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng_0.3.3-1_amd64_debian-13.deb
sudo apt install ./okular-mupdf-ng_0.3.3-1_amd64_debian-13.deb
```

### Ubuntu 26.04

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.3.3/okular-mupdf-ng_0.3.3-1_amd64_ubuntu-26.04.deb
sudo apt install ./okular-mupdf-ng_0.3.3-1_amd64_ubuntu-26.04.deb
```

### Any other distro

Build from source — see [Building and testing](#building-and-testing).

## Requirements to build

- A C++23 compiler (Clang preferred), CMake 3.20 or newer, Ninja, mold, `pkg-config`, and the usual
  build tools.
- Qt 6 development packages. The default Okular plugin build also requires
  KDE Frameworks 6 and Okular 6 development packages.
- NSS/NSPR for certificate and signature operations.
- Build dependencies required by MuPDF, including FreeType, HarfBuzz, JPEG,
  JBIG2, OpenJPEG, Brotli, Leptonica, and Zlib.
- `python3 >=3.12` when using the bundled MuPDF source for the first time (verified with sha256).
- `patch` for applying the bundled MuPDF fixes during CMake configuration.

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
