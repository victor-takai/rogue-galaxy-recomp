# The game executable: generated code for the main ELF and each overlay, the
# overlay manager, and main().

set(RG_OVERLAYS title insmenu normal factory insect)

# Generated code aliases memory across access widths and relies on wrapping
# integer arithmetic; keep the optimizer from assuming otherwise.
set(RG_GENERATED_FLAGS -O2 -g0 -fno-strict-aliasing -fwrapv)

# ee_fpu.h reaches the generated code only through its precompiled header;
# without it the EE float conversions would quietly round again.
if(CMAKE_DISABLE_PRECOMPILE_HEADERS)
    message(FATAL_ERROR "The generated code needs its precompiled header (ee_fpu.h); "
                        "configure without CMAKE_DISABLE_PRECOMPILE_HEADERS")
endif()

# One static library per generated tree. Each tree has its own
# ps2_recompiled_functions.h behind the same include guard, so a tree's own
# directory must come first on its include path and trees never share a TU.
function(rg_add_generated target dir)
    file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
    list(REMOVE_ITEM sources ${ARGN})
    list(LENGTH sources count)
    if(count EQUAL 0)
        message(FATAL_ERROR "No generated sources in ${dir}. Run: python setup.py recompile")
    endif()
    message(STATUS "${target}: ${count} generated translation units")

    add_library(${target} STATIC ${sources})
    target_include_directories(${target} BEFORE PRIVATE "${dir}")
    # Include paths, -msse4.1 and runtime definitions come from ps2_runtime.
    target_link_libraries(${target} PRIVATE ps2_runtime)
    target_compile_options(${target} PRIVATE ${RG_GENERATED_FLAGS})
    # One function per file and no file-local symbols, as ps2EntryRunner
    # itself assumes: batching them cuts the header parsing ~64-fold.
    set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
    # ee_fpu.h overrides runtime macros, so it comes right after them.
    target_precompile_headers(${target} PRIVATE
        <ps2_runtime_macros.h> "${CMAKE_CURRENT_SOURCE_DIR}/ee_fpu.h" <ps2_runtime.h>
        <ps2_recompiled_functions.h> <ps2_recompiled_stubs.h>
        <ps2_syscalls.h> <ps2_stubs.h>)
endfunction()

# Main ELF: its register_functions.cpp defines the runtime's own function
# table (g_ps2RecompiledFunctionTable and friends).
rg_add_generated(rg_generated_main "${RG_GENERATED_DIR}/main")

