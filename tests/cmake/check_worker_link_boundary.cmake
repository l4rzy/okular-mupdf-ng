if(NOT DEFINED WORKER_PATH OR NOT EXISTS "${WORKER_PATH}")
    message(FATAL_ERROR "okular-mupdf-worker executable was not built: ${WORKER_PATH}")
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

# The control plane is native Unix sockets. Qt Network must not re-enter the
# worker dependency graph; GUI, Okular, and KDE libraries are also forbidden.
string(REGEX MATCH "(libQt[56](Gui|Network)|libOkular|libKF[0-9])" forbidden "${dependencies}")
if(forbidden)
    message(FATAL_ERROR "okular-mupdf-worker links a forbidden dependency: ${forbidden}\n${dependencies}")
endif()
