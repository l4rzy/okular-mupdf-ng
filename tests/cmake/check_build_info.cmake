# SPDX-License-Identifier: GPL-3.0-or-later

set(repository "${TEST_ROOT}/repository")
set(header "${TEST_ROOT}/build_info.hpp")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${repository}/src/shared")
file(COPY "${PROJECT_SOURCE_DIR}/src/shared/build_info.hpp.in" DESTINATION "${repository}/src/shared")

function(runGit)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${repository}"
            -c user.name=Test -c user.email=test@example.invalid
            -c commit.gpgsign=false -c tag.gpgsign=false -c core.hooksPath= ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 10)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGV}: ${error}")
    endif()
    set(git_output "${output}" PARENT_SCOPE)
endfunction()

function(checkBuildInfo source expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DMU_SOURCE_DIR=${source}" "-DMU_BUILD_INFO_FILE=${header}" ${ARGN}
            -P "${PROJECT_SOURCE_DIR}/cmake/build_info.cmake"
        RESULT_VARIABLE result TIMEOUT 20)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Build information generation failed")
    endif()
    file(READ "${header}" content)
    string(FIND "${content}" "GIT_BUILD = \"${expected}\";" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Expected Git build '${expected}', got:\n${content}")
    endif()
endfunction()

runGit(init -q)
checkBuildInfo("${repository}" "")
# Signing and hooks belong to the developer's workflow, not fixture setup.
runGit(config commit.gpgsign true)
runGit(config tag.gpgsign true)
runGit(add src/shared/build_info.hpp.in)
runGit(commit -q -m Initial)
runGit(rev-parse --short=7 HEAD)
set(hash "${git_output}")
checkBuildInfo("${repository}" "${hash}")

file(WRITE "${repository}/.git/info/exclude" "ignored.txt\n")
file(WRITE "${repository}/ignored.txt" "ignored output")
checkBuildInfo("${repository}" "${hash}")

# Both unstaged and staged tracked edits must update an existing header.
file(APPEND "${repository}/src/shared/build_info.hpp.in" "\n")
checkBuildInfo("${repository}" "${hash} - dirty")
runGit(add src/shared/build_info.hpp.in)
checkBuildInfo("${repository}" "${hash} - dirty")
runGit(commit -q -m Update)
runGit(rev-parse --short=7 HEAD)
set(hash "${git_output}")
checkBuildInfo("${repository}" "${hash}")

runGit(tag v1)
checkBuildInfo("${repository}" "")
file(WRITE "${repository}/untracked.txt" "local edit")
checkBuildInfo("${repository}" "${hash} - dirty")
file(REMOVE "${repository}/untracked.txt")
runGit(tag -d v1)
runGit(tag -a v2 -m Release)
checkBuildInfo("${repository}" "")
runGit(checkout --detach -q HEAD)
checkBuildInfo("${repository}" "")
runGit(tag -d v2)
checkBuildInfo("${repository}" "${hash}")

runGit(worktree add --detach -q "${TEST_ROOT}/worktree with spaces" HEAD)
checkBuildInfo("${TEST_ROOT}/worktree with spaces" "${hash}")
checkBuildInfo("${repository}" "" "-DMU_GIT_EXECUTABLE=")
checkBuildInfo("${repository}" "" "-DMU_GIT_EXECUTABLE=${TEST_ROOT}/missing-git")

# An archive nested inside another repository must not inherit its revision.
file(MAKE_DIRECTORY "${repository}/archive/src/shared")
file(COPY "${PROJECT_SOURCE_DIR}/src/shared/build_info.hpp.in" DESTINATION "${repository}/archive/src/shared")
checkBuildInfo("${repository}/archive" "")
