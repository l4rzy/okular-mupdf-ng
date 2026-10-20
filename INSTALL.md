# Installation

## Prebuilt packages

Prebuilt packages for supported distros are attached to each
[GitHub release](https://github.com/l4rzy/okular-mupdf-ng/releases). Repository
installations receive updates through your package manager. The commands
below install v0.4.8.

Installing this plugin will override the default backend for PDF and ePUB. You
can select the backend of your choice every time you open a document by enabling
the "Show backend selection dialog" option in Okular.

### Arch Linux

```bash
yay -U https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.8/okular-mupdf-ng-0.4.8-1-x86_64.archlinux.pkg.tar.zst
```

Arch Linux updates to Okular, KDE Frameworks 6 (KF6), Qt, or other dependencies
may change the ABI and make prebuilt packages incompatible with your system.
If a prebuilt package does not work after an update, build the package locally
using [dist/PKGBUILD](dist/PKGBUILD) so it matches your installed libraries.

### Manjaro Linux

```bash
sudo pacman -U https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.8/okular-mupdf-ng-0.4.8-1-x86_64.manjaro.pkg.tar.zst
```

### Fedora

Enable the [COPR repository](https://copr.fedorainfracloud.org/coprs/l4rzy/okular-mupdf-ng/)
and install the package:

```bash
sudo dnf copr enable l4rzy/okular-mupdf-ng
sudo dnf install okular-mupdf-ng
```

### openSUSE Tumbleweed

Add the [OBS repository](https://build.opensuse.org/package/show/home:23ro/okular-mupdf-ng)
and install the package:

```bash
sudo zypper addrepo --refresh https://download.opensuse.org/repositories/home:/23ro/openSUSE_Tumbleweed/home:23ro.repo
sudo zypper refresh
sudo zypper install okular-mupdf-ng
```

### Debian 13

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.8/okular-mupdf-ng_0.4.8-1_amd64_debian-13.deb
sudo apt install ./okular-mupdf-ng_0.4.8-1_amd64_debian-13.deb
```

### Ubuntu 26.04

```bash
curl -LO https://github.com/l4rzy/okular-mupdf-ng/releases/download/v0.4.8/okular-mupdf-ng_0.4.8-1_amd64_ubuntu-26.04.deb
sudo apt install ./okular-mupdf-ng_0.4.8-1_amd64_ubuntu-26.04.deb
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
- System libraries required by bundled MuPDF: FreeType, JPEG, Brotli, and Zlib.
  JBIG2, OpenJPEG, HarfBuzz, Gumbo, LCMS2, Tesseract, and Leptonica are bundled.
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

Build the debug preset, then launch Okular with the plugin and worker from
`build/`:

```bash
make dev
./scripts/run-okular.sh /path/to/document.pdf
# Okular options are forwarded unchanged:
./scripts/run-okular.sh --page 9 /path/to/document.pdf
```

The launcher sets `QT_PLUGIN_PATH` for this process and starts a private D-Bus
session so documents are not forwarded to an already-running Okular. It requires
`okular` and `dbus-run-session` on `PATH`; no installation, root privileges, or
symlinks are needed. Enable Okular's backend selection dialog if you need to
choose the MuPDF backend. Restart the launched Okular after rebuilding.

The launcher uses `build/`, not `build-release/`, and checks that
`ENABLE_BUILD_TREE_WORKER=ON` so the matching worker is used. This is enabled by
the debug preset on a fresh build. For an existing build with it disabled:

```bash
cmake -B build -DENABLE_BUILD_TREE_WORKER=ON
cmake --build build
```

#### Alternative: symlink the built plugin

After `make release`, run these commands from the repository root to link the
built plugin into Qt 6's Okular generator directory:

```bash
okular_generator_dir="$(qmake6 -query QT_INSTALL_PLUGINS)/okular_generators"
sudo ln -s "$(pwd)/build-release/bin/okular_generators/okularGenerator_mupdfng.so" \
  "$okular_generator_dir/okularGenerator_mupdfng.so"
```

For `make dev`, replace `build-release` with `build`. Both presets enable
build-tree worker lookup on a fresh build, so keep the build directory available.
Close all Okular instances and reopen Okular after linking or rebuilding the
plugin. Enable Okular's backend selection dialog to choose the MuPDF backend.

The command refuses to overwrite an existing plugin. If a packaged version is
installed, uninstall it before creating the link. To remove the test symlink:

```bash
sudo unlink "$okular_generator_dir/okularGenerator_mupdfng.so"
```
