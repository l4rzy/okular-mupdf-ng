Format: 3.0 (quilt)
Source: okular-mupdf-ng
Binary: okular-mupdf-ng
Architecture: any
Version: 0.5.9-1
Maintainer: l4rzy <me@23ro.org>
Homepage: https://github.com/l4rzy/okular-mupdf-ng
Standards-Version: 4.7.0
Build-Depends: debhelper-compat (= 13),
 gcc,
 g++,
 cargo-web (>= 1.91) | cargo (>= 1.91),
 rustc-web (>= 1.91) | rustc (>= 1.91),
 cmake (>= 3.20),
 extra-cmake-modules (>= 6.3.0),
 gettext,
 ninja-build,
 patch,
 pkg-config,
 python3 (>= 3.12),
 libavcodec-extra,
 libavformat-extra,
 libbrotli-dev,
 libfreetype-dev,
 libjpeg-dev,
 libkf6config-dev,
 libkf6configwidgets-dev,
 libkf6coreaddons-dev,
 libkf6i18n-dev,
 libkf6kio-dev,
 libkf6widgetsaddons-dev,
 libkf6xmlgui-dev,
 libnss3-dev,
 libseccomp-dev,
 libselinux1-dev,
 okular-dev,
 qt6-base-dev,
 zlib1g-dev,
 fonts-dejavu-core,
 libnss3-tools,
 openssl,
 tesseract-ocr-eng
Debtransform-Tar: okular-mupdf-ng-@VERSION@.tar.gz
Debtransform-Files-Tar:
Debtransform-Files: mupdf-1.28.5-source.tar.gz vendor.tar.gz
Debtransform-Release: 1
