# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# okular-mupdf-worker: sandboxed worker executable
#
# IMPORTANT: This target MUST NOT link against Okular::Core, Qt GUI, or any
# KF6 library. It is the isolated MuPDF capability process.
# ---------------------------------------------------------------------------

# Document engine is split by concern; the list is shared with the test targets.
set(MUPDF_WORKER_DOCUMENT_SOURCES
    document_base.cpp
    pdf/document.cpp
    pdf/page.cpp
    pdf/render.cpp
    pdf/annotations.cpp
    pdf/forms.cpp
    pdf/signature.cpp
    pdf/save.cpp
    pdf/outline.cpp
    pdf/metadata.cpp
    pdf/fonts.cpp
    pdf/embedded.cpp
    epub/document.cpp
    epub/page.cpp
    epub/render.cpp
    epub/outline.cpp
    epub/metadata.cpp
    epub/fonts.cpp
    epub/save.cpp
    CACHE INTERNAL "MuPDF worker document engine source files")
set(mupdf_worker_document_sources "")
foreach(source IN LISTS MUPDF_WORKER_DOCUMENT_SOURCES)
    list(APPEND mupdf_worker_document_sources engine/${source})
endforeach()

set(mupdf_worker_core_SRCS
    runtime/command_service.cpp
    ${mupdf_worker_document_sources}
    engine/ocr/ocr.cpp
    engine/ocr/jobs.cpp
    engine/signer.cpp
    sys/sys.cpp
    ../shared/logging.cpp
    ../shared/model/validation.cpp
    ../shared/protocol/zpp_codec.cpp
)

add_library(mupdf_worker_core OBJECT ${mupdf_worker_core_SRCS})

target_include_directories(mupdf_worker_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..       # src/ root for shared/ contract headers
    ${MUPDF_GENERATED_INCLUDE_DIR}
)
target_include_directories(mupdf_worker_core SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty # vendored thirdparty/ headers
)

target_link_libraries(mupdf_worker_core PRIVATE
    MuPDF::Engine
)
target_link_libraries(mupdf_worker_core PUBLIC
    Threads::Threads
    mupdf_transport
)

if(MUPDF_HAS_OCR)
    target_compile_definitions(mupdf_worker_core PRIVATE
        MUPDF_HAS_OCR
        TESSDATA_DIR="${TESSDATA_DIR}"
    )
endif()

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
    ${MUPDF_GENERATED_INCLUDE_DIR}
    ${SECCOMP_INCLUDE_DIRS}
)
target_include_directories(okular-mupdf-worker SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty # vendored thirdparty/ headers
)

target_link_libraries(okular-mupdf-worker PRIVATE
    ${SECCOMP_LIBRARIES}
    Threads::Threads
)

if(SECCOMP_FOUND)
    target_compile_definitions(okular-mupdf-worker PRIVATE MUPDF_HAVE_LIBSECCOMP)
endif()

if(MUPDF_HAS_OCR)
    target_compile_definitions(okular-mupdf-worker PRIVATE
        MUPDF_HAS_OCR
    )
endif()
target_compile_definitions(okular-mupdf-worker PRIVATE TESSDATA_DIR="${TESSDATA_DIR}")

install(TARGETS okular-mupdf-worker
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/okular-mupdf-ng"
)

# Expose the build-time path so the plugin can find the binary without install
set(WORKER_BUILD_PATH
    "$<TARGET_FILE:okular-mupdf-worker>"
    CACHE INTERNAL "Full path to the okular-mupdf-worker binary in the build tree")
