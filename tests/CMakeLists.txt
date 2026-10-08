# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 REQUIRED COMPONENTS Core Gui Test Xml)
if(BUILD_OKULAR_GENERATOR)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent Network)
endif()

function(add_layer_test target source label)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Test mupdf_shared)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS "${label}"
        TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_layer_test(test_worker_framepool worker/frame_pool.cpp worker
    ../src/worker/runtime/frame_pool.cpp ../src/worker/sys/sys.cpp)
target_include_directories(test_worker_framepool PRIVATE ../src ../src/worker)

if(BUILD_OKULAR_GENERATOR)
add_executable(test_render_worker support/render_worker.cpp ../src/worker/runtime/worker_server.cpp)
set_target_properties(test_render_worker PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(test_render_worker PRIVATE ../src ../src/worker ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_render_worker PRIVATE mupdf_worker_core MuPDF::Engine)
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_render_worker PRIVATE MU_DEBUG_ENABLED)
    target_link_libraries(test_render_worker PRIVATE mupdf_ipc_debug)
endif()

add_executable(test_mupdf
    test_runner.cpp
    worker/document.cpp
    worker/page.cpp
    worker/signature.cpp
    worker/ocr.cpp
    worker/epub.cpp
    worker/mobi.cpp
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
    TEST_SYSTEM_MUPDF=$<BOOL:${USE_SYSTEM_MUPDF}>
    HAVE_NEW_SIGNATURE_API=1
    TEST_EPUB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/epub"
    TEST_MOBI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/mobi"
    TEST_BUNDLED_MOBI_TOC=$<NOT:$<BOOL:${USE_SYSTEM_MUPDF}>>
    TEST_SIGNATURE_PDF_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pdf"
    FORM_JS_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/form_javascript_calculation.pdf"
    FORM_JS_BUTTON_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/form_javascript_button.pdf"
    FORM_JS_CONTROLS_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/form_javascript_controls.pdf"
    FORM_JS_PARTIAL_ERROR_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/form_javascript_partial_error.pdf"
    WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    RENDER_WORKER_BUILD_PATH="$<TARGET_FILE:test_render_worker>"
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    TESSDATA_DIR="${TESSDATA_DIR}")
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_mupdf PRIVATE MU_DEBUG_ENABLED)
endif()
add_dependencies(test_mupdf mupdfng-worker mupdfng-cli test_render_worker)

set(MUPDF_TEST_SUITES
    "test_worker_document|worker"
    "test_worker_page|worker"
    "test_worker_signature|worker,signature"
    "test_plugin_signature|plugin,signature"
    "test_worker_ocr|worker"
    "test_worker_epub|worker"
    "test_worker_mobi|worker"
    "test_worker_native_types|worker"
    "test_worker_native_runtime|worker,security"
    "test_integration_ipc|integration,plugin,worker")
