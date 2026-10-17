# Installation

## Prebuilt packages

Prebuilt packages for supported distros are attached to each
[GitHub release](https://github.com/l4rzy/okular-mupdf-ng/releases). The commands
below install v0.4.5.

Installing this plugin will override the default backend for PDF and ePUB. You
can select the backend of your choice every time you open a document by enabling
the "Show backend selection dialog" option in Okular.

### Arch Linux

```bash
yay -U https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng-0.4.5-1-x86_64.archlinux.pkg.tar.zst
```

Arch Linux updates to Okular, KDE Frameworks 6 (KF6), Qt, or other dependencies
may change the ABI and make prebuilt packages incompatible with your system.
If a prebuilt package does not work after an update, build the package locally
using [dist/PKGBUILD](dist/PKGBUILD) so it matches your installed libraries.

### Manjaro Linux

```bash
sudo pacman -U https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng-0.4.5-1-x86_64.manjaro.pkg.tar.zst
```

### Fedora

```bash
# Fedora 44 x86_64
sudo dnf install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng-0.4.5-1.fc44.x86_64.rpm

# Fedora 43 aarch64/Asahi Linux
sudo dnf install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng-0.4.5-1.fc43.aarch64.rpm
```

### openSUSE Tumbleweed

```bash
sudo zypper install https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng-0.4.5-1.tumbleweed.x86_64.rpm
```

### Debian 13

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng_0.4.5-1_amd64_debian-13.deb
sudo apt install ./okular-mupdf-ng_0.4.5-1_amd64_debian-13.deb
```

### Ubuntu 26.04

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.5/okular-mupdf-ng_0.4.5-1_amd64_ubuntu-26.04.deb
sudo apt install ./okular-mupdf-ng_0.4.5-1_amd64_ubuntu-26.04.deb
```

### Any other distro

Build from source — see [Building and testing](#building-and-testing).

## Requirements to build

- A C++23 compiler (Clang preferred), CMake 3.20 or newer, Ninja, mold, `pkg-config`, and the usual
  build tools.
- Qt 6 development packages. The default Okular plugin build also requires
  KDE Frameworks 6 and Okular 6 development packages.
- For Clang LTO: LLVM tools (`llvm-ar`, `llvm-ranlib`) and the matching LLVMgold
  plugin for mold/GNU ld. Install `llvm`; on openSUSE also install `llvm-gold`.
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

# Optimized release build with LTO, verbose diagnostics, and the build-tree worker
make release
# or: ./scripts/build.sh release

# Configure, build, and run the debug test suite
make test

# Format src/ and tests/
make format

# Remove build directories
make clean
```

LTO is enabled by default for CMake targets in Release, RelWithDebInfo, and
MinSizeRel builds when the compiler and linker support it. Disable it with
`-DENABLE_LTO=OFF`. Debug and sanitizer builds are excluded; Arch's `None`
configuration continues to use makepkg's flags. The separately built bundled
MuPDF archives keep their existing compiler flags.

For an already configured build tree, run all tests with:

```bash
ctest --test-dir build --output-on-failure
```

### Test the built plugin in Okular

After `make release`, run these commands from the repository root to link the
built plugin into Qt 6's Okular generator directory:

```bash
okular_generator_dir="$(qmake6 -query QT_INSTALL_PLUGINS)/okular_generators"
sudo ln -s "$(pwd)/build-release/bin/okular_generators/okularGenerator_mupdfng.so" \
  "$okular_generator_dir/okularGenerator_mupdfng.so"
```

For `make dev`, replace `build-release` with `build`. Both presets use the worker
from their build tree, so keep that directory available. Close all Okular
instances and reopen Okular after linking or rebuilding the plugin. Enable
Okular's backend selection dialog to choose the MuPDF backend.

The command refuses to overwrite an existing plugin. If a packaged version is
installed, uninstall it before creating the link. To remove the test symlink:

```bash
sudo unlink "$okular_generator_dir/okularGenerator_mupdfng.so"
```
