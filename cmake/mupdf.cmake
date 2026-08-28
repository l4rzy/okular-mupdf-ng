# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mupdf.version" _mupdf_version_lines LIMIT_COUNT 2)
list(GET _mupdf_version_lines 0 _mupdf_file_version)
string(STRIP "${_mupdf_file_version}" _mupdf_file_version)
if(NOT _mupdf_file_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid version in cmake/mupdf.version: ${_mupdf_file_version}")
endif()
set(MUPDF_REQUIRED_VERSION "${_mupdf_file_version}" CACHE STRING "Required MuPDF version (from cmake/mupdf.version)")

option(USE_SYSTEM_MUPDF "Use the system MuPDF package instead of bundled MuPDF" OFF)

if(USE_SYSTEM_MUPDF)
    pkg_check_modules(MUPDF_SYSTEM REQUIRED IMPORTED_TARGET mupdf>=${MUPDF_REQUIRED_VERSION})

    add_library(MuPDF::Engine INTERFACE IMPORTED GLOBAL)
    set_target_properties(MuPDF::Engine PROPERTIES
        INTERFACE_LINK_LIBRARIES PkgConfig::MUPDF_SYSTEM)
else()
    set(MUPDF_SOURCE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/mupdf" CACHE PATH
        "Path to the pinned MuPDF source tree")
    if(NOT EXISTS "${MUPDF_SOURCE_DIR}/Makefile"
       AND MUPDF_SOURCE_DIR STREQUAL "${CMAKE_SOURCE_DIR}/thirdparty/mupdf"
       AND EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/mupdf-${MUPDF_REQUIRED_VERSION}-source/Makefile")
        set(MUPDF_SOURCE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/mupdf-${MUPDF_REQUIRED_VERSION}-source")
    endif()

    if(NOT EXISTS "${MUPDF_SOURCE_DIR}/Makefile"
       OR NOT EXISTS "${MUPDF_SOURCE_DIR}/include/mupdf/fitz/version.h")
        message(FATAL_ERROR
            "Bundled MuPDF source not found. Set MUPDF_SOURCE_DIR to the MuPDF ${MUPDF_REQUIRED_VERSION} source tree.")
    endif()

    file(STRINGS "${MUPDF_SOURCE_DIR}/include/mupdf/fitz/version.h" MUPDF_VERSION_LINE
        REGEX "^#define FZ_VERSION \"")
    string(REGEX MATCH "FZ_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${MUPDF_VERSION_LINE}")
    set(MUPDF_VERSION "${CMAKE_MATCH_1}")
    if(NOT MUPDF_VERSION VERSION_EQUAL MUPDF_REQUIRED_VERSION)
        message(FATAL_ERROR
            "MuPDF ${MUPDF_REQUIRED_VERSION} is required, found ${MUPDF_VERSION} in ${MUPDF_SOURCE_DIR}")
    endif()

    find_program(MUPDF_MAKE_EXECUTABLE NAMES make gmake REQUIRED)
    find_program(MUPDF_NPROC_EXECUTABLE NAMES nproc REQUIRED)
    execute_process(
        COMMAND "${MUPDF_NPROC_EXECUTABLE}"
        OUTPUT_VARIABLE MUPDF_BUILD_JOBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE MUPDF_NPROC_RESULT)
    if(NOT MUPDF_NPROC_RESULT EQUAL 0 OR NOT MUPDF_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "nproc did not return a valid CPU count")
    endif()

    pkg_check_modules(MUPDF_LEPTONICA REQUIRED IMPORTED_TARGET lept)
    pkg_check_modules(MUPDF_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(MUPDF_GUMBO REQUIRED IMPORTED_TARGET gumbo)
    pkg_check_modules(MUPDF_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(MUPDF_JPEG REQUIRED IMPORTED_TARGET libjpeg)
    pkg_check_modules(MUPDF_JBIG2DEC REQUIRED IMPORTED_TARGET jbig2dec)
    pkg_check_modules(MUPDF_OPENJPEG REQUIRED IMPORTED_TARGET libopenjp2)
    pkg_check_modules(MUPDF_BROTLI REQUIRED IMPORTED_TARGET libbrotlidec libbrotlienc)

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(MUPDF_BUILD_PROFILE release)
    else()
        set(MUPDF_BUILD_PROFILE debug)
    endif()

    set(MUPDF_BUILD_DIR "${CMAKE_BINARY_DIR}/mupdf/${MUPDF_BUILD_PROFILE}")
    set(MUPDF_CORE_LIBRARY "${MUPDF_BUILD_DIR}/libmupdf.a")
    set(MUPDF_THIRD_LIBRARY "${MUPDF_BUILD_DIR}/libmupdf-third.a")
    set(MUPDF_THREADS_LIBRARY "${MUPDF_BUILD_DIR}/libmupdf-threads.a")

    add_custom_command(
        OUTPUT
            "${MUPDF_CORE_LIBRARY}"
            "${MUPDF_THIRD_LIBRARY}"
            "${MUPDF_THREADS_LIBRARY}"
        COMMAND
            "${MUPDF_MAKE_EXECUTABLE}"
            -C "${MUPDF_SOURCE_DIR}"
            -j${MUPDF_BUILD_JOBS}
            OUT=${MUPDF_BUILD_DIR}
            build=${MUPDF_BUILD_PROFILE}
            mujs=no
            tesseract=yes
            xps=no
            extract=no
            archive=no
            barcode=no
            USE_SYSTEM_LIBS=yes
            USE_SYSTEM_TESSERACT=no
            USE_SYSTEM_LEPTONICA=yes
            USE_SYSTEM_CURL=no
            USE_SYSTEM_LCMS2=no
            USE_CMARK_GFM=no
            "XCFLAGS=-DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_FB2=0 -DFZ_ENABLE_MOBI=0 -DFZ_ENABLE_TXT=0 -DFZ_ENABLE_OFFICE=0 -DFZ_ENABLE_MD=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0"
            libs
            libmupdf-threads
        DEPENDS
            "${MUPDF_SOURCE_DIR}/Makefile"
            "${MUPDF_SOURCE_DIR}/Makerules"
            "${MUPDF_SOURCE_DIR}/Makethird"
            "${MUPDF_SOURCE_DIR}/include/mupdf/fitz/version.h"
        WORKING_DIRECTORY "${MUPDF_SOURCE_DIR}"
        VERBATIM
        COMMENT "Building bundled MuPDF ${MUPDF_VERSION} (${MUPDF_BUILD_PROFILE})"
    )

    add_custom_target(mupdf_static_build
        DEPENDS
            "${MUPDF_CORE_LIBRARY}"
            "${MUPDF_THIRD_LIBRARY}"
            "${MUPDF_THREADS_LIBRARY}")

    add_library(MuPDF::Engine INTERFACE IMPORTED GLOBAL)
    set_target_properties(MuPDF::Engine PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MUPDF_SOURCE_DIR}/include"
        INTERFACE_LINK_OPTIONS "-Wl,--gc-sections"
        INTERFACE_LINK_LIBRARIES
            "-Wl,--start-group;${MUPDF_CORE_LIBRARY};${MUPDF_THIRD_LIBRARY};${MUPDF_THREADS_LIBRARY};-Wl,--end-group;PkgConfig::MUPDF_LEPTONICA;PkgConfig::MUPDF_FREETYPE;PkgConfig::MUPDF_GUMBO;PkgConfig::MUPDF_HARFBUZZ;PkgConfig::MUPDF_JPEG;PkgConfig::MUPDF_JBIG2DEC;PkgConfig::MUPDF_OPENJPEG;PkgConfig::MUPDF_BROTLI;ZLIB::ZLIB;Threads::Threads;${CMAKE_DL_LIBS};m")
    add_dependencies(MuPDF::Engine mupdf_static_build)
endif()

set(MUPDF_HAS_OCR ON)
