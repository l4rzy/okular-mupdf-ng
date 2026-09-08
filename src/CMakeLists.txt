# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

add_subdirectory(generator/config)

# Qt-free code shared by the plugin and the sandboxed worker: transport
# channels, logging, model validation, and the zpp serialization codec.
add_library(mupdf_shared STATIC
  shared/transport/ctrl_channel.cpp
  shared/transport/fd_channel.cpp
  shared/transport/poll.cpp
  shared/logging.cpp
  shared/model/validation.cpp
  shared/protocol/zpp_codec.cpp
)
set_target_properties(mupdf_shared PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(mupdf_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(mupdf_shared SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty # zpp_bits.h for the codec
)
if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdf_shared PRIVATE MU_DEBUG_ENABLED)
endif()

if(MU_DEBUG_BUILD)
    add_library(mupdf_ipc_debug STATIC shared/protocol/ipc_debug.cpp)
    set_target_properties(mupdf_ipc_debug PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(mupdf_ipc_debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mupdf_ipc_debug PUBLIC mupdf_shared)
    target_compile_definitions(mupdf_ipc_debug PRIVATE MU_DEBUG_ENABLED)
endif()

add_subdirectory(worker)

add_library(mupdf_plugin_cache STATIC
  plugin/caching/cache_file.cpp
  plugin/caching/epub_cache.cpp
  plugin/caching/ocr_cache.cpp
)
target_include_directories(mupdf_plugin_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mupdf_plugin_cache PUBLIC Qt6::Core mupdf_shared ZLIB::ZLIB)

add_library(mupdf_plugin_crypto STATIC
  plugin/crypto/nss.cpp
  plugin/crypto/nss_state.cpp
  plugin/crypto/nss_signing.cpp
  plugin/crypto/certificate_info.cpp
  plugin/crypto/certificate_database.cpp
)
target_include_directories(mupdf_plugin_crypto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NSS_INCLUDE_DIRS}
)
target_link_libraries(mupdf_plugin_crypto PUBLIC Qt6::Core mupdf_shared ${NSS_LIBRARIES})

add_library(mupdf_plugin_worker STATIC
  plugin/worker_client.cpp
  plugin/worker_transport.cpp
)
target_include_directories(mupdf_plugin_worker PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(mupdf_plugin_worker PUBLIC
    Qt6::Core
    Qt6::Gui
    mupdf_plugin_cache
    mupdf_plugin_crypto
    mupdf_shared
)
if(MU_DEBUG_BUILD)
    target_link_libraries(mupdf_plugin_worker PUBLIC mupdf_ipc_debug)
endif()
target_compile_definitions(mupdf_plugin_worker PRIVATE
    WORKER_INSTALL_PATH="${WORKER_INSTALL_PATH}"
)
if(ENABLE_BUILD_TREE_WORKER_PATH)
    target_compile_definitions(mupdf_plugin_worker PRIVATE
        WORKER_BUILD_PATH="$<TARGET_FILE:okular-mupdf-worker>"
    )
endif()

add_library(mupdf_plugin_ocr STATIC plugin/ocr/controller.cpp)
target_include_directories(mupdf_plugin_ocr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mupdf_plugin_ocr PUBLIC Qt6::Concurrent mupdf_plugin_cache mupdf_plugin_worker)

add_library(mupdf_plugin_util STATIC plugin/util/signature_image.cpp plugin/util/render_image.cpp plugin/util/signing_timestamp.cpp)
target_include_directories(mupdf_plugin_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mupdf_plugin_util PUBLIC Qt6::Gui)

add_library(mupdf_generator_placeholder STATIC generator/placeholder.cpp)
target_include_directories(mupdf_generator_placeholder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mupdf_generator_placeholder PUBLIC Okular::Core Qt6::Core)

add_library(mupdf_generator_adapters STATIC
  generator/conversion/annotation.cpp
  generator/conversion/certificate.cpp
  generator/conversion/document.cpp
  generator/conversion/signing.cpp
  generator/conversion/text.cpp
  generator/proxy/annotation.cpp
  generator/proxy/form/text.cpp
  generator/proxy/form/checkbox.cpp
  generator/proxy/form/radio_button.cpp
  generator/proxy/form/push_button.cpp
  generator/proxy/form/choice.cpp
  generator/proxy/form/coordinator.cpp
  generator/proxy/certificate_store.cpp
  generator/printing.cpp
)
target_include_directories(mupdf_generator_adapters PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(mupdf_generator_adapters PUBLIC
    Okular::Core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Xml
    KF6::I18n
    MuPDF::Headers
    mupdf_plugin_crypto
    mupdf_plugin_util
)
target_compile_definitions(mupdf_generator_adapters PRIVATE
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING
)

add_library(mupdf_generator_signature STATIC generator/proxy/form/signature.cpp)
target_include_directories(mupdf_generator_signature PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
)
target_link_libraries(mupdf_generator_signature PUBLIC
    Okular::Core
    Qt6::Core
    Qt6::Gui
    MuPDF::Headers
    mupdf_plugin_crypto
    mupdf_plugin_util
    mupdf_plugin_worker
    mupdf_generator_adapters
)
target_compile_definitions(mupdf_generator_signature PRIVATE
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING
)
set_target_properties(
    mupdf_plugin_cache
    mupdf_plugin_crypto
    mupdf_plugin_worker
    mupdf_plugin_ocr
    mupdf_plugin_util
    mupdf_generator_placeholder
    mupdf_generator_adapters
    mupdf_generator_signature
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdf_plugin_cache PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_plugin_crypto PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_plugin_worker PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_plugin_ocr PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_plugin_util PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_generator_placeholder PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_generator_adapters PRIVATE MU_DEBUG_ENABLED)
    target_compile_definitions(mupdf_generator_signature PRIVATE MU_DEBUG_ENABLED)
endif()

set(okularGenerator_mupdfng_SRCS
  generator/mupdfng.cpp
  generator/render_tracker.cpp
  generator/config/settingswidget.cpp
  generator/config/csseditor/editor.cpp
  generator/config/csseditor/highlighter.cpp
  generator/config/certmanager/manager_dialog.cpp
  generator/config/certmanager/self_signed_dialog.cpp
)

ki18n_wrap_ui(okularGenerator_mupdfng_SRCS
   generator/config/settingswidget.ui
)

set(mupdf_generator_settings_SRCS generator/config/settings.cpp)
kconfig_add_kcfg_files(mupdf_generator_settings_SRCS
   GENERATE_MOC generator/config/mupdfngsettings.kcfgc
)

add_library(mupdf_generator_settings STATIC ${mupdf_generator_settings_SRCS})
set_target_properties(mupdf_generator_settings PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MU_DEBUG_BUILD)
    target_compile_definitions(mupdf_generator_settings PRIVATE MU_DEBUG_ENABLED)
endif()
target_include_directories(mupdf_generator_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mupdf_generator_settings PUBLIC Qt6::Core KF6::ConfigWidgets mupdf_plugin_cache)

add_custom_target(mupdf_generator_settings_codegen
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mupdfngsettings.cpp
)
add_dependencies(mupdf_generator_settings mupdf_generator_settings_codegen)

kcoreaddons_add_plugin(okularGenerator_mupdfng
    INSTALL_NAMESPACE "okular_generators"
    SOURCES ${okularGenerator_mupdfng_SRCS}
)

if(MU_DEBUG_BUILD)
    target_compile_definitions(okularGenerator_mupdfng PRIVATE MU_DEBUG_ENABLED)
endif()

target_include_directories(okularGenerator_mupdfng PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MU_GENERATED_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${NSS_INCLUDE_DIRS}
)

target_compile_definitions(okularGenerator_mupdfng PRIVATE TESSDATA_DIR="${TESSDATA_DIR}")
target_compile_definitions(okularGenerator_mupdfng PRIVATE MUPDF_HAS_OCR)

target_link_libraries(okularGenerator_mupdfng
    mupdf_generator_adapters
    mupdf_generator_signature
    mupdf_generator_settings
    mupdf_generator_placeholder
    mupdf_plugin_cache
    mupdf_plugin_crypto
    mupdf_plugin_ocr
    mupdf_plugin_util
    mupdf_plugin_worker
    mupdf_shared
    Okular::Core
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::KIOWidgets
    Qt6::PrintSupport
    Qt6::Concurrent
    Qt6::Network
    Qt6::Xml
    ZLIB::ZLIB
    MuPDF::Headers
    ${NSS_LIBRARIES}
)
target_compile_definitions(okularGenerator_mupdfng PRIVATE
    TRANSLATION_DOMAIN="okular_mupdfng"
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING)
