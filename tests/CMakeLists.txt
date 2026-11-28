# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 REQUIRED COMPONENTS Core Gui Test Xml)
find_package(Git QUIET)
if(GIT_FOUND)
    add_test(NAME test_build_info
        COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/git-build-info-test"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_build_info.cmake")
    set_tests_properties(test_build_info PROPERTIES LABELS "tools" TIMEOUT 30)
endif()
if(BUILD_OKULAR_GENERATOR)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent Network)
endif()

function(add_layer_test target source label)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Test ng_common)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS "${label}"
        TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_layer_test(test_security_sandbox_status security/sandbox_status.cpp security)

# mupdfng-cli argument parsing: pure decision logic, no worker needed.
add_layer_test(test_tools_cliargs tools/test_cli_args.cpp tools ../src/tools/cli/cli_args.cpp)
target_include_directories(test_tools_cliargs PRIVATE ../src ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_tools_cliargs PRIVATE Qt6::Core)
add_dependencies(test_tools_cliargs ng_build_info)

add_executable(test_tools_cli_xfdf tools/test_cli_xfdf.cpp)
target_link_libraries(test_tools_cli_xfdf PRIVATE Qt6::Core Qt6::Test Qt6::Xml)
target_compile_definitions(test_tools_cli_xfdf PRIVATE
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/digital_signature.pdf"
    TEST_CALLOUT_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/freetext_callout_no_intent.pdf")
add_dependencies(test_tools_cli_xfdf mupdfng-cli paperbox-worker)
add_test(NAME test_tools_cli_xfdf COMMAND test_tools_cli_xfdf)
set_tests_properties(test_tools_cli_xfdf PROPERTIES
    LABELS "tools;integration"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_tools_cli_ocr tools/test_cli_ocr.cpp)
target_link_libraries(test_tools_cli_ocr PRIVATE Qt6::Core Qt6::Test)
target_compile_definitions(test_tools_cli_ocr PRIVATE
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/ocr_images.pdf"
    TESSDATA_DIR="${TESSDATA_DIR}")
add_dependencies(test_tools_cli_ocr mupdfng-cli paperbox-worker)
add_test(NAME test_tools_cli_ocr COMMAND test_tools_cli_ocr)
set_tests_properties(test_tools_cli_ocr PROPERTIES
    LABELS "tools;integration"
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_layer_test(test_tools_cli_export tools/test_cli_export.cpp "tools;integration")
target_link_libraries(test_tools_cli_export PRIVATE paperbox_mupdf_engine)
target_compile_definitions(test_tools_cli_export PRIVATE
    MU_DISABLE_WORKER_SANDBOX=$<BOOL:${DISABLE_WORKER_SANDBOX}>
    MUPDFNG_CLI_PATH="$<TARGET_FILE:mupdfng-cli>"
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_EPUB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/epub/sample.epub"
    TEST_MOBI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/mobi")
add_dependencies(test_tools_cli_export mupdfng-cli paperbox-worker)
set_tests_properties(test_tools_cli_export PROPERTIES TIMEOUT 150)

add_layer_test(test_tools_layoutconfig tools/test_layout_config.cpp tools)
target_link_libraries(test_tools_layoutconfig PRIVATE ng_cli_layout)
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(test_tools_layoutconfig PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()
if(BUILD_OKULAR_GENERATOR)
    target_link_libraries(test_tools_layoutconfig PRIVATE ng_generator_settings)
    target_include_directories(test_tools_layoutconfig PRIVATE ${CMAKE_BINARY_DIR}/src)
    target_compile_definitions(test_tools_layoutconfig PRIVATE HAVE_KDE_CONFIG)
endif()

if(BUILD_OKULAR_GENERATOR)
    set(plugin_link_argument "-DPLUGIN_PATH=$<TARGET_FILE:okularGenerator_mupdfng>")
endif()
add_test(NAME test_cli_link_boundary
    COMMAND ${CMAKE_COMMAND}
        -DCLI_PATH=$<TARGET_FILE:mupdfng-cli>
        -DCLI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/cli
        -DWORKER_PATH=$<TARGET_FILE:paperbox-worker>
        ${plugin_link_argument}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_cli_link_boundary.cmake)
set_tests_properties(test_cli_link_boundary PROPERTIES LABELS "tools" TIMEOUT 15)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)

if(NOT BUILD_OKULAR_GENERATOR)
    return()
endif()

add_layer_test(test_plugin_ocroptions plugin/test_ocr_options.cpp plugin)
target_include_directories(test_plugin_ocroptions PRIVATE ../src)
target_link_libraries(test_plugin_ocroptions PRIVATE ng_plugin_util)

add_layer_test(test_plugin_legacyocrcache plugin/test_ocrcache.cpp plugin)
target_link_libraries(test_plugin_legacyocrcache PRIVATE ng_plugin_util)

add_layer_test(test_plugin_certificatedatabase plugin/test_certificate_database.cpp "plugin;signature")
target_link_libraries(test_plugin_certificatedatabase PRIVATE ng_plugin_crypto ng_plugin_worker KF6::I18n Qt6::Widgets)
target_compile_definitions(test_plugin_certificatedatabase PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_SIGNATURE_PDF_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pdf")
target_link_options(test_plugin_certificatedatabase PRIVATE "LINKER:--wrap=SEC_PKCS12DecoderImportBags")
set_tests_properties(test_plugin_certificatedatabase PROPERTIES TIMEOUT 180)

add_layer_test(test_plugin_renderimage plugin/test_render_image.cpp plugin)
target_include_directories(test_plugin_renderimage PRIVATE ../src)
target_link_libraries(test_plugin_renderimage PRIVATE Qt6::Gui ng_plugin_util)

add_layer_test(test_plugin_signatureimage plugin/test_signature_image.cpp plugin)
target_include_directories(test_plugin_signatureimage PRIVATE ../src)
target_link_libraries(test_plugin_signatureimage PRIVATE Qt6::Gui ng_plugin_util)

add_layer_test(test_plugin_signingtimestamp plugin/test_signing_timestamp.cpp plugin)
target_include_directories(test_plugin_signingtimestamp PRIVATE ../src)
target_link_libraries(test_plugin_signingtimestamp PRIVATE Qt6::Core ng_plugin_util)

add_layer_test(test_plugin_xfdfparse plugin/test_xfdf_parse.cpp plugin)
target_include_directories(test_plugin_xfdfparse PRIVATE ../src)
target_link_libraries(test_plugin_xfdfparse PRIVATE ng_plugin_util)

# NSS initialization is process-global, so exercise each runtime mode in a
# separate test process even though the modes share one test executable.
add_executable(test_nss_runtime
    plugin/test_nss_runtime.cpp)
target_include_directories(test_nss_runtime PRIVATE ../src)
target_link_libraries(test_nss_runtime PRIVATE Qt6::Core Qt6::Test ng_plugin_crypto)
foreach(runtime_test IN ITEMS
        defaultPathSelection
        defaultFallbackMode
        readWriteMode
        readOnlyMode
        noDbMode
        freshDatabaseMode
        urlSchemeDatabasePath
        signingAuthenticationFailures)
    add_test(NAME test_nss_runtime_${runtime_test} COMMAND test_nss_runtime ${runtime_test})
    set_tests_properties(test_nss_runtime_${runtime_test} PROPERTIES
        LABELS "plugin;signature;security"
        TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_layer_test(test_generator_layers generator/test_layers_model.cpp generator)
target_link_libraries(test_generator_layers PRIVATE ng_generator_adapters KF6::ConfigCore)
target_include_directories(test_generator_layers PRIVATE ${OKULAR_CORE_INCLUDE_DIR})
target_compile_definitions(test_generator_layers PRIVATE
    TEST_LAYER_PDF="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf"
    TEST_MOBI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mobi/legacy-palmdoc.mobi"
    TEST_MOBI_TOC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mobi/toc-palmdoc.mobi"
    TEST_BUNDLED_MOBI_TOC=$<NOT:$<BOOL:${USE_SYSTEM_MUPDF}>>
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:paperbox-worker>")
add_dependencies(test_generator_layers okularGenerator_mupdfng paperbox-worker)

add_layer_test(test_generator_forms generator/test_forms.cpp generator)
target_include_directories(test_generator_forms PRIVATE ${OKULAR_CORE_INCLUDE_DIR})
target_link_libraries(test_generator_forms PRIVATE Qt6::Gui Qt6::PrintSupport Okular::Core KF6::ConfigCore)
target_compile_definitions(test_generator_forms PRIVATE
    TEST_FORM_JAVASCRIPT=$<BOOL:${ENABLE_FORM_JAVASCRIPT}>
    TEST_FORM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pdf/forms"
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:paperbox-worker>")
add_dependencies(test_generator_forms okularGenerator_mupdfng paperbox-worker)

add_layer_test(test_generator_epub generator/test_epub.cpp "generator;integration" support/genpdf.cpp)
target_include_directories(test_generator_epub PRIVATE "${PAPERBOX_SOURCE_DIR}/src" ../src ${OKULAR_CORE_INCLUDE_DIR} ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_generator_epub PRIVATE Qt6::Gui Qt6::PrintSupport Okular::Core KF6::ConfigCore ng_plugin_util paperbox_mupdf_engine KF6::XmlGui)
target_compile_definitions(test_generator_epub PRIVATE
    TEST_BUNDLED_MUPDF=$<NOT:$<BOOL:${USE_SYSTEM_MUPDF}>>
    TEST_EPUB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/epub"
    TEST_XPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/xps"
    TEST_MOBI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/mobi"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/ocr_images.pdf"
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:paperbox-worker>")
add_dependencies(test_generator_epub okularGenerator_mupdfng paperbox-worker)
set_tests_properties(test_generator_epub PROPERTIES TIMEOUT 150)

add_layer_test(test_generator_ocr generator/test_ocr.cpp generator)
target_include_directories(test_generator_ocr PRIVATE ../src ${OKULAR_CORE_INCLUDE_DIR} ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_generator_ocr PRIVATE
    Qt6::Gui
    Okular::Core
    KF6::ConfigCore
    KF6::XmlGui
    ng_generator_settings
    ng_plugin_util
    ng_plugin_worker)
target_compile_definitions(test_generator_ocr PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/ocr_images.pdf"
    TEST_EPUB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/epub/sample.epub"
    TEST_MOBI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mobi/legacy-palmdoc.mobi"
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:paperbox-worker>"
    TESSDATA_DIR="${TESSDATA_DIR}")
add_dependencies(test_generator_ocr okularGenerator_mupdfng paperbox-worker)
if(NOT MU_OKULAR_PAGE_PLACEHOLDER_COLOR)
    target_sources(test_generator_ocr PRIVATE ../src/generator/page/placeholder.cpp)
endif()

add_layer_test(test_generator_page_error generator/test_page_error.cpp generator)
target_include_directories(test_generator_page_error PRIVATE ../src)
target_link_libraries(test_generator_page_error PRIVATE ng_generator_page_error Qt6::Gui KF6::I18n)

add_layer_test(test_plugin_ocrcontroller plugin/test_ocrcontroller.cpp plugin)
target_include_directories(test_plugin_ocrcontroller PRIVATE
    ../src ${MU_GENERATED_INCLUDE_DIR})
target_link_libraries(test_plugin_ocrcontroller PRIVATE ng_plugin_ocr Qt6::Gui Qt6::Network)
target_compile_definitions(test_plugin_ocrcontroller PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf")
add_dependencies(test_plugin_ocrcontroller paperbox-worker)

add_layer_test(test_plugin_workerclientstate plugin/test_worker_client_state.cpp plugin)
target_include_directories(test_plugin_workerclientstate PRIVATE ../src)
target_link_libraries(test_plugin_workerclientstate PRIVATE ng_plugin_worker)
target_compile_definitions(test_plugin_workerclientstate PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf")

add_layer_test(test_generator_config generator/test_config.cpp generator
    ../src/generator/config/certmanager/self_signed_dialog.cpp
    ../src/generator/config/certmanager/manager_dialog.cpp)
target_include_directories(test_generator_config PRIVATE "${PAPERBOX_SOURCE_DIR}/src" ../src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(test_generator_config PRIVATE Qt6::Gui Qt6::Concurrent KF6::I18n ng_generator_settings ng_plugin_crypto)

add_layer_test(test_generator_pageplaceholder generator/test_page_placeholder.cpp generator
    ../src/generator/page/placeholder.cpp)
target_include_directories(test_generator_pageplaceholder PRIVATE ../src ${OKULAR_CORE_INCLUDE_DIR})
target_link_libraries(test_generator_pageplaceholder PRIVATE Qt6::Gui Okular::Core KF6::ConfigCore)

add_layer_test(test_generator_signaturepreview generator/test_signature_preview.cpp generator)
target_include_directories(test_generator_signaturepreview PRIVATE ../src)
target_link_libraries(test_generator_signaturepreview PRIVATE Qt6::Gui ng_generator_settings)

# Generator adapters are the only unit tests linked to Okular proxy classes.
add_layer_test(test_generator_proxies generator/test_proxies.cpp generator)
target_include_directories(test_generator_proxies PRIVATE ../src)
target_link_libraries(test_generator_proxies PRIVATE
    ng_generator_adapters
    ng_generator_signature
    KF6::I18n
)

add_layer_test(test_generator_linkpreview generator/test_link_preview.cpp "generator;integration")
target_link_libraries(test_generator_linkpreview PRIVATE ng_generator_adapters ng_plugin_worker)
target_compile_definitions(test_generator_linkpreview PRIVATE
    WORKER_BUILD_PATH="$<TARGET_FILE:paperbox-worker>"
    TEST_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/pdf/layers.pdf")
add_dependencies(test_generator_linkpreview paperbox-worker)

add_layer_test(test_generator_signingconversion generator/test_signing_conversion.cpp generator)
target_include_directories(test_generator_signingconversion PRIVATE ../src)
target_link_libraries(test_generator_signingconversion PRIVATE ng_generator_adapters KF6::I18n)

add_layer_test(test_generator_xfdf generator/test_xfdf.cpp generator)
target_include_directories(test_generator_xfdf PRIVATE ../src ${OKULAR_CORE_INCLUDE_DIR})
target_link_libraries(test_generator_xfdf PRIVATE ng_generator_adapters KF6::I18n KF6::XmlGui)
target_compile_definitions(test_generator_xfdf PRIVATE
    TEST_PLUGIN_ROOT="$<TARGET_FILE_DIR:okularGenerator_mupdfng>/.."
    TEST_WORKER_DIR="$<TARGET_FILE_DIR:paperbox-worker>")
add_dependencies(test_generator_xfdf okularGenerator_mupdfng paperbox-worker)

add_test(NAME test_plugin_boundary
    COMMAND ${CMAKE_COMMAND}
        -DPLUGIN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_plugin_boundary.cmake)
set_tests_properties(test_plugin_boundary PROPERTIES LABELS "plugin" TIMEOUT 15)
