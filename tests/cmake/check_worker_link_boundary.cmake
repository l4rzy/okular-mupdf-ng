# SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED WORKER_PATH OR NOT EXISTS "${WORKER_PATH}")
    message(FATAL_ERROR "mupdfng-worker executable was not built: ${WORKER_PATH}")
endif()

find_program(LDD_EXECUTABLE ldd REQUIRED)
execute_process(
    COMMAND "${LDD_EXECUTABLE}" "${WORKER_PATH}"
    RESULT_VARIABLE ldd_status
    OUTPUT_VARIABLE dependencies
    ERROR_VARIABLE ldd_error)
if(NOT ldd_status EQUAL 0)
    message(FATAL_ERROR "Unable to inspect worker dependencies: ${ldd_error}")
endif()

# The worker must remain free of all Qt, Okular, and KDE libraries.
if(dependencies MATCHES "(libQt[0-9]|libOkular|libKF[0-9])")
    message(FATAL_ERROR "mupdfng-worker links a forbidden dependency:\n${dependencies}")
endif()

# Check literal includes, as in the plugin and CLI boundary tests.
foreach(source_dir IN ITEMS "${WORKER_SOURCE_DIR}" "${SHARED_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${source_dir}")
        message(FATAL_ERROR "Worker boundary source directory is unavailable: ${source_dir}")
    endif()
    file(GLOB_RECURSE sources "${source_dir}/*.cpp" "${source_dir}/*.hpp" "${source_dir}/*.h" "${source_dir}/*.hpp.in")
    foreach(source IN LISTS sources)
        file(READ "${source}" contents)
        if(contents MATCHES "#[ \t]*include[ \t]*[<\"](Qt|Q[A-Za-z_]|q[a-z0-9_]+\\.h|K[A-Z]|(generator|plugin|okular)/)")
            message(FATAL_ERROR "Worker boundary source includes a forbidden header: ${source}")
        endif()
    endforeach()
endforeach()
