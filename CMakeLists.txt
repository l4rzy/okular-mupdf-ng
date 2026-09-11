#/***************************************************************************
# *   Copyright (C) 2026 by l4rzy <me@23ro.org>                            *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU General Public License as published by  *
# *   the Free Software Foundation; either version 3 of the License, or     *
# *   (at your option) any later version.                                   *
# ***************************************************************************/

cmake_minimum_required(VERSION 3.20.0)
project(okular-mupdf-ng)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(ECM 6.3.0 CONFIG REQUIRED)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})
include(FeatureSummary)
include(KDEInstallDirs)

# ECM_ENABLE_SANITIZERS must be populated before ECMEnableSanitizers runs: the
# module appends the -fsanitize flags to CMAKE_CXX_FLAGS at include time.
option(ENABLE_SANITIZERS "Enable Address/Undefined/LeakSanitizer instrumentation" OFF)
if(ENABLE_SANITIZERS)
    set(ECM_ENABLE_SANITIZERS "address;leak;undefined" CACHE STRING
        "ECM sanitizer set for the release test build" FORCE)
endif()

include(ECMEnableSanitizers)
include(KDECMakeSettings)

# Only the exact Debug configuration enables verbose diagnostics. None is used
# by Arch packaging so that makepkg owns the compiler flags; it must still
# produce a release-profile MuPDF build. The bundled MuPDF profile itself is
# controlled by MUPDF_OPTIMIZED_BUILD (default ON: release profile in every
# preset).
set(MU_IS_DEBUG_BUILD OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MU_IS_DEBUG_BUILD ON)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mupdf.cmake")

set(TESSDATA_DIR "/usr/share/tessdata" CACHE PATH "Path to tesseract tessdata directory")

find_package(Okular6 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(NSS REQUIRED nss)
pkg_check_modules(SECCOMP QUIET libseccomp)
find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    I18n
    Config
    ConfigWidgets
    KIO
    )
find_package(Qt6 REQUIRED COMPONENTS Concurrent PrintSupport Network Xml)

option(ENABLE_BUILD_TREE_WORKER
    "Embed the build-tree worker for uninstalled developer runs"
    ${MU_IS_DEBUG_BUILD})

option(ENABLE_MU_DEBUG "Compile verbose Mu diagnostics (Debug builds only)" ${MU_IS_DEBUG_BUILD})
set(MU_DEBUG_BUILD OFF)
if(MU_IS_DEBUG_BUILD AND ENABLE_MU_DEBUG)
    set(MU_DEBUG_BUILD ON)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/generator/manifest.json" MU_GENERATOR_MANIFEST)
string(JSON MU_COMPAT_VERSION ERROR_VARIABLE MU_MANIFEST_ERROR
    GET "${MU_GENERATOR_MANIFEST}" KPlugin Version)
if(MU_MANIFEST_ERROR)
    message(FATAL_ERROR "Cannot read KPlugin.Version from the generator manifest: ${MU_MANIFEST_ERROR}")
endif()
if(NOT MU_COMPAT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "KPlugin.Version must use MAJOR.MINOR.PATCH: ${MU_COMPAT_VERSION}")
endif()
set(MU_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/compat.hpp.in"
    "${MU_GENERATED_INCLUDE_DIR}/shared/compat.hpp"
    @ONLY)

set(MUPDF_WORKER_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/okular-mupdf-ng")
set(WORKER_INSTALL_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/okular-mupdf-ng/okular-mupdf-worker"
    CACHE PATH "Installed path of the okular-mupdf-worker binary")
add_subdirectory(src)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist/mupdfng.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng"
    RENAME copyright)

message(STATUS "Manifest version: ${MU_COMPAT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "MuPDF build profile: ${MUPDF_BUILD_PROFILE}")
message(STATUS "Mu debug diagnostics: ${MU_DEBUG_BUILD}")
message(STATUS "Worker install path: ${WORKER_INSTALL_PATH}")
message(STATUS "Build-tree worker: ${ENABLE_BUILD_TREE_WORKER}")
message(STATUS "Tessdata directory: ${TESSDATA_DIR}")
message(STATUS "Seccomp support: ${SECCOMP_FOUND}")
# BUILD_TESTING is defined by ECM's KDECMakeSettings (default ON).
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/po")
    ki18n_install(po)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

# Optional color-emoji font used to render emoji glyphs in the sandbox
# enforcement status view. It is an optional dependency (RPM Recommends /
# Debian Suggests) because Okular renders fine without it, and package names
# differ per distribution, so they are resolved from /etc/os-release to keep a
# single CPack configuration serving every packaging workflow. Overridable via
# the cache for exotic targets; empty disables the entry.
set(MU_EMOJI_FONT_PACKAGE "" CACHE STRING "Optional color-emoji font package name")
if(NOT MU_EMOJI_FONT_PACKAGE AND EXISTS "/etc/os-release")
    file(READ "/etc/os-release" _mu_os_release)
    if(_mu_os_release MATCHES "ID_LIKE=.*opensuse|ID=opensuse")
        set(MU_EMOJI_FONT_PACKAGE "google-noto-coloremoji-fonts")
    elseif(_mu_os_release MATCHES "ID_LIKE=.*fedora|ID=fedora")
        set(MU_EMOJI_FONT_PACKAGE "google-noto-color-emoji-fonts")
    elseif(_mu_os_release MATCHES "ID_LIKE=.*debian|ID=debian|ID=ubuntu")
        set(MU_EMOJI_FONT_PACKAGE "fonts-noto-color-emoji")
    endif()
    unset(_mu_os_release)
endif()

set(CPACK_PACKAGE_NAME "okular-mupdf-ng")
set(CPACK_PACKAGE_VERSION "${MU_COMPAT_VERSION}")
set(CPACK_PACKAGE_CONTACT "l4rzy <me@23ro.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MuPDF backend generator plugin for KDE Okular")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/l4rzy/okular-mupdf-ng")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_DEBIAN_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "okular")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "${MU_EMOJI_FONT_PACKAGE}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_RPM_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Productivity/Office/Other")
set(CPACK_RPM_PACKAGE_VENDOR "l4rzy")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_RPM_PACKAGE_REQUIRES
    "okular"
    CACHE STRING "Explicit runtime dependencies for RPM packages")
set(CPACK_RPM_PACKAGE_SUGGESTS "${MU_EMOJI_FONT_PACKAGE}")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
include(CPack)
