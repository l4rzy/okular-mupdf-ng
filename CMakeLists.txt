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

option(BUILD_OKULAR_GENERATOR "Build the Okular generator and KDE integration" ON)
option(ENABLE_SANITIZERS "Enable Address/LeakSanitizer instrumentation" OFF)
option(ENABLE_COVERAGE "Enable LLVM source-based coverage instrumentation" OFF)
option(ENABLE_LTO "Enable supported LTO in optimized CMake configurations" ON)
option(DISABLE_WORKER_SANDBOX "Disable worker sandboxing for diagnostic builds" OFF)
find_package(PkgConfig REQUIRED)
include(FeatureSummary)
if(BUILD_OKULAR_GENERATOR)
    find_package(ECM 6.3.0 CONFIG REQUIRED)
    list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
    include(KDEInstallDirs)
    if(ENABLE_SANITIZERS)
        set(ECM_ENABLE_SANITIZERS "address;leak" CACHE STRING
            "ECM sanitizer set for the release test build" FORCE)
    endif()
    include(ECMEnableSanitizers)
    include(KDECMakeSettings)
else()
    include(GNUInstallDirs)
    set(CMAKE_AUTOMOC ON)
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()
include(CTest)

if(ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE requires Clang for both C and C++")
    endif()
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    # Directory options instrument our CMake targets, without forwarding flags
    # into the external MuPDF/Rust builds. Counter relocation supports LLVM's
    # continuous profiling on Linux, retaining coverage from killed workers.
    # Same-named private helpers in files such as EPUB/XPS fonts.cpp must
    # have distinct profile records rather than colliding on the basename.
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic
        -funique-internal-linkage-names
        "SHELL:-mllvm -runtime-counter-relocation")
    add_link_options(-fprofile-instr-generate)
endif()

# Keep Debug/sanitizer builds and packaging's None configuration under their
# existing flag policy. IPO applies to CMake targets, not MuPDF's external Make build.
set(MU_LTO_ENABLED OFF)
if(ENABLE_LTO AND NOT ENABLE_SANITIZERS AND NOT ENABLE_COVERAGE AND
   (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$"))
    include(CheckIPOSupported)
    if(POLICY CMP0138)
        cmake_policy(SET CMP0138 NEW)
    endif()
    check_ipo_supported(RESULT MU_LTO_SUPPORTED OUTPUT MU_LTO_ERROR LANGUAGES C CXX)
    if(MU_LTO_SUPPORTED)
        foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config} ON)
        endforeach()
        set(MU_LTO_ENABLED ON)
    else()
        message(WARNING "LTO unavailable; continuing without it: ${MU_LTO_ERROR}")
    endif()
endif()

# Only the exact Debug configuration defaults to verbose diagnostics. None is used
# by Arch packaging so that makepkg owns the compiler flags; it must still
# produce a release-profile MuPDF build. The bundled MuPDF profile itself is
# controlled by MUPDF_OPTIMIZED_BUILD (default ON: release profile in every
# preset).
set(MU_IS_DEBUG_BUILD OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MU_IS_DEBUG_BUILD ON)
endif()

option(USE_SYSTEM_MUPDF "Use the system MuPDF package instead of bundled MuPDF" OFF)
option(MUPDF_OPTIMIZED_BUILD "Build bundled MuPDF with the release profile" ON)
option(ENABLE_FORM_JAVASCRIPT "Enable PDF form JavaScript support" ON)
set(MUPDF_CJK_FALLBACK "droidfull" CACHE STRING "Bundled MuPDF CJK fallback")
set_property(CACHE MUPDF_CJK_FALLBACK PROPERTY STRINGS sourcehan droidfull droid none)
set(TESSDATA_DIR "/usr/share/tessdata" CACHE PATH "Path to tesseract tessdata directory")
set(SIGNATURE_FONT_DIR "${CMAKE_INSTALL_FULL_DATADIR}/okular-mupdf-ng" CACHE PATH
    "Directory containing the bundled signature font")
set(PAPERBOX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/paperbox")
if(NOT EXISTS "${PAPERBOX_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Paperbox is missing. Run git submodule update --init thirdparty/paperbox.")
endif()
file(STRINGS "${PAPERBOX_SOURCE_DIR}/cmake/mupdf.version" MUPDF_REQUIRED_VERSION LIMIT_COUNT 1)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)
if(BUILD_OKULAR_GENERATOR)
    find_package(Okular6 REQUIRED)
    find_package(KF6 REQUIRED COMPONENTS CoreAddons I18n Config ConfigWidgets KIO XmlGui WidgetsAddons)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent PrintSupport Network Xml)
