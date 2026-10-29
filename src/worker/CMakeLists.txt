# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# mupdfng-worker: sandboxed worker executable
#
# IMPORTANT: This target MUST NOT link against Okular::Core, Qt GUI, or any
# KF6 library. It is the isolated MuPDF capability process.
# ---------------------------------------------------------------------------

find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(BOKO_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/engine/kindle/Cargo.toml")
set(BOKO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/boko")
set(BOKO_LIBRARY "${BOKO_TARGET_DIR}/release/libmupdfng_boko.a")
file(GLOB_RECURSE BOKO_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/thirdparty/boko/src/*.rs")
add_custom_command(OUTPUT "${BOKO_LIBRARY}"
    COMMAND "${CARGO_EXECUTABLE}" build --locked --release
        --manifest-path "${BOKO_MANIFEST}" --target-dir "${BOKO_TARGET_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/engine/kindle"
    DEPENDS "${BOKO_MANIFEST}" "${CMAKE_CURRENT_SOURCE_DIR}/engine/kindle/Cargo.lock"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/kindle/src/lib.rs"
        "${PROJECT_SOURCE_DIR}/thirdparty/boko/Cargo.toml" ${BOKO_SOURCES}
    VERBATIM)
add_custom_target(mupdfng_boko_build DEPENDS "${BOKO_LIBRARY}")
add_library(mupdfng_boko STATIC IMPORTED GLOBAL)
set_target_properties(mupdfng_boko PROPERTIES IMPORTED_LOCATION "${BOKO_LIBRARY}")
add_dependencies(mupdfng_boko mupdfng_boko_build)

set(mupdf_worker_core_SRCS
    runtime/command_service.cpp
    runtime/frame_pool.cpp
    engine/document_base.cpp
    engine/document_factory.cpp
    engine/mupdf_helpers.cpp
    engine/theming/device.cpp
    engine/theming/palette.cpp
    engine/pdf/document.cpp
    engine/pdf/page.cpp
    engine/pdf/render.cpp
    engine/pdf/annotations.cpp
    engine/pdf/forms.cpp
    engine/pdf/signature.cpp
    engine/pdf/save.cpp
    engine/pdf/outline.cpp
    engine/pdf/generated_outline.cpp
    engine/pdf/layers.cpp
    engine/pdf/metadata.cpp
    engine/pdf/fonts.cpp
    engine/pdf/embedded.cpp
    engine/epub/document.cpp
    engine/epub/page.cpp
    engine/epub/render.cpp
    engine/epub/outline.cpp
    engine/epub/metadata.cpp
    engine/epub/fonts.cpp
    engine/epub/save.cpp
    engine/epub/export_jobs.cpp
    engine/mobi/document.cpp
    engine/kindle/document.cpp
    engine/ocr/ocr.cpp
    engine/ocr/jobs.cpp
    engine/signer.cpp
    sys/sys.cpp
    sys/operation_budget.cpp
)

add_library(mupdf_worker_core OBJECT ${mupdf_worker_core_SRCS})

target_include_directories(mupdf_worker_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..       # src/ root for shared/ contract headers
    ${MU_GENERATED_INCLUDE_DIR}
)

target_link_libraries(mupdf_worker_core PRIVATE
    MuPDF::Engine
)
target_link_libraries(mupdf_worker_core PUBLIC
    Threads::Threads
    mupdf_shared
    mupdfng_boko
    ${CMAKE_DL_LIBS}
)

# The bundled MuPDF is built with tesseract enabled, so OCR is always available.
target_compile_definitions(mupdf_worker_core PRIVATE
    MU_WORKER_USE_SYSTEM_MUPDF=$<BOOL:${USE_SYSTEM_MUPDF}>
    TESSDATA_DIR="${TESSDATA_DIR}"
    SIGNATURE_FONT_DIR="${SIGNATURE_FONT_DIR}"
)
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(mupdf_worker_core PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()

add_executable(mupdfng-worker
    main.cpp
    runtime/worker_server.cpp
    sys/sandbox.cpp
)
target_link_libraries(mupdfng-worker PRIVATE
    mupdf_worker_core
    MuPDF::Engine
)
if(MU_DEBUG_BUILD)
    target_link_libraries(mupdfng-worker PRIVATE mupdf_ipc_debug)
endif()
set_target_properties(mupdfng-worker PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdf_worker_core PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdfng-worker PRIVATE MU_DEBUG_ENABLED)
endif()

target_include_directories(mupdfng-worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # worker/ headers
    ${CMAKE_CURRENT_SOURCE_DIR}/..       # src/ root for shared/ contract headers
    ${MU_GENERATED_INCLUDE_DIR}
    ${SECCOMP_INCLUDE_DIRS}
)

target_link_libraries(mupdfng-worker PRIVATE
    ${SECCOMP_LIBRARIES}
)

if(SECCOMP_FOUND)
    target_compile_definitions(mupdfng-worker PRIVATE MUPDF_HAVE_LIBSECCOMP)
endif()

target_compile_definitions(mupdfng-worker PRIVATE
    TESSDATA_DIR="${TESSDATA_DIR}"
    SIGNATURE_FONT_DIR="${SIGNATURE_FONT_DIR}"
)

install(TARGETS mupdfng-worker
    DESTINATION "${MUPDF_WORKER_INSTALL_DIR}"
)
