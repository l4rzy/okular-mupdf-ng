# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui Network Test Xml)

function(add_layer_test target source label)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Test mupdf_shared)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS "${label}"
        TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_executable(test_mupdf
    test_runner.cpp
    worker/document.cpp
    worker/page.cpp
    worker/signature.cpp
    worker/ocr.cpp
    worker/epub.cpp
    worker/native_types.cpp
    worker/native_runtime.cpp
    integration/ipc.cpp
    signature/validation.cpp
    support/genpdf.cpp
)
target_include_directories(test_mupdf PRIVATE
    support
    ../src
    ../src/worker
    ${MU_GENERATED_INCLUDE_DIR}
    ${NSS_INCLUDE_DIRS})
target_link_libraries(test_mupdf PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Gui
    Qt6::Network
    Okular::Core
    mupdf_generator_adapters
    mupdf_generator_signature
    mupdf_plugin_cache
    mupdf_plugin_crypto
    mupdf_plugin_util
    mupdf_plugin_worker
    mupdf_shared
    mupdf_worker_core
    MuPDF::Engine
    ${NSS_LIBRARIES}
    ZLIB::ZLIB)
target_compile_definitions(test_mupdf PRIVATE
    TEST_EPUB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/epub"
    TEST_SIGNATURE_PDF_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pdf"
    WORKER_BUILD_PATH="$<TARGET_FILE:okular-mupdf-worker>"
    TESSDATA_DIR="${TESSDATA_DIR}")
target_compile_definitions(test_mupdf PRIVATE MUPDF_HAS_OCR)
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_mupdf PRIVATE MU_DEBUG_ENABLED)
endif()
add_dependencies(test_mupdf okular-mupdf-worker)

add_test(NAME test_mupdf COMMAND test_mupdf)
set_tests_properties(test_mupdf PROPERTIES
    LABELS "mupdf;worker;plugin;signature;integration"
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Plugin behaviour owns persistent cache state and uses a per-test root.
add_layer_test(test_plugin_ocrcache plugin/test_ocrcache.cpp plugin)
target_include_directories(test_plugin_ocrcache PRIVATE ../src)
target_link_libraries(test_plugin_ocrcache PRIVATE mupdf_plugin_cache)

add_layer_test(test_plugin_epubcache plugin/test_epubcache.cpp plugin)
target_include_directories(test_plugin_epubcache PRIVATE ../src)
target_link_libraries(test_plugin_epubcache PRIVATE Qt6::Core mupdf_plugin_cache)

add_layer_test(test_plugin_renderimage plugin/test_render_image.cpp plugin)
target_include_directories(test_plugin_renderimage PRIVATE ../src)
target_link_libraries(test_plugin_renderimage PRIVATE Qt6::Gui mupdf_plugin_util)

add_layer_test(test_plugin_signingtimestamp plugin/test_signing_timestamp.cpp plugin)
target_include_directories(test_plugin_signingtimestamp PRIVATE ../src)
target_link_libraries(test_plugin_signingtimestamp PRIVATE Qt6::Core mupdf_plugin_util)

# NSS initialization is process-global, so exercise each runtime mode in a
# separate test process even though the modes share one test executable.
add_executable(test_nss_runtime
    plugin/test_nss_runtime.cpp)
target_include_directories(test_nss_runtime PRIVATE ../src ${NSS_INCLUDE_DIRS})
target_link_libraries(test_nss_runtime PRIVATE Qt6::Core Qt6::Test mupdf_plugin_crypto)
foreach(runtime_test IN ITEMS
        defaultPathSelection
        defaultFallbackMode
        readWriteMode
        readOnlyMode
        noDbMode
        freshDatabaseMode
        urlSchemeDatabasePath)
    add_test(NAME test_nss_runtime_${runtime_test} COMMAND test_nss_runtime ${runtime_test})
    set_tests_properties(test_nss_runtime_${runtime_test} PROPERTIES
        LABELS "plugin;signature;security"
        TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_layer_test(test_generator_placeholder generator/test_placeholder.cpp generator)
target_include_directories(test_generator_placeholder PRIVATE ../src)
target_link_libraries(test_generator_placeholder PRIVATE mupdf_generator_placeholder Qt6::Gui KF6::I18n)

add_layer_test(test_plugin_ocrcontroller plugin/test_ocrcontroller.cpp plugin)
target_include_directories(test_plugin_ocrcontroller PRIVATE
    ../src ${MU_GENERATED_INCLUDE_DIR} ${NSS_INCLUDE_DIRS})
target_link_libraries(test_plugin_ocrcontroller PRIVATE mupdf_plugin_ocr Qt6::Gui Qt6::Network)

add_layer_test(test_generator_config generator/test_config.cpp generator)
target_include_directories(test_generator_config PRIVATE ../src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(test_generator_config PRIVATE Qt6::Gui mupdf_generator_settings)

# Generator adapters are the only unit tests linked to Okular proxy classes.
add_layer_test(test_generator_proxies generator/test_proxies.cpp generator)
target_include_directories(test_generator_proxies PRIVATE ../src)
# Explicit MuPDF include path: annotation.cpp uses PDF_ANNOT_* constants and
# must not silently fall back to system MuPDF headers.
target_link_libraries(test_generator_proxies PRIVATE
    mupdf_generator_adapters
    mupdf_generator_signature
    KF6::I18n
)

add_layer_test(test_generator_signingconversion generator/test_signing_conversion.cpp generator)
target_include_directories(test_generator_signingconversion PRIVATE ../src)
target_link_libraries(test_generator_signingconversion PRIVATE mupdf_generator_adapters KF6::I18n)

# Security policy checks execute in a child because sandbox activation is irreversible.
add_layer_test(test_security_sandbox security/sandbox.cpp security ../src/worker/sys/sandbox.cpp)
target_include_directories(test_security_sandbox PRIVATE ../src ../src/worker ${SECCOMP_INCLUDE_DIRS})
target_link_libraries(test_security_sandbox PRIVATE ${SECCOMP_LIBRARIES})
if(SECCOMP_FOUND)
    target_compile_definitions(test_security_sandbox PRIVATE MUPDF_HAVE_LIBSECCOMP)
endif()
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_security_sandbox PRIVATE MU_DEBUG_ENABLED)
endif()

add_layer_test(test_security_transport security/transport.cpp security)
target_include_directories(test_security_transport PRIVATE
    ../src ${MU_GENERATED_INCLUDE_DIR})
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_security_transport PRIVATE MU_DEBUG_ENABLED)
    target_link_libraries(test_security_transport PRIVATE mupdf_ipc_debug)
    target_link_libraries(test_mupdf PRIVATE mupdf_ipc_debug)
endif()

# Keep the isolated process free of GUI and host-application dependencies.
add_test(NAME test_worker_link_boundary
    COMMAND ${CMAKE_COMMAND}
        -DWORKER_PATH=$<TARGET_FILE:okular-mupdf-worker>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_worker_link_boundary.cmake)
set_tests_properties(test_worker_link_boundary PROPERTIES LABELS "security" TIMEOUT 15)

add_test(NAME test_plugin_boundary
    COMMAND ${CMAKE_COMMAND}
        -DPLUGIN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_plugin_boundary.cmake)
set_tests_properties(test_plugin_boundary PROPERTIES LABELS "plugin" TIMEOUT 15)

# The normal developer test command runs the MuPDF suites together and keeps
# the remaining lightweight tests as separate executables.
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)
