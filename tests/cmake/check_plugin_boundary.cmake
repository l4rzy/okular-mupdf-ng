# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED PLUGIN_SOURCE_DIR OR NOT IS_DIRECTORY "${PLUGIN_SOURCE_DIR}")
    message(FATAL_ERROR "Plugin source directory is unavailable: ${PLUGIN_SOURCE_DIR}")
endif()

file(GLOB_RECURSE plugin_sources
    "${PLUGIN_SOURCE_DIR}/*.cpp"
    "${PLUGIN_SOURCE_DIR}/*.hpp")

set(violations)
foreach(source IN LISTS plugin_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#[ \\t]*include[ \\t]*[<\"]okular/"
       OR contents MATCHES "#[ \\t]*include[ \\t]*[<\"]generator/"
       OR contents MATCHES "Okular::")
        list(APPEND violations "${source}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n" report)
    message(FATAL_ERROR "Plugin code must not depend on generator or Okular types:\n${report}")
endif()
