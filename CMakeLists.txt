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
include(ECMEnableSanitizers)
include(KDECMakeSettings)

option(ENABLE_SANITIZERS "Enable Address/Undefined/LeakSanitizer instrumentation" OFF)
if(ENABLE_SANITIZERS)
    set(ECM_ENABLE_SANITIZERS "address;leak;undefined" CACHE STRING
        "ECM sanitizer set for the release test build" FORCE)
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
find_package(Qt6 REQUIRED COMPONENTS Concurrent PrintSupport Network)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(DEFAULT_IPC_DEBUG OFF)
else()
    set(DEFAULT_IPC_DEBUG ON)
endif()

option(ENABLE_MU_DEBUG "Compile verbose Mu diagnostics" ${DEFAULT_IPC_DEBUG})
set(MU_DEBUG_BUILD OFF)
if(ENABLE_MU_DEBUG AND NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(MU_DEBUG_BUILD ON)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/generator/libokularGenerator_mupdfng.json" MU_GENERATOR_MANIFEST)
string(JSON MU_COMPAT_VERSION ERROR_VARIABLE MU_MANIFEST_ERROR
    GET "${MU_GENERATOR_MANIFEST}" KPlugin Version)
if(MU_MANIFEST_ERROR)
    message(FATAL_ERROR "Cannot read KPlugin.Version from the generator manifest: ${MU_MANIFEST_ERROR}")
endif()
if(NOT MU_COMPAT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "KPlugin.Version must use MAJOR.MINOR.PATCH: ${MU_COMPAT_VERSION}")
endif()
set(MUPDF_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MUPDF_GENERATED_INCLUDE_DIR}/shared/transport")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/compat.hpp.in"
    "${MUPDF_GENERATED_INCLUDE_DIR}/shared/transport/compat.hpp"
    @ONLY)

set(WORKER_INSTALL_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/okular-mupdf-ng/okular-mupdf-worker"
    CACHE PATH "Installed path of the okular-mupdf-worker binary")
add_subdirectory(src)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist/mupdfng.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng"
    RENAME copyright)

# Embed worker binary paths so the plugin can locate it without install
target_compile_definitions(okularGenerator_mupdfng PRIVATE
    "$<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>>:WORKER_BUILD_PATH=\"$<TARGET_FILE:okular-mupdf-worker>\">"
    WORKER_INSTALL_PATH="${WORKER_INSTALL_PATH}"
)

message(STATUS "Manifest version: ${MU_COMPAT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Worker install path: ${WORKER_INSTALL_PATH}")
message(STATUS "Tessdata directory: ${TESSDATA_DIR}")
message(STATUS "OCR support: ${MUPDF_HAS_OCR}")
message(STATUS "Seccomp support: ${SECCOMP_FOUND}")
if(BUILD_TESTING)
    include(CTest)
    add_subdirectory(tests)
endif()

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/po")
    ki18n_install(po)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

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
set(CPACK_DEBIAN_PACKAGE_DEPENDS "okular, tesseract-ocr")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_RPM_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Productivity/Office/Other")
set(CPACK_RPM_PACKAGE_VENDOR "l4rzy")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_RPM_PACKAGE_REQUIRES "okular, tesseract-ocr, tesseract-ocr-traineddata-eng")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
include(CPack)
