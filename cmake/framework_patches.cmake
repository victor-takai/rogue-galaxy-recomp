# Local changes to the pinned framework, carried as patches so the submodule
# stays at its upstream commit: each is applied to the checkout once and
# skipped when it already is. See patches/README.md.
#
# Included by the top-level CMakeLists.txt when the game is configured, and
# run on its own before the recompiler is built (`python setup.py tools`,
# which runs `cmake -P cmake/framework_patches.cmake`): some patches change
# the recompiler, and a recompiler built before them generates code
# without them (a fresh clone built in the README's old order did, 2026-10-26).

if(CMAKE_SCRIPT_MODE_FILE)
    # On its own, the policies of today (IN_LIST below); included, the
    # project's.
    cmake_minimum_required(VERSION 3.24)
endif()
get_filename_component(rg_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE git)
endif()
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "Git is needed to apply patches/ps2recomp to thirdparty/PS2Recomp")
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB rg_framework_patches "${rg_root}/patches/ps2recomp/*.patch")
else()
    # A new patch reconfigures the game.
    file(GLOB rg_framework_patches CONFIGURE_DEPENDS "${rg_root}/patches/ps2recomp/*.patch")
endif()
list(SORT rg_framework_patches)
# The patches applied so far are listed in a stamp inside the checkout: a
# later patch may change lines an earlier one added, after which the earlier
# one no longer reverse-applies, so that check alone cannot tell "applied"
# from "not yet" (it failed configure twice). Undoing the patches
# (`git -C thirdparty/PS2Recomp checkout -- .`) should remove the stamp too;
# the check below notices when it did not.
set(rg_patch_stamp "${rg_root}/thirdparty/PS2Recomp/.rg-patches-applied")
set(rg_patches_applied "")
if(EXISTS "${rg_patch_stamp}")
    file(STRINGS "${rg_patch_stamp}" rg_patches_applied)
endif()
# The stamp is no tracked file, so a checkout reset without deleting it
# would skip every patch, and the recompiler would be built without them.
# The first patch applies to the pinned commit as it is, and not once it
# is in: when it applies again, the checkout lost the patches. (The last
# one cannot tell: it applies only over the others, 2026-10-27.) The files
# the patches create are no checkout's files either, and outlast such a
# reset (a release archive unpacked over the folder, 2026-11-14): left
# there, the patch that creates one fails ("already exists in working
# directory"), so they go too.
set(rg_patch_first "")
if(rg_patches_applied)
    list(GET rg_patches_applied 0 rg_patch_first)
    set(rg_patch_first "${rg_root}/patches/ps2recomp/${rg_patch_first}")
endif()
if(NOT EXISTS "${rg_patch_first}" AND rg_framework_patches)
    list(GET rg_framework_patches 0 rg_patch_first)
endif()
if(EXISTS "${rg_patch_first}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${rg_patch_first}"
        WORKING_DIRECTORY "${rg_root}/thirdparty/PS2Recomp"
        RESULT_VARIABLE rg_patch_out OUTPUT_QUIET ERROR_QUIET)
    if(rg_patch_out EQUAL 0)
        if(rg_patches_applied)
            get_filename_component(rg_patch_first "${rg_patch_first}" NAME)
            message(STATUS "thirdparty/PS2Recomp lost its patches (${rg_patch_first} applies again); applying them all")
            file(REMOVE "${rg_patch_stamp}")
            set(rg_patches_applied "")
        endif()
        foreach(patch IN LISTS rg_framework_patches)
            file(STRINGS "${patch}" rg_patch_lines REGEX "^(--- /dev/null|\\+\\+\\+ b/)")
            set(rg_patch_new FALSE)
            foreach(rg_patch_line IN LISTS rg_patch_lines)
                if(rg_patch_line MATCHES "^--- /dev/null")
                    set(rg_patch_new TRUE)
                elseif(rg_patch_new)
                    string(SUBSTRING "${rg_patch_line}" 6 -1 rg_patch_created)
                    string(STRIP "${rg_patch_created}" rg_patch_created)
                    if(EXISTS "${rg_root}/thirdparty/PS2Recomp/${rg_patch_created}")
                        message(STATUS "Removing thirdparty/PS2Recomp/${rg_patch_created}, which a patch creates")
                        file(REMOVE "${rg_root}/thirdparty/PS2Recomp/${rg_patch_created}")
                    endif()
                    set(rg_patch_new FALSE)
                endif()
            endforeach()
        endforeach()
    endif()
endif()
foreach(patch IN LISTS rg_framework_patches)
    get_filename_component(rg_patch_name "${patch}" NAME)
    if(rg_patch_name IN_LIST rg_patches_applied)
        continue()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${rg_root}/thirdparty/PS2Recomp"
        RESULT_VARIABLE rg_patch_present OUTPUT_QUIET ERROR_QUIET)
    if(NOT rg_patch_present EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${rg_root}/thirdparty/PS2Recomp"
            RESULT_VARIABLE rg_patch_result)
        if(NOT rg_patch_result EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch} to thirdparty/PS2Recomp")
        endif()
        message(STATUS "Applied ${patch}")
    endif()
    file(APPEND "${rg_patch_stamp}" "${rg_patch_name}\n")
endforeach()
