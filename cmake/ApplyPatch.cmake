# Applies a patch file to the current directory (a FetchContent source tree).
#
#   cmake -DPATCH_FILE=<file> -DPATCH_SHA256=<sha> -P ApplyPatch.cmake
#
# The FetchContent source tree lives inside this repository's work tree, so a
# plain `git apply` would resolve paths against the outer repository and skip
# every hunk. GIT_CEILING_DIRECTORIES stops repository discovery at the source
# tree, which makes git apply the patch relative to the current directory.
# PATCH_SHA256 is part of the command line so editing the patch re-runs it.

if(NOT PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "ApplyPatch: PATCH_FILE '${PATCH_FILE}' not found")
endif()
find_program(_se_git git REQUIRED)
get_filename_component(_se_parent "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
set(ENV{GIT_CEILING_DIRECTORIES} "${_se_parent}")

set(_se_stamp "${CMAKE_CURRENT_SOURCE_DIR}/.se-patch-applied")
if(EXISTS "${_se_stamp}")
  file(READ "${_se_stamp}" _se_applied)
  if(_se_applied STREQUAL "${PATCH_SHA256}")
    message(STATUS "ApplyPatch: already applied (${PATCH_SHA256})")
    return()
  endif()
  message(FATAL_ERROR "ApplyPatch: a different patch was applied to this "
                      "tree; delete the FetchContent source directory.")
endif()

execute_process(
  COMMAND "${_se_git}" apply --check --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE _se_rc ERROR_VARIABLE _se_err)
if(NOT _se_rc EQUAL 0)
  message(FATAL_ERROR "ApplyPatch: patch does not apply:\n${_se_err}")
endif()
execute_process(
  COMMAND "${_se_git}" apply --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE _se_rc ERROR_VARIABLE _se_err)
if(NOT _se_rc EQUAL 0)
  message(FATAL_ERROR "ApplyPatch: git apply failed:\n${_se_err}")
endif()
file(WRITE "${_se_stamp}" "${PATCH_SHA256}")
message(STATUS "ApplyPatch: applied ${PATCH_FILE}")