endif()

option(ENABLE_BUILD_TREE_WORKER
    "Embed the build-tree worker for uninstalled developer runs"
    ${MU_IS_DEBUG_BUILD})

option(ENABLE_MU_DEBUG "Compile verbose Mu diagnostics" ${MU_IS_DEBUG_BUILD})
set(MU_DEBUG_BUILD OFF)
if(ENABLE_MU_DEBUG)
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
set(MU_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(MU_BUILD_INFO_FILE "${MU_GENERATED_INCLUDE_DIR}/build_info.hpp")
include(cmake/build_info.cmake)
# Check on every build: commits, tags, and local edits can change without a
# CMake reconfiguration. configure_file leaves an unchanged header untouched.
add_custom_target(ng_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DMU_SOURCE_DIR=${MU_SOURCE_DIR}"
        "-DMU_BUILD_INFO_FILE=${MU_BUILD_INFO_FILE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake"
    BYPRODUCTS "${MU_BUILD_INFO_FILE}"
    VERBATIM)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/cli/version.hpp.in"
    "${MU_GENERATED_INCLUDE_DIR}/tools/cli/version.hpp"
    @ONLY)

set(MUPDF_WORKER_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/okular-mupdf-ng")
set(default_worker_path "${CMAKE_INSTALL_FULL_LIBDIR}/okular-mupdf-ng/paperbox-worker")
if(WORKER_INSTALL_PATH STREQUAL "${CMAKE_INSTALL_FULL_LIBDIR}/okular-mupdf-ng/mupdfng-worker")
    # Refresh the previous default in existing build trees.
    unset(WORKER_INSTALL_PATH CACHE)
endif()
set(WORKER_INSTALL_PATH "${default_worker_path}" CACHE PATH "Installed path of the paperbox-worker binary")
# Keep Paperbox's archives private to the plugin and CLI. Install just the
# worker executable from the subproject, along with our existing assets.
function(add_paperbox)
    set(BUILD_SHARED_LIBS OFF)
    set(PAPERBOX_BUILD_HOST ON)
    set(PAPERBOX_BUILD_WORKER ON)
    set(PAPERBOX_BUILD_NSS ON)
    set(PAPERBOX_BUILD_TESTING OFF)
    set(PAPERBOX_BUILD_QT_TESTS OFF)
    set(PAPERBOX_DEBUG_BUILD "${MU_DEBUG_BUILD}")
    set(PAPERBOX_DISABLE_WORKER_SANDBOX "${DISABLE_WORKER_SANDBOX}")
    set(PAPERBOX_USE_SYSTEM_MUPDF "${USE_SYSTEM_MUPDF}")
    set(PAPERBOX_MUPDF_OPTIMIZED_BUILD "${MUPDF_OPTIMIZED_BUILD}")
    set(PAPERBOX_MUPDF_CJK_FALLBACK "${MUPDF_CJK_FALLBACK}" CACHE STRING "Paperbox MuPDF CJK fallback" FORCE)
    if(NOT PAPERBOX_MUPDF_SOURCE_DIR AND MUPDF_SOURCE_DIR AND EXISTS "${MUPDF_SOURCE_DIR}/Makefile")
        set(PAPERBOX_MUPDF_SOURCE_DIR "${MUPDF_SOURCE_DIR}" CACHE PATH "Path to patched MuPDF sources" FORCE)
    endif()
    add_subdirectory("${PAPERBOX_SOURCE_DIR}" paperbox EXCLUDE_FROM_ALL)
endfunction()
add_paperbox()
# Apply the NSS read-size fix to a build-tree copy until the pinned Paperbox
# revision includes it.
find_program(PAPERBOX_PATCH_EXECUTABLE patch REQUIRED)
set(paperbox_nss_dir "${PAPERBOX_SOURCE_DIR}/src/internal/nss")
set(paperbox_nss_verification "${paperbox_nss_dir}/verification.cpp")
set(paperbox_nss_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/paperbox-nss-read-size.patch")
set(paperbox_nss_patched "${CMAKE_CURRENT_BINARY_DIR}/paperbox/verification.cpp")
add_custom_command(OUTPUT "${paperbox_nss_patched}"
    COMMAND "${PAPERBOX_PATCH_EXECUTABLE}" --force --fuzz=0 --silent
        --output "${paperbox_nss_patched}" "${paperbox_nss_verification}" "${paperbox_nss_patch}"
    DEPENDS "${paperbox_nss_verification}" "${paperbox_nss_patch}"
    VERBATIM)
add_custom_target(paperbox_nss_read_size_fix DEPENDS "${paperbox_nss_patched}")
add_dependencies(paperbox_nss paperbox_nss_read_size_fix)
set_source_files_properties("${paperbox_nss_verification}"
    TARGET_DIRECTORY paperbox_nss PROPERTIES HEADER_FILE_ONLY ON)
target_sources(paperbox_nss PRIVATE "${paperbox_nss_patched}")
target_include_directories(paperbox_nss PRIVATE "${paperbox_nss_dir}")

set_target_properties(paperbox-worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
install(TARGETS paperbox-worker RUNTIME DESTINATION "${MUPDF_WORKER_INSTALL_DIR}")
add_subdirectory(src)

if(BUILD_OKULAR_GENERATOR)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist/mupdfng.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng"
    RENAME copyright)
install(FILES
    "${PAPERBOX_SOURCE_DIR}/thirdparty/allura/Allura-Regular.ttf"
    DESTINATION "${SIGNATURE_FONT_DIR}")
install(FILES
    "${PAPERBOX_SOURCE_DIR}/thirdparty/allura/OFL.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng"
    RENAME Allura-OFL.txt)
install(FILES "${PAPERBOX_SOURCE_DIR}/thirdparty/xxhash/xxHash-LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng")
# MuPDF is AGPL-3.0-or-later; ship its license text alongside the plugin. Only
# MuPDF's own license is redistributed here, not its bundled sub-dependencies.
install(FILES
    "${PAPERBOX_SOURCE_DIR}/thirdparty/mupdf-COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/okular-mupdf-ng"
    RENAME MuPDF-COPYING)

message(STATUS "================================================================")
message(STATUS "Manifest version: ${MU_COMPAT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "LTO for optimized configurations: ${MU_LTO_ENABLED}")
message(STATUS "LLVM coverage instrumentation: ${ENABLE_COVERAGE}")
message(STATUS "System MuPDF in Paperbox worker: ${USE_SYSTEM_MUPDF}")
message(STATUS "MuPDF CJK fallback: ${MUPDF_CJK_FALLBACK}")
message(STATUS "PDF form JavaScript: ${ENABLE_FORM_JAVASCRIPT}")
message(STATUS "Mu debug diagnostics: ${MU_DEBUG_BUILD}")
message(STATUS "Worker install path: ${WORKER_INSTALL_PATH}")
message(STATUS "Build-tree worker: ${ENABLE_BUILD_TREE_WORKER}")
message(STATUS "Tessdata directory: ${TESSDATA_DIR}")
message(STATUS "Signature font directory: ${SIGNATURE_FONT_DIR}")
message(STATUS "================================================================")

# Both build profiles expose the standard BUILD_TESTING option.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(BUILD_OKULAR_GENERATOR AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/po")
    ki18n_install(po)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CPACK_PACKAGE_NAME "okular-mupdf-ng")
set(CPACK_PACKAGE_VERSION "${MU_COMPAT_VERSION}")
set(CPACK_PACKAGE_CONTACT "l4rzy <me@23ro.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Secure and fast MuPDF-based backend for Okular")
set(MU_PACKAGE_DESCRIPTION
    "Supports PDF, ePUB, legacy MOBI, and DRM-free AZW3 and single-container KFX.\nDocument parsing, rendering, and OCR run in a separate sandboxed worker.\nIncludes the mupdfng-cli command-line utility.")
if(BUILD_OKULAR_GENERATOR)
    set(MU_APPLICATION_PACKAGE_DEPENDS "okular")
else()
    set(MU_APPLICATION_PACKAGE_DEPENDS "")
endif()
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/l4rzy/okular-mupdf-ng")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_DEBIAN_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
string(REPLACE "\n" "\n " MU_DEBIAN_PACKAGE_DESCRIPTION "${MU_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}\n ${MU_DEBIAN_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${MU_APPLICATION_PACKAGE_DEPENDS}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_RPM_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Productivity/Office/Other")
set(CPACK_RPM_PACKAGE_VENDOR "l4rzy")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${MU_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_REQUIRES
    "${MU_APPLICATION_PACKAGE_DEPENDS}"
    CACHE STRING "Explicit runtime dependencies for RPM packages")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
include(CPack)
