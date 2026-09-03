# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# okular-mupdf-worker: sandboxed worker executable
#
# IMPORTANT: This target MUST NOT link against Okular::Core, Qt GUI, or any
# KF6 library. It is the isolated MuPDF capability process.
# ---------------------------------------------------------------------------

set(mupdf_worker_core_SRCS
    runtime/command_service.cpp
    engine/document_base.cpp
    engine/pdf/document.cpp
    engine/pdf/page.cpp
    engine/pdf/render.cpp
    engine/pdf/annotations.cpp
    engine/pdf/forms.cpp
    engine/pdf/signature.cpp
    engine/pdf/save.cpp
    engine/pdf/outline.cpp
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
    engine/ocr/ocr.cpp
    engine/ocr/jobs.cpp
    engine/signer.cpp
    sys/sys.cpp
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
)

# The bundled MuPDF is built with tesseract enabled, so OCR is always available.
target_compile_definitions(mupdf_worker_core PRIVATE
    MUPDF_HAS_OCR
    TESSDATA_DIR="${TESSDATA_DIR}"
)

add_executable(okular-mupdf-worker
    main.cpp
    runtime/worker_server.cpp
    sys/sandbox.cpp
)
target_link_libraries(okular-mupdf-worker PRIVATE
    mupdf_worker_core
    MuPDF::Engine
)
set_target_properties(okular-mupdf-worker PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdf_worker_core PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(okular-mupdf-worker PRIVATE MU_DEBUG_ENABLED)
    target_sources(mupdf_worker_core PRIVATE ../shared/protocol/ipc_debug.cpp)
endif()

target_include_directories(okular-mupdf-worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # worker/ headers
    ${CMAKE_CURRENT_SOURCE_DIR}/..       # src/ root for shared/ contract headers
    ${MU_GENERATED_INCLUDE_DIR}
    ${SECCOMP_INCLUDE_DIRS}
)

target_link_libraries(okular-mupdf-worker PRIVATE
    ${SECCOMP_LIBRARIES}
)

if(SECCOMP_FOUND)
    target_compile_definitions(okular-mupdf-worker PRIVATE MUPDF_HAVE_LIBSECCOMP)
endif()

target_compile_definitions(okular-mupdf-worker PRIVATE MUPDF_HAS_OCR TESSDATA_DIR="${TESSDATA_DIR}")

install(TARGETS okular-mupdf-worker
    DESTINATION "${MUPDF_WORKER_INSTALL_DIR}"
)

# Expose the build-time path so the plugin can find the binary without install
set(WORKER_BUILD_PATH
    "$<TARGET_FILE:okular-mupdf-worker>"
    CACHE INTERNAL "Full path to the okular-mupdf-worker binary in the build tree")
