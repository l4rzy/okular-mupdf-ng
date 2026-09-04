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

add_subdirectory(worker)

set(okularGenerator_mupdfng_SRCS
  generator/conversion/annotation.cpp
  generator/conversion/certificate.cpp
  generator/conversion/document.cpp
  generator/conversion/text.cpp
  generator/proxy/annotation.cpp
  generator/proxy/form/signature.cpp
  generator/proxy/form/text.cpp
  generator/proxy/form/checkbox.cpp
  generator/proxy/form/radio_button.cpp
  generator/proxy/form/push_button.cpp
  generator/proxy/form/choice.cpp
  generator/proxy/form/coordinator.cpp
  plugin/crypto/nss.cpp
  plugin/crypto/nss_state.cpp
  plugin/crypto/nss_signing.cpp
  plugin/crypto/certificate_info.cpp
  generator/config/settings.cpp
  generator/mupdfng.cpp
  generator/printing.cpp
  generator/proxy/certificate_store.cpp
  generator/config/settingswidget.cpp
  generator/config/csseditor/editor.cpp
  generator/config/csseditor/highlighter.cpp
  plugin/crypto/certificate_database.cpp
  generator/config/certmanager/manager_dialog.cpp
  generator/config/certmanager/self_signed_dialog.cpp
  plugin/worker_client.cpp
  plugin/worker_transport.cpp
  plugin/caching/cache_file.cpp
  plugin/caching/epub_cache.cpp
  plugin/caching/ocr_cache.cpp
  generator/placeholder.cpp
  plugin/ocr/controller.cpp
  plugin/util/signature_image.cpp
)

ki18n_wrap_ui(okularGenerator_mupdfng_SRCS
   generator/config/settingswidget.ui
)

kconfig_add_kcfg_files(okularGenerator_mupdfng_SRCS
   GENERATE_MOC generator/config/mupdfngsettings.kcfgc
)

add_custom_target(mupdf_generator_settings_codegen
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mupdfngsettings.cpp
)

kcoreaddons_add_plugin(okularGenerator_mupdfng
    INSTALL_NAMESPACE "okular_generators"
    SOURCES ${okularGenerator_mupdfng_SRCS}
)

if(MU_DEBUG_BUILD)
    target_compile_definitions(okularGenerator_mupdfng PRIVATE MU_DEBUG_ENABLED)
    target_sources(okularGenerator_mupdfng PRIVATE shared/protocol/ipc_debug.cpp)
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

# Embed worker binary paths so the plugin can locate it without install.
# WORKER_BUILD_PATH is exported by src/worker (CACHE INTERNAL).
target_compile_definitions(okularGenerator_mupdfng PRIVATE
    "$<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>>:WORKER_BUILD_PATH=\"$<TARGET_FILE:okular-mupdf-worker>\">"
    WORKER_INSTALL_PATH="${WORKER_INSTALL_PATH}"
)
