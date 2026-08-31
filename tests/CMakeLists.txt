# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Okular6 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui Network Test Xml)

add_library(mupdf_test_plugin_ocr STATIC
    ../src/plugin/caching/cache_file.cpp
    ../src/plugin/caching/ocr_cache.cpp
    ../src/shared/logging.cpp)
target_include_directories(mupdf_test_plugin_ocr PUBLIC
    ../src ../src/shared)
target_link_libraries(mupdf_test_plugin_ocr PUBLIC Qt6::Core ZLIB::ZLIB)

function(add_layer_test target source label)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Test)
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
    ../src/plugin/caching/cache_file.cpp
    ../src/plugin/caching/epub_cache.cpp
    ../src/plugin/worker_client.cpp
    ../src/plugin/worker_transport.cpp
    ../src/plugin/crypto/nss.cpp
    ../src/plugin/crypto/nss_state.cpp
    ../src/plugin/crypto/nss_signing.cpp
    ../src/plugin/crypto/certificate_info.cpp
    ../src/generator/signature_image.cpp
    ../src/generator/conversion/certificate.cpp
    ../src/plugin/crypto/certificate_database.cpp
    ../src/generator/proxy/certificate_store.cpp
    ../src/generator/proxy/form/signature.cpp
)
target_include_directories(test_mupdf PRIVATE
    support
    ../src
    ../src/worker
    ../src/shared
    ${MUPDF_GENERATED_INCLUDE_DIR}
    ${NSS_INCLUDE_DIRS})
target_include_directories(test_mupdf SYSTEM PRIVATE ../thirdparty)
target_link_libraries(test_mupdf PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Gui
    Qt6::Network
    Qt6::Xml
    Okular::Core
    mupdf_worker_core
    MuPDF::Engine
    ${NSS_LIBRARIES}
    ZLIB::ZLIB)
target_compile_definitions(test_mupdf PRIVATE
    TEST_EPUB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/epub"
    TEST_SIGNATURE_PDF_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pdf"
    WORKER_BUILD_PATH="$<TARGET_FILE:okular-mupdf-worker>"
    TESSDATA_DIR="${TESSDATA_DIR}")
if(MUPDF_HAS_OCR)
    target_compile_definitions(test_mupdf PRIVATE MUPDF_HAS_OCR)
endif()
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
target_link_libraries(test_plugin_ocrcache PRIVATE mupdf_test_plugin_ocr)

add_layer_test(test_plugin_epubcache plugin/test_epubcache.cpp plugin
    ../src/plugin/caching/cache_file.cpp
    ../src/plugin/caching/epub_cache.cpp)
target_include_directories(test_plugin_epubcache PRIVATE ../src)
target_link_libraries(test_plugin_epubcache PRIVATE Qt6::Core)

# NSS initialization is process-global, so exercise each runtime mode in a
# separate test process even though the modes share one test executable.
add_executable(test_nss_runtime
    plugin/test_nss_runtime.cpp
    ../src/plugin/crypto/nss.cpp
    ../src/plugin/crypto/nss_state.cpp
    ../src/plugin/crypto/nss_signing.cpp
    ../src/plugin/crypto/certificate_info.cpp
    ../src/plugin/crypto/certificate_database.cpp
    ../src/shared/logging.cpp)
target_include_directories(test_nss_runtime PRIVATE ../src ../src/shared ${NSS_INCLUDE_DIRS})
target_link_libraries(test_nss_runtime PRIVATE Qt6::Core Qt6::Test ${NSS_LIBRARIES})
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

add_layer_test(test_generator_placeholder_image generator/test_placeholder_image.cpp generator
    ../src/generator/placeholder_image.cpp)
target_include_directories(test_generator_placeholder_image PRIVATE ../src)
target_link_libraries(test_generator_placeholder_image PRIVATE Qt6::Gui KF6::I18n)

set(MUPDF_SETTINGS_SOURCE ${CMAKE_BINARY_DIR}/src/mupdfsettings.cpp)
set_source_files_properties(${MUPDF_SETTINGS_SOURCE} PROPERTIES GENERATED TRUE)
add_layer_test(test_generator_config generator/test_config.cpp generator
    ../src/generator/config/settings.cpp
    ${MUPDF_SETTINGS_SOURCE})
target_include_directories(test_generator_config PRIVATE ../src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(test_generator_config PRIVATE Qt6::Gui KF6::ConfigWidgets mupdf_test_plugin_ocr)
add_dependencies(test_generator_config mupdf_generator_settings_codegen)

# Generator adapters are the only unit tests linked to Okular proxy classes.
add_layer_test(test_generator_proxies generator/test_proxies.cpp generator
    ../src/generator/proxy/form/signature.cpp
    ../src/generator/proxy/form/text.cpp
    ../src/generator/proxy/form/checkbox.cpp
    ../src/generator/proxy/form/radio_button.cpp
    ../src/generator/proxy/form/push_button.cpp
    ../src/generator/proxy/form/choice.cpp
    ../src/generator/proxy/form/coordinator.cpp
    ../src/generator/conversion/annotation.cpp
    ../src/generator/conversion/certificate.cpp
    ../src/generator/conversion/document.cpp
    ../src/generator/conversion/text.cpp
    ../src/generator/signature_image.cpp)
target_include_directories(test_generator_proxies PRIVATE ../src)
target_link_libraries(test_generator_proxies PRIVATE Okular::Core Qt6::Gui)

# Security policy checks execute in a child because sandbox activation is irreversible.
add_layer_test(test_security_sandbox security/sandbox.cpp security ../src/worker/sys/sandbox.cpp)
target_include_directories(test_security_sandbox PRIVATE ../src ../src/shared ../src/worker ${SECCOMP_INCLUDE_DIRS})
target_link_libraries(test_security_sandbox PRIVATE ${SECCOMP_LIBRARIES})
if(SECCOMP_FOUND)
    target_compile_definitions(test_security_sandbox PRIVATE MUPDF_HAVE_LIBSECCOMP)
endif()
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_security_sandbox PRIVATE MU_DEBUG_ENABLED)
endif()

add_layer_test(test_security_transport security/transport.cpp security
    ../src/shared/model/validation.cpp
    ../src/shared/protocol/zpp_codec.cpp
    ../src/shared/logging.cpp)
target_include_directories(test_security_transport PRIVATE
    ../src ../src/shared ${MUPDF_GENERATED_INCLUDE_DIR})
target_include_directories(test_security_transport SYSTEM PRIVATE ../thirdparty)
target_link_libraries(test_security_transport PRIVATE mupdf_transport)
if(MU_DEBUG_BUILD)
    target_compile_definitions(test_security_transport PRIVATE MU_DEBUG_ENABLED)
    target_sources(test_security_transport PRIVATE ../src/shared/protocol/ipc_debug.cpp)
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
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -LE slow
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)
