# SPDX-License-Identifier: GPL-3.0-or-later

# Keep these values in sync with cmake/mupdf.version.
%global mupdf_version 1.28.5
%global mupdf_sha256 98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
%global _vpath_builddir build
%global tessdata_dir %{_datadir}/tesseract/tessdata

%bcond_without tests

Name:           okular-mupdf-ng
Version:        0.5.4
Release:        1%{?dist}
Summary:        A MuPDF-based secure and fast backend for Okular
License:        AGPL-3.0-or-later AND GPL-3.0-or-later AND OFL-1.1
URL:            https://github.com/l4rzy/okular-mupdf-ng
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz
Source1:        https://github.com/ArtifexSoftware/mupdf-downloads/releases/download/%{mupdf_version}/mupdf-%{mupdf_version}-source.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  cmake-rpm-macros
BuildRequires:  extra-cmake-modules >= 6.3.0
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  make
BuildRequires:  ninja-build
BuildRequires:  patch
BuildRequires:  pkgconf-pkg-config
BuildRequires:  cmake(KF6Config)
BuildRequires:  cmake(KF6ConfigWidgets)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6KIO)
BuildRequires:  cmake(KF6WidgetsAddons)
BuildRequires:  cmake(KF6XmlGui)
BuildRequires:  cmake(Okular6)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6PrintSupport)
BuildRequires:  cmake(Qt6Xml)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(libbrotlidec)
BuildRequires:  pkgconfig(libbrotlienc)
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(nss)
BuildRequires:  pkgconfig(zlib)
%if %{with tests}
BuildRequires:  cmake(Qt6Test)
BuildRequires:  dejavu-fonts-all
BuildRequires:  nss-tools
BuildRequires:  openssl
BuildRequires:  tesseract-langpack-eng
%endif
Requires:       okular
Recommends:     tesseract-langpack-eng

%description
A backend for Okular that supports PDF, EPUB, and MOBI documents using MuPDF.
Document parsing, rendering, and OCR run in a separate sandboxed worker.
The package includes the command-line utility mupdfng-cli and a statically
linked, pinned MuPDF engine with the project's compatibility fixes.

%prep
printf '%s  %s\n' '%{mupdf_sha256}' '%{SOURCE1}' | sha256sum --check --status
%setup -q -a 1
test "$(sed -n '1p' cmake/mupdf.version)" = '%{mupdf_version}'
test "$(sed -n '2p' cmake/mupdf.version)" = '%{mupdf_sha256}'
sed -i 's/\r$//' thirdparty/allura/OFL.txt

%build
%cmake -G Ninja \
    -DKDE_INSTALL_USE_QT_SYS_PATHS:BOOL=ON \
    -DUSE_SYSTEM_MUPDF:BOOL=OFF \
    -DMUPDF_SOURCE_DIR:PATH="%{_builddir}/%{name}-%{version}/mupdf-%{mupdf_version}-source" \
    -DBUILD_TESTING:BOOL=%{with tests} \
    -DENABLE_BUILD_TREE_WORKER:BOOL=OFF \
    -DENABLE_MU_DEBUG:BOOL=OFF \
    -DDISABLE_WORKER_SANDBOX:BOOL=OFF \
    -DTESSDATA_DIR:PATH=%{tessdata_dir}
%cmake_build

%install
%cmake_install
# RPM installs the canonical license copies separately.
rm -r "%{buildroot}%{_datadir}/doc/%{name}"
%find_lang okular_mupdfng

%check
%if %{with tests}
ctest --test-dir %{_vpath_builddir} --output-on-failure --parallel %{_smp_build_ncpus}
%endif

%files -f okular_mupdfng.lang
%license COPYING thirdparty/mupdf-COPYING thirdparty/allura/OFL.txt
%doc README.md INSTALL.md MATURITY.md
%{_bindir}/mupdfng-cli
%dir %{_libdir}/okular-mupdf-ng
%{_libdir}/okular-mupdf-ng/mupdfng-worker
%{_libdir}/qt6/plugins/okular_generators/okularGenerator_mupdfng.so
%{_datadir}/config.kcfg/mupdfngsettings.kcfg
%{_datadir}/icons/hicolor/scalable/apps/mupdfng.svg
%dir %{_datadir}/okular-mupdf-ng
%{_datadir}/okular-mupdf-ng/Allura-Regular.ttf

%changelog
* Sun Oct 25 2026 l4rzy <me@23ro.org> - 0.5.4-1
- Update to version 0.5.4.
- Add XFDF/XML annotation import from the Edit menu with undo/redo support.
- Refine direct runtime dependencies.

* Sat Oct 24 2026 l4rzy <me@23ro.org> - 0.5.3-1
- Update to version 0.5.3.
- Fix tile boundary padding and fit oversized tiled render requests.
- Avoid distorted tiles after zoom changes when render cancellation is disabled.

* Fri Oct 23 2026 l4rzy <me@23ro.org> - 0.5.2-1
- Update to version 0.5.2.
- Add configurable link previews with scrolling, dragging, and flick gestures.
- Add theme and link-preview status notifications.

* Thu Oct 22 2026 l4rzy <me@23ro.org> - 0.5.0-1
- Update to version 0.5.0.
- Improve OCR scheduling, render cancellation tests, and export lifecycle handling.

* Wed Oct 21 2026 l4rzy <me@23ro.org> - 0.4.9-1
- Update to version 0.4.9.
- Refactor for readability.
- Use a pixmap for the dark theme instead of accessibility inversion, improving selection in dark mode.

* Tue Oct 20 2026 l4rzy <me@23ro.org> - 0.4.8-1
- Support icon and custom image stamps embedded in PDFs; add native stamp colors.
- Share one menu controller per window and preserve menu order across tab switches.
- Display annotation operation errors in the GUI

* Mon Oct 19 2026 l4rzy <me@23ro.org> - 0.4.6-1
- Add Fedora COPR packaging.
