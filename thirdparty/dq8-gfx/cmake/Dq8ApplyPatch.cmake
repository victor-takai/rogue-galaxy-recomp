# cmake -DPATCH=<file> -P Dq8ApplyPatch.cmake, in a git checkout: applies the
# patch unless it is already in (FetchContent's patch step can run again on
# a tree it patched before).
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH}"
                RESULT_VARIABLE ALREADY OUTPUT_QUIET ERROR_QUIET)
if(ALREADY EQUAL 0)
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${PATCH}" RESULT_VARIABLE FAILED)
if(NOT FAILED EQUAL 0)
    message(FATAL_ERROR "could not apply ${PATCH}")
endif()
