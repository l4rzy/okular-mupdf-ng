# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later
find_program(LDD_EXECUTABLE ldd REQUIRED)
execute_process(COMMAND "${LDD_EXECUTABLE}" "${CLI_PATH}"
    RESULT_VARIABLE status OUTPUT_VARIABLE dependencies ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot inspect CLI dependencies: ${error}")
endif()
if(dependencies MATCHES "(libKF[0-9]|libOkular)")
    message(FATAL_ERROR "CLI must not link KDE or Okular libraries:\n${dependencies}")
endif()
file(GLOB cli_sources "${CLI_SOURCE_DIR}/*.cpp" "${CLI_SOURCE_DIR}/*.hpp")
foreach(source IN LISTS cli_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#[ \t]*include[ \t]*[<\"](generator/|KConfig|KSharedConfig|okular/)")
        message(FATAL_ERROR "CLI source depends on generator/KDE/Okular: ${source}")
    endif()
endforeach()
