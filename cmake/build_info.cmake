# SPDX-License-Identifier: GPL-3.0-or-later

set(MU_GIT_BUILD "")
find_program(MU_GIT_EXECUTABLE git)
# Do not pick up an unrelated parent repository around a source archive.
# Worktrees have a .git file rather than a directory.
if(MU_GIT_EXECUTABLE AND EXISTS "${MU_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${MU_GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${MU_SOURCE_DIR}"
        RESULT_VARIABLE hash_result OUTPUT_VARIABLE git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5)
    execute_process(
        COMMAND "${MU_GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${MU_SOURCE_DIR}"
        RESULT_VARIABLE status_result OUTPUT_VARIABLE git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5)
    execute_process(
        COMMAND "${MU_GIT_EXECUTABLE}" describe --exact-match --tags HEAD
        WORKING_DIRECTORY "${MU_SOURCE_DIR}"
        RESULT_VARIABLE tag_result OUTPUT_QUIET ERROR_QUIET TIMEOUT 5)
    if(hash_result EQUAL 0 AND status_result EQUAL 0)
        if(NOT git_status STREQUAL "")
            set(MU_GIT_BUILD "${git_hash} - dirty")
        elseif(NOT tag_result EQUAL 0)
            set(MU_GIT_BUILD "${git_hash}")
        endif()
    endif()
endif()

configure_file("${MU_SOURCE_DIR}/src/shared/build_info.hpp.in" "${MU_BUILD_INFO_FILE}" @ONLY)