# Overlays: their register_functions.cpp would define the same table symbols.
# Each is compiled instead through overlay_table.cpp.in, which renames them
# and exposes the table as a FunctionRegion for the overlay manager.
set(RG_OVERLAY_LIBS)
foreach(name IN LISTS RG_OVERLAYS)
    set(dir "${RG_GENERATED_DIR}/${name}")
    if(NOT EXISTS "${dir}/overlay_info.h")
        message(FATAL_ERROR "${dir}/overlay_info.h is missing. Run: python setup.py recompile")
    endif()
    rg_add_generated(rg_generated_${name} "${dir}" "${dir}/register_functions.cpp")
    target_include_directories(rg_generated_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set(RG_OVERLAY_NAME "${name}")
    configure_file(overlay/overlay_table.cpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/overlay_table_${name}.cpp" @ONLY)
    target_sources(rg_generated_${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/overlay_table_${name}.cpp")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/overlay_table_${name}.cpp" PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    list(APPEND RG_OVERLAY_LIBS rg_generated_${name})
endforeach()

add_executable(roguegalaxy
    main.cpp
    graphics.cpp
    battle_rates.cpp
    button_art.cpp
    button_glyphs.cpp
    camera_view.cpp
    depth_reads.cpp
    dev_tools.cpp
    draw_distance.cpp
    ee_fpu.cpp
    flip_pacing.cpp
    font_trace.cpp
    free_camera.cpp
    movie_audio.cpp
    quick_scenes.cpp
    return_hook.cpp
    instant_text.cpp
    libc_rand.cpp
    loading_screen.cpp
    map_chests.cpp
    opening.cpp
    widescreen.cpp
    hud_size.cpp
    inventory.cpp
    save_anywhere.cpp
    update_check.cpp
    scene_timing.cpp
    settings_pages.cpp
    sharp_letters.cpp
    shop_prices.cpp
    step_fixes.cpp
    stick_response.cpp
    vif1_thread.cpp
    vu0_flags.cpp
    overlay/overlay_manager.cpp
    debug/call_trace.cpp
    debug/crash_report.cpp
    debug/gs_census.cpp
    debug/gs_timing.cpp
    debug/host_stacks.cpp
    debug/hw_watch.cpp
    debug/input_script.cpp
    debug/pos_trace.cpp
    debug/encounter.cpp
    debug/jump_trace.cpp
    debug/sampler.cpp
    debug/stall_watch.cpp
    debug/selftest.cpp
)
target_include_directories(roguegalaxy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GNU ld reads each archive once, in order: the function table defined in
    # rg_generated_main is first needed by ps2_runtime, listed after it, so the
    # member would never be pulled in. Rescan them as a group.
    target_link_libraries(roguegalaxy PRIVATE
        "$<LINK_GROUP:RESCAN,rg_generated_main,${RG_OVERLAY_LIBS},ps2_runtime>")
else()
    target_link_libraries(roguegalaxy PRIVATE rg_generated_main ${RG_OVERLAY_LIBS} ps2_runtime)
endif()
if(WIN32)
    # update_check.cpp asks GitHub through WinHTTP.
    target_link_libraries(roguegalaxy PRIVATE winhttp)
endif()
target_compile_definitions(roguegalaxy PRIVATE RG_APP_VERSION="${PROJECT_VERSION}")
# The commit this build is from, for the menu's About window (as of the last
# configure): the REVISION file of a release's source archive, which is no
# git checkout (tools/release/source_zip.py), or else git. Changed files make
# it "-dirty", but not the framework's checkout, which configure always
# changes with patches/ps2recomp (`git describe --dirty` counts that too).
if(EXISTS "${CMAKE_SOURCE_DIR}/REVISION")
    file(STRINGS "${CMAKE_SOURCE_DIR}/REVISION" RG_REVISION LIMIT_COUNT 1)
    set(RG_REVISION_RESULT 0)
else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE RG_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE RG_REVISION_RESULT)
    if(RG_REVISION_RESULT EQUAL 0 AND NOT RG_REVISION STREQUAL "")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no --ignore-submodules=dirty
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE rg_changed OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT rg_changed STREQUAL "")
            string(APPEND RG_REVISION "-dirty")
        endif()
    endif()
endif()
if(NOT RG_REVISION_RESULT EQUAL 0 OR RG_REVISION STREQUAL "")
    set(RG_REVISION "unknown")
endif()
target_compile_definitions(roguegalaxy PRIVATE RG_REVISION="${RG_REVISION}")
if(RG_GS_SDLGPU)
    target_link_libraries(roguegalaxy PRIVATE dq8_gfx_sdlgpu dq8_ui)
    target_compile_definitions(roguegalaxy PRIVATE RG_HAS_SDLGPU=1)
endif()
# On Windows the runtime's FFmpeg is DLLs; put them beside the executable.
ps2x_stage_ffmpeg_runtime_dlls(roguegalaxy)

# So do clang's MinGW C++ runtime DLLs, so the game starts without the
# toolchain on PATH (from Explorer or a launcher).
if(MINGW)
    get_filename_component(rg_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libc++.dll libunwind.dll libwinpthread-1.dll)
        if(EXISTS "${rg_toolchain_bin}/${dll}")
            add_custom_command(TARGET roguegalaxy POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${rg_toolchain_bin}/${dll}"
                        "$<TARGET_FILE_DIR:roguegalaxy>"
                VERBATIM)
        endif()
    endforeach()
endif()

# "Rogue Galaxy.exe", the launcher to double-click: debug\RogueGalaxy.bat's
# work without its terminal (launcher/launcher.c). It goes to the
# repository's folder; the batch file keeps the terminal for the other
# debug\ launchers and for runs from a terminal. Plain C linked statically, so it
# needs no DLL beside it. Its icon and manifest are a .res file that
# launcher/make_resources.py writes and the linker takes as it is (it says
# why not windres); without Python the launcher is built without them.
if(WIN32)
    set(rg_launcher_sources launcher/launcher.c)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        set(rg_launcher_resources "${CMAKE_CURRENT_BINARY_DIR}/launcher_resources.res")
        add_custom_command(OUTPUT "${rg_launcher_resources}"
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/launcher/make_resources.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/rogue-galaxy.ico"
                    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.manifest" "${rg_launcher_resources}"
            DEPENDS launcher/make_resources.py launcher/rogue-galaxy.ico launcher/launcher.manifest
            VERBATIM)
        set_source_files_properties("${rg_launcher_resources}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        list(APPEND rg_launcher_sources "${rg_launcher_resources}")
    endif()
    add_executable(rg_launcher WIN32 ${rg_launcher_sources})
    set_target_properties(rg_launcher PROPERTIES
        OUTPUT_NAME "Rogue Galaxy"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(MINGW)
        target_link_options(rg_launcher PRIVATE -static)
    endif()
endif()