foreach(suite_definition IN LISTS MUPDF_TEST_SUITES)
    string(REPLACE "|" ";" suite_fields "${suite_definition}")
    list(GET suite_fields 0 suite_name)
    list(GET suite_fields 1 suite_labels)
    string(REPLACE "," ";" suite_labels "${suite_labels}")
    add_test(NAME "test_mupdf_${suite_name}"
        COMMAND test_mupdf --suite "${suite_name}")
    set_tests_properties("test_mupdf_${suite_name}" PROPERTIES
        LABELS "mupdf;${suite_labels}"
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

endif()

# mupdfng-cli argument parsing: pure decision logic, no worker needed.
add_layer_test(test_tools_cliargs tools/test_cli_args.cpp tools ../src/tools/cli/cli_args.cpp)
target_include_directories(test_tools_cliargs PRIVATE ../src ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_tools_cliargs PRIVATE Qt6::Core)

add_executable(test_tools_cli_xfdf tools/test_cli_xfdf.cpp)
target_link_libraries(test_tools_cli_xfdf PRIVATE Qt6::Core Qt6::Test Qt6::Xml)
target_compile_definitions(test_tools_cli_xfdf PRIVATE
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/digital_signature.pdf"
    TEST_CALLOUT_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/freetext_callout_no_intent.pdf")
add_dependencies(test_tools_cli_xfdf mupdfng-cli mupdfng-worker)
add_test(NAME test_tools_cli_xfdf COMMAND test_tools_cli_xfdf)
set_tests_properties(test_tools_cli_xfdf PROPERTIES
    LABELS "tools;integration"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_tools_cli_ocr tools/test_cli_ocr.cpp)
target_link_libraries(test_tools_cli_ocr PRIVATE Qt6::Core Qt6::Test)
target_compile_definitions(test_tools_cli_ocr PRIVATE
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/ocr_images.pdf"
    TESSDATA_DIR="${TESSDATA_DIR}")
add_dependencies(test_tools_cli_ocr mupdfng-cli mupdfng-worker)
add_test(NAME test_tools_cli_ocr COMMAND test_tools_cli_ocr)
set_tests_properties(test_tools_cli_ocr PROPERTIES
    LABELS "tools;integration"
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_layer_test(test_tools_cli_export tools/test_cli_export.cpp "tools;integration")
target_link_libraries(test_tools_cli_export PRIVATE MuPDF::Engine)
target_compile_definitions(test_tools_cli_export PRIVATE
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    TEST_EPUB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/epub/sample.epub")
add_dependencies(test_tools_cli_export mupdfng-cli mupdfng-worker)
set_tests_properties(test_tools_cli_export PROPERTIES TIMEOUT 150)

add_layer_test(test_tools_layoutconfig tools/test_layout_config.cpp tools)
target_link_libraries(test_tools_layoutconfig PRIVATE mupdf_cli_layout)
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(test_tools_layoutconfig PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()
if(BUILD_OKULAR_GENERATOR)
    target_link_libraries(test_tools_layoutconfig PRIVATE mupdf_generator_settings)
    target_include_directories(test_tools_layoutconfig PRIVATE ${CMAKE_BINARY_DIR}/src)
    target_compile_definitions(test_tools_layoutconfig PRIVATE HAVE_KDE_CONFIG)
endif()

add_test(NAME test_cli_link_boundary
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:mupdfng-cli>
        -DCLI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/cli
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_cli_link_boundary.cmake)
set_tests_properties(test_cli_link_boundary PROPERTIES LABELS "tools" TIMEOUT 15)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)

if(NOT BUILD_OKULAR_GENERATOR)
    return()
endif()

# Plugin behaviour owns persistent cache state and uses a per-test root.
add_layer_test(test_plugin_ocrcache plugin/test_ocrcache.cpp plugin)
target_include_directories(test_plugin_ocrcache PRIVATE ../src)
target_link_libraries(test_plugin_ocrcache PRIVATE mupdf_plugin_cache)

add_layer_test(test_plugin_cachevacuum plugin/test_cachevacuum.cpp plugin)
target_include_directories(test_plugin_cachevacuum PRIVATE ../src)
target_link_libraries(test_plugin_cachevacuum PRIVATE mupdf_plugin_cache)

add_layer_test(test_plugin_ocroptions plugin/test_ocr_options.cpp plugin)
target_include_directories(test_plugin_ocroptions PRIVATE ../src)
target_link_libraries(test_plugin_ocroptions PRIVATE mupdf_plugin_util)

add_layer_test(test_plugin_processmemory plugin/test_process_memory.cpp plugin)
target_include_directories(test_plugin_processmemory PRIVATE ../src)
target_link_libraries(test_plugin_processmemory PRIVATE mupdf_plugin_util)

add_layer_test(test_plugin_epubcache plugin/test_epubcache.cpp plugin)
target_include_directories(test_plugin_epubcache PRIVATE ../src)
target_link_libraries(test_plugin_epubcache PRIVATE Qt6::Core mupdf_plugin_cache)

add_layer_test(test_plugin_layerresponse plugin/test_layer_response.cpp plugin)
target_include_directories(test_plugin_layerresponse PRIVATE ../src)

add_layer_test(test_plugin_renderimage plugin/test_render_image.cpp plugin)
target_include_directories(test_plugin_renderimage PRIVATE ../src)
target_link_libraries(test_plugin_renderimage PRIVATE Qt6::Gui mupdf_plugin_util)

add_layer_test(test_plugin_signingauthorization plugin/test_signing_authorization.cpp plugin)
target_include_directories(test_plugin_signingauthorization PRIVATE ../src)

add_layer_test(test_plugin_signingtimestamp plugin/test_signing_timestamp.cpp plugin)
target_include_directories(test_plugin_signingtimestamp PRIVATE ../src)
target_link_libraries(test_plugin_signingtimestamp PRIVATE Qt6::Core mupdf_plugin_util)

add_layer_test(test_plugin_xfdfparse plugin/test_xfdf_parse.cpp plugin)
target_include_directories(test_plugin_xfdfparse PRIVATE ../src)
target_link_libraries(test_plugin_xfdfparse PRIVATE mupdf_plugin_util)

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

add_layer_test(test_generator_layers generator/test_layers_model.cpp generator)
target_link_libraries(test_generator_layers PRIVATE mupdf_generator_adapters KF6::ConfigCore)
# SettingsCore uses an unqualified include for its generated export header.
find_path(OKULAR_CORE_INCLUDE_DIR okularcore_export.h PATH_SUFFIXES okular/core REQUIRED)
target_include_directories(test_generator_layers PRIVATE ${OKULAR_CORE_INCLUDE_DIR})
target_compile_definitions(test_generator_layers PRIVATE
    TEST_LAYER_PDF="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf"
    TEST_MOBI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mobi/legacy-palmdoc.mobi"
    TEST_MOBI_TOC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mobi/toc-palmdoc.mobi"
    TEST_BUNDLED_MOBI_TOC=$<NOT:$<BOOL:${USE_SYSTEM_MUPDF}>>
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:mupdfng-worker>")
add_dependencies(test_generator_layers okularGenerator_mupdfng mupdfng-worker)

add_layer_test(test_generator_ocr generator/test_ocr.cpp generator)
target_include_directories(test_generator_ocr PRIVATE ${OKULAR_CORE_INCLUDE_DIR})
target_link_libraries(test_generator_ocr PRIVATE Qt6::Gui Okular::Core KF6::ConfigCore)
target_compile_definitions(test_generator_ocr PRIVATE
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:mupdfng-worker>"
    TESSDATA_DIR="${TESSDATA_DIR}")
add_dependencies(test_generator_ocr okularGenerator_mupdfng mupdfng-worker)

add_layer_test(test_generator_placeholder generator/test_placeholder.cpp generator)
target_include_directories(test_generator_placeholder PRIVATE ../src)
target_link_libraries(test_generator_placeholder PRIVATE mupdf_generator_placeholder Qt6::Gui KF6::I18n)

add_layer_test(test_plugin_ocrcontroller plugin/test_ocrcontroller.cpp plugin)
target_include_directories(test_plugin_ocrcontroller PRIVATE
    ../src ${MU_GENERATED_INCLUDE_DIR} ${NSS_INCLUDE_DIRS})
target_link_libraries(test_plugin_ocrcontroller PRIVATE mupdf_plugin_ocr Qt6::Gui Qt6::Network)
target_compile_definitions(test_plugin_ocrcontroller PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf")
add_dependencies(test_plugin_ocrcontroller mupdfng-worker)

add_layer_test(test_plugin_workerclientstate plugin/test_worker_client_state.cpp plugin)
target_include_directories(test_plugin_workerclientstate PRIVATE ../src)
target_link_libraries(test_plugin_workerclientstate PRIVATE mupdf_plugin_worker)

add_layer_test(test_generator_config generator/test_config.cpp generator
    ../src/generator/config/certmanager/self_signed_dialog.cpp)
target_include_directories(test_generator_config PRIVATE ../src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(test_generator_config PRIVATE Qt6::Gui KF6::I18n mupdf_generator_settings)

add_layer_test(test_generator_signaturepreview generator/test_signature_preview.cpp generator)
target_include_directories(test_generator_signaturepreview PRIVATE ../src)
target_link_libraries(test_generator_signaturepreview PRIVATE Qt6::Gui mupdf_generator_settings)

# Generator adapters are the only unit tests linked to Okular proxy classes.
add_layer_test(test_generator_proxies generator/test_proxies.cpp generator)
target_include_directories(test_generator_proxies PRIVATE ../src)
target_compile_definitions(test_generator_proxies PRIVATE HAVE_NEW_SIGNATURE_API=1)
target_link_libraries(test_generator_proxies PRIVATE
    mupdf_generator_adapters
    mupdf_generator_signature
    KF6::I18n
)

add_layer_test(test_generator_signingconversion generator/test_signing_conversion.cpp generator)
target_include_directories(test_generator_signingconversion PRIVATE ../src)
target_link_libraries(test_generator_signingconversion PRIVATE mupdf_generator_adapters KF6::I18n)

add_layer_test(test_generator_xfdf generator/test_xfdf.cpp generator)
target_include_directories(test_generator_xfdf PRIVATE ../src)
target_link_libraries(test_generator_xfdf PRIVATE mupdf_generator_adapters KF6::I18n)

# Security policy checks execute in a child because sandbox activation is irreversible.
add_layer_test(test_security_sandbox security/sandbox.cpp security ../src/worker/sys/sandbox.cpp ../src/worker/sys/operation_budget.cpp)
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

# Keep the isolated process and shared contract free of Qt and host dependencies.
add_test(NAME test_worker_link_boundary
    COMMAND ${CMAKE_COMMAND}
        -DWORKER_PATH=$<TARGET_FILE:mupdfng-worker>
        -DWORKER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/worker
        -DSHARED_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/shared
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_worker_link_boundary.cmake)
set_tests_properties(test_worker_link_boundary PROPERTIES LABELS "security" TIMEOUT 15)

add_test(NAME test_plugin_boundary
    COMMAND ${CMAKE_COMMAND}
        -DPLUGIN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_plugin_boundary.cmake)
set_tests_properties(test_plugin_boundary PROPERTIES LABELS "plugin" TIMEOUT 15)

add_layer_test(test_plugin_pdf_toc plugin/test_pdf_toc.cpp plugin ../src/worker/engine/pdf/generated_outline.cpp)
target_include_directories(test_plugin_pdf_toc PRIVATE ../src ../src/worker)
target_link_libraries(test_plugin_pdf_toc PRIVATE mupdf_plugin_cache MuPDF::Engine)
