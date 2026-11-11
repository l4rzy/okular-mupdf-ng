# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

# Qt-free code shared by the plugin and the sandboxed worker: transport
# channels, logging, model validation, and the zpp serialization codec.
add_library(ng_shared STATIC
  shared/transport/ctrl_channel.cpp
  shared/transport/fd_channel.cpp
  shared/transport/poll.cpp
  shared/logging.cpp
  shared/model/validation.cpp
  shared/model/export.cpp
  shared/protocol/zpp_codec.cpp
)
add_dependencies(ng_shared ng_build_info)
set_target_properties(ng_shared PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(ng_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(ng_shared SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty # zpp_bits.h for the codec
)
if(MU_DEBUG_BUILD)
    target_compile_definitions(ng_shared PRIVATE MU_DEBUG_ENABLED)
endif()

if(MU_DEBUG_BUILD)
    add_library(ng_ipc_debug STATIC shared/protocol/ipc_debug.cpp)
    set_target_properties(ng_ipc_debug PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ng_ipc_debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ng_ipc_debug PUBLIC ng_shared)
    target_compile_definitions(ng_ipc_debug PRIVATE MU_DEBUG_ENABLED)
endif()

add_subdirectory(worker)

add_library(ng_cli_layout STATIC tools/cli/layout_config.cpp)
set_target_properties(ng_cli_layout PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ng_cli_layout PUBLIC Qt6::Core ng_shared)
if(BUILD_OKULAR_GENERATOR AND KF6Config_VERSION VERSION_LESS "6.29.0")
    # KConfig 6.29 moved user globals after cascaded application defaults.
    target_compile_definitions(ng_cli_layout PUBLIC MU_KCONFIG_LEGACY_PRECEDENCE)
endif()
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(ng_cli_layout PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()

# mupdfng-cli: thin command-line adapter over the plugin worker bridge
# (WorkerClient). Only WorkerClient crosses the process boundary here.
add_executable(mupdfng-cli
    tools/cli/main.cpp
    tools/cli/cli_args.cpp
)
target_include_directories(mupdfng-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mupdfng-cli PRIVATE
    ng_plugin_worker
    ng_plugin_util
    ng_cli_layout
    Qt6::Core
)
target_compile_definitions(mupdfng-cli PRIVATE
    TESSDATA_DIR="${TESSDATA_DIR}"
)
if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdfng-cli PRIVATE MU_DEBUG_ENABLED)
endif()
install(TARGETS mupdfng-cli
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# In-process host libraries: consumed by the worker bridge AND directly by the
# generator for local work (NSS, caches, helpers). Only WorkerClient crosses
# the process boundary; nothing here may depend on generator/Okular types.
add_library(ng_plugin_cache STATIC
  plugin/caching/cache_file.cpp
  plugin/caching/epub_cache.cpp
  plugin/caching/pdf_toc_cache.cpp
  plugin/caching/ocr_cache.cpp
  plugin/caching/vacuum.cpp
)
target_include_directories(ng_plugin_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(ng_plugin_cache PUBLIC Qt6::Core ng_shared ZLIB::ZLIB)

add_library(ng_plugin_crypto STATIC
  plugin/crypto/nss.cpp
  plugin/crypto/nss_state.cpp
  plugin/crypto/nss_signing.cpp
  plugin/crypto/certificate_info.cpp
  plugin/crypto/certificate_database.cpp
)
target_include_directories(ng_plugin_crypto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NSS_INCLUDE_DIRS}
)
target_link_libraries(ng_plugin_crypto PUBLIC Qt6::Core ng_shared ${NSS_LIBRARIES})

add_library(ng_plugin_worker STATIC
  plugin/worker_client.cpp
  plugin/worker_transport.cpp
)
target_include_directories(ng_plugin_worker PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(ng_plugin_worker PUBLIC
    Qt6::Core
    Qt6::Gui
    ng_plugin_cache
    ng_plugin_crypto
    ng_shared
)
target_link_libraries(ng_plugin_worker PRIVATE ng_plugin_util)
if(MU_DEBUG_BUILD)
    target_link_libraries(ng_plugin_worker PUBLIC ng_ipc_debug)
endif()
target_compile_definitions(ng_plugin_worker PRIVATE
    WORKER_INSTALL_PATH="${WORKER_INSTALL_PATH}"
)
if(ENABLE_BUILD_TREE_WORKER)
    target_compile_definitions(ng_plugin_worker PRIVATE
        WORKER_BUILD_PATH="$<TARGET_FILE:mupdfng-worker>"
    )
endif()

add_library(ng_plugin_util STATIC
  plugin/util/document_type.cpp
  plugin/util/ocr_options.cpp
  plugin/util/process_memory.cpp
  plugin/util/signature_image.cpp
  plugin/util/stamp_image.cpp
  plugin/util/render_image.cpp
  plugin/util/signing_timestamp.cpp
  plugin/xfdf/export.cpp
  plugin/xfdf/import.cpp)
target_include_directories(ng_plugin_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ng_plugin_util PUBLIC Qt6::Core Qt6::Gui ng_shared)

set_target_properties(ng_plugin_cache ng_plugin_crypto ng_plugin_worker ng_plugin_util
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MU_DEBUG_BUILD)
    foreach(target IN ITEMS ng_plugin_cache ng_plugin_crypto ng_plugin_worker ng_plugin_util)
        target_compile_definitions(${target} PRIVATE MU_DEBUG_ENABLED)
    endforeach()
endif()

if(NOT BUILD_OKULAR_GENERATOR)
    return()
endif()

add_library(ng_plugin_ocr STATIC plugin/ocr/controller.cpp)
target_include_directories(ng_plugin_ocr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ng_plugin_ocr PUBLIC Qt6::Concurrent ng_plugin_cache ng_plugin_worker)

add_subdirectory(generator/config)

# Probe APIs independently: development builds can share a version number.
find_path(OKULAR_CORE_INCLUDE_DIR okularcore_export.h PATH_SUFFIXES okular/core REQUIRED)
find_path(OKULAR_INCLUDE_DIR interfaces/printinterface.h PATH_SUFFIXES okular REQUIRED)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Okular::Core Qt6::Xml)
set(CMAKE_REQUIRED_INCLUDES ${OKULAR_INCLUDE_DIR} ${OKULAR_CORE_INCLUDE_DIR})
check_cxx_source_compiles("#include <okular/core/document.h>
int main() { return sizeof(Okular::DocumentSynopsis::ElementBuilder) == 0; }"
    MU_OKULAR_STRUCTURED_SYNOPSIS)
check_cxx_source_compiles("#include <okular/interfaces/printinterface.h>
int main() { return sizeof(Okular::PrintOptionsWidgetInterface) == 0; }"
    MU_OKULAR_PRINT_OPTIONS_INTERFACE)
check_cxx_source_compiles("#include <okular/core/page.h>
#include <QColor>
int main() {
    Okular::Page page(0, 10, 10, Okular::Rotation0);
    page.setPlaceholderColor(QColor(Qt::black));
    return page.placeholderColor().isValid() ? 0 : 1;
}"
    MU_OKULAR_PAGE_PLACEHOLDER_COLOR)
cmake_pop_check_state()
configure_file(generator/okular_compat.hpp.in ${MU_GENERATED_INCLUDE_DIR}/generator/okular_compat.hpp)

add_library(ng_generator_page_error STATIC generator/page/error.cpp)
target_compile_definitions(ng_generator_page_error PRIVATE
    MUPDFNG_LOGO_PATH="${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/scalable/apps/mupdfng.svg")
target_include_directories(ng_generator_page_error PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ng_generator_page_error PUBLIC Okular::Core Qt6::Core KF6::I18n)

add_library(ng_generator_adapters STATIC
  generator/ui/link_preview.cpp
  generator/conversion/annotation.cpp
  generator/conversion/certificate.cpp
  generator/conversion/document.cpp
  generator/conversion/signing.cpp
  generator/conversion/text.cpp
  generator/conversion/xfdf.cpp
  generator/proxy/annotation.cpp
  generator/proxy/preview_goto_action.cpp
  generator/proxy/form/text.cpp
  generator/proxy/form/checkbox.cpp
  generator/proxy/form/radio_button.cpp
  generator/proxy/form/push_button.cpp
  generator/proxy/form/choice.cpp
  generator/proxy/form/coordinator.cpp
  generator/proxy/form/construction.cpp
  generator/proxy/certificate_store.cpp
  generator/ui/printing.cpp
  generator/ui/layers_model.cpp
)
target_include_directories(ng_generator_adapters PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
    ${OKULAR_INCLUDE_DIR}
    ${OKULAR_CORE_INCLUDE_DIR}
)
target_link_libraries(ng_generator_adapters PUBLIC
    Okular::Core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Xml
    KF6::I18n
    ng_generator_settings
    ng_plugin_crypto
    ng_plugin_util
)
target_compile_definitions(ng_generator_adapters PRIVATE
    TRANSLATION_DOMAIN="okular_mupdfng"
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING
)

add_library(ng_generator_signature STATIC generator/proxy/form/signature.cpp)
target_include_directories(ng_generator_signature PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(ng_generator_signature PUBLIC
    Okular::Core
    Qt6::Core
    Qt6::Gui
    ng_generator_settings
    ng_plugin_crypto
    ng_plugin_util
    ng_plugin_worker
    ng_generator_adapters
)
target_compile_definitions(ng_generator_signature PRIVATE
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING
)
set_target_properties(
    ng_plugin_ocr
    ng_generator_page_error
    ng_generator_adapters
    ng_generator_signature
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

if(MU_DEBUG_BUILD)
    target_compile_definitions(ng_plugin_ocr PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(ng_generator_page_error PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(ng_generator_adapters PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(ng_generator_signature PRIVATE MU_DEBUG_ENABLED)
endif()

set(okularGenerator_mupdfng_SRCS
  generator/mupdfng.cpp
  generator/export.cpp
  generator/ui/actions.cpp
  generator/ui/document_settings.cpp
  generator/ui/xfdf_import.cpp
  generator/ui/window_menus.cpp
  generator/config/settingswidget.cpp
  generator/config/csseditor/editor.cpp
  generator/config/csseditor/highlighter.cpp
  generator/config/certmanager/manager_dialog.cpp
  generator/config/certmanager/self_signed_dialog.cpp
)

if(NOT MU_OKULAR_PAGE_PLACEHOLDER_COLOR)
    list(APPEND okularGenerator_mupdfng_SRCS generator/page/placeholder.cpp)
endif()

ki18n_wrap_ui(okularGenerator_mupdfng_SRCS
   generator/config/settingswidget.ui
)

set(mupdf_generator_settings_SRCS generator/config/document_settings.cpp generator/config/settings.cpp generator/config/signature_preview.cpp)
kconfig_add_kcfg_files(mupdf_generator_settings_SRCS
   GENERATE_MOC generator/config/mupdfngsettings.kcfgc
)

add_library(ng_generator_settings STATIC ${mupdf_generator_settings_SRCS})
set_target_properties(ng_generator_settings PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MU_DEBUG_BUILD)
    target_compile_definitions(ng_generator_settings PRIVATE MU_DEBUG_ENABLED)
endif()
target_compile_definitions(ng_generator_settings PRIVATE
    TESSDATA_DIR="${TESSDATA_DIR}"
    SIGNATURE_FONT_DIR="${SIGNATURE_FONT_DIR}"
)
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(ng_generator_settings PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()
target_include_directories(ng_generator_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(ng_generator_settings PUBLIC Qt6::Core KF6::ConfigWidgets ng_plugin_cache ng_plugin_util)

add_custom_target(ng_generator_settings_codegen
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mupdfngsettings.cpp
)
add_dependencies(ng_generator_settings ng_generator_settings_codegen)

kcoreaddons_add_plugin(okularGenerator_mupdfng
    INSTALL_NAMESPACE "okular_generators"
    SOURCES ${okularGenerator_mupdfng_SRCS}
)
qt_add_resources(okularGenerator_mupdfng "mupdfng_xmlgui"
    PREFIX "/kxmlgui5/okular_mupdfng"
    BASE "generator/ui"
    FILES generator/ui/mupdfngui.rc)

if(MU_DEBUG_BUILD)
    target_compile_definitions(okularGenerator_mupdfng PRIVATE MU_DEBUG_ENABLED)
endif()

target_include_directories(okularGenerator_mupdfng PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${NSS_INCLUDE_DIRS}
    ${OKULAR_CORE_INCLUDE_DIR}
)

target_compile_definitions(okularGenerator_mupdfng PRIVATE TESSDATA_DIR="${TESSDATA_DIR}")
if(ENABLE_FORM_JAVASCRIPT)
    target_compile_definitions(okularGenerator_mupdfng PRIVATE MU_WORKER_ENABLE_FORM_JAVASCRIPT)
endif()

target_link_libraries(okularGenerator_mupdfng
    ng_generator_adapters
    ng_generator_signature
    ng_generator_settings
    ng_generator_page_error
    ng_plugin_cache
    ng_plugin_crypto
    ng_plugin_ocr
    ng_plugin_util
    ng_plugin_worker
    ng_shared
    Okular::Core
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::KIOWidgets
    KF6::XmlGui
    KF6::WidgetsAddons
    Qt6::PrintSupport
    Qt6::Concurrent
    Qt6::Network
    Qt6::Xml
    ZLIB::ZLIB
    ${NSS_LIBRARIES}
)
target_compile_definitions(okularGenerator_mupdfng PRIVATE
    TRANSLATION_DOMAIN="okular_mupdfng"
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING)
