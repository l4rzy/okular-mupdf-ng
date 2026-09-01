# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

add_subdirectory(generator/config)

add_library(mupdf_transport STATIC
  shared/transport/ctrl_channel.cpp
  shared/transport/fd_channel.cpp
  shared/transport/poll.cpp
)
set_target_properties(mupdf_transport PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(mupdf_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

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
  generator/generator_mupdf.cpp
  generator/proxy/certificate_store.cpp
  generator/config/mupdfsettingswidget.cpp
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
  generator/signature_image.cpp
  shared/logging.cpp
  shared/model/validation.cpp
  shared/protocol/zpp_codec.cpp
  shared/model/validation.hpp
  generator/placeholder.hpp
  generator/signature_image.hpp
  shared/protocol/zpp_codec.hpp
  generator/proxy/annotation.hpp
  generator/conversion/annotation.hpp
  generator/conversion/certificate.hpp
  generator/conversion/document.hpp
  generator/conversion/text.hpp
  plugin/worker_client.hpp
  plugin/worker_transport.hpp
  plugin/caching/cache_file.hpp
  plugin/caching/epub_cache.hpp
  plugin/caching/ocr_cache.hpp
  plugin/caching/ocr_constants.hpp
  plugin/crypto/nss.hpp
  plugin/crypto/certificate_info_internal.hpp
  plugin/crypto/certificate_database.hpp
  plugin/crypto/nss_handles.hpp
  plugin/crypto/nss_internal.hpp
  plugin/crypto/nss_error_internal.hpp
  generator/proxy/form/signature.hpp
  plugin/ocr/constants.hpp
  plugin/ocr/config.hpp
  plugin/ocr/controller.hpp
  plugin/ocr/controller.cpp
  plugin/ocr/scheduler.hpp
  plugin/ocr/ocr.hpp
  plugin/util/temp_dir.hpp
  shared/protocol/ipc_debug.hpp
  generator/generator_mupdf.hpp
  generator/config/settings.hpp
  generator/proxy/certificate_store.hpp
  generator/proxy/embedded_file.hpp
  generator/config/mupdfsettingswidget.hpp
  generator/config/csseditor/editor.hpp
  generator/config/csseditor/highlighter.hpp
  generator/config/certmanager/dialog_utils.hpp
  generator/config/certmanager/manager_dialog.hpp
  generator/config/certmanager/self_signed_dialog.hpp
)

ki18n_wrap_ui(okularGenerator_mupdfng_SRCS
   generator/config/mupdfsettingswidget.ui
)

kconfig_add_kcfg_files(okularGenerator_mupdfng_SRCS
   GENERATE_MOC generator/config/mupdfsettings.kcfgc
)

add_custom_target(mupdf_generator_settings_codegen
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mupdfsettings.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${MUPDF_GENERATED_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${NSS_INCLUDE_DIRS}
)
target_include_directories(okularGenerator_mupdfng SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty
)

target_compile_definitions(okularGenerator_mupdfng PRIVATE TESSDATA_DIR="${TESSDATA_DIR}")
if(MUPDF_HAS_OCR)
    target_compile_definitions(okularGenerator_mupdfng PRIVATE MUPDF_HAS_OCR)
endif()

target_link_libraries(okularGenerator_mupdfng
    mupdf_transport
    Okular::Core
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::KIOWidgets
    Qt6::PrintSupport
    Qt6::Concurrent
    Qt6::Network
    ZLIB::ZLIB
    MuPDF::Engine
    ${NSS_LIBRARIES}
)
target_compile_definitions(okularGenerator_mupdfng PRIVATE
    TRANSLATION_DOMAIN="okular_mupdfng"
    HAVE_NEW_SIGNATURE_API=1
    MUPDF_FORMFIELD_REMOTE_SIGNING)
