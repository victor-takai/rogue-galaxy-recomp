# SDL3's configure checks answered ahead (Rogue Galaxy: added, 2026-11-14).
# SDL3 runs some 180 checks at configure, 71-75 s of a first configure with
# the installer's toolchain (llvm-mingw 20260616). sdl3-preseed.cmake holds
# their answers for that toolchain: with them SDL3 configures in 2.5 s, its
# SDL_build_config.h the same. With any other compiler, target, flags,
# headers, CMake, SDL release or SDL patch they are left out and SDL3 checks
# as before; a check a library configured earlier already answered keeps its
# answer. In a fresh build folder -DDQ8_SDL3_PRESEED=OFF leaves them out too
# (answers already in a cache stay there). After changing the toolchain, SDL3's
# release or its patches, regenerate the file: python
# tools/install/sdl3_preseed.py (its help says how).

option(DQ8_SDL3_PRESEED "Answer SDL3's configure checks from cmake/sdl3-preseed.cmake when the toolchain matches" ON)

# What the answers depend on, but the headers: the SDL release, the source
# folder if one is given and the patches; the C compiler, its version and its
# target; CMake's version and the flags try_compile builds the checks with.
function(dq8_sdl3_preseed_base tag out)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE target OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(flags "")
    foreach(var IN ITEMS CMAKE_VERSION FETCHCONTENT_SOURCE_DIR_SDL3 CMAKE_C_COMPILER_TARGET CMAKE_SYSROOT
            CMAKE_TRY_COMPILE_CONFIGURATION CMAKE_C_STANDARD CMAKE_C_EXTENSIONS
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_DEBUG
            CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_EXE_LINKER_FLAGS_MINSIZEREL CMAKE_REQUIRED_FLAGS CMAKE_REQUIRED_DEFINITIONS
            CMAKE_REQUIRED_INCLUDES CMAKE_REQUIRED_LIBRARIES CMAKE_REQUIRED_LINK_OPTIONS)
        string(APPEND flags "${var}=${${var}}\n")
    endforeach()
    file(GLOB patches "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/sdl3-*.patch")
    list(SORT patches)
    foreach(patch IN LISTS patches)
        get_filename_component(name "${patch}" NAME)
        file(SHA256 "${patch}" sum)
        string(APPEND flags "${name} ${sum}\n")
    endforeach()
    string(SHA256 flags "${flags}")
    set(${out} "${tag}|${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}|${target}|${flags}" PARENT_SCOPE)
endfunction()

# The headers the compiler sees, by name and size, with _mingw.h by content
# (the MinGW version and the default C runtime are in it). A third of a second
# for llvm-mingw's 4000 files; more for a system's /usr/include.
function(dq8_sdl3_preseed_headers out)
    set(headers "")
    set(mingw "none")
    foreach(dir IN LISTS CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES)
        file(GLOB_RECURSE files RELATIVE "${dir}" "${dir}/*")
        list(SORT files)
        foreach(file IN LISTS files)
            # The glob lists a link to a folder, and a broken link, as a file.
            if(IS_DIRECTORY "${dir}/${file}" OR NOT EXISTS "${dir}/${file}")
                continue()
            endif()
            file(SIZE "${dir}/${file}" size)
            string(APPEND headers "${file} ${size}\n")
        endforeach()
        if(mingw STREQUAL "none" AND EXISTS "${dir}/_mingw.h")
            file(SHA256 "${dir}/_mingw.h" mingw)
        endif()
    endforeach()
    string(SHA256 headers "${headers}${mingw}")
    set(${out} "${headers}" PARENT_SCOPE)
endfunction()

# Before SDL3 configures. The cache keeps what the generator needs to trust a
# folder: today's key, the key SDL3 first checked with here, and whether
# answers were ever loaded into it. The headers are read only when the rest of
# the key matches the recorded one, or when recording (OFF); otherwise the key
# ends in "-".
function(dq8_sdl3_preseed tag)
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sdl3-preseed.cmake")
    dq8_sdl3_preseed_base("${tag}" key)
    string(FIND "${DQ8_SDL3_PRESEED_FOR}" "${key}|" at)
    if(NOT DQ8_SDL3_PRESEED OR at EQUAL 0)
        dq8_sdl3_preseed_headers(headers)
    else()
        set(headers "-")
    endif()
    string(APPEND key "|${headers}")
    set(DQ8_SDL3_PRESEED_KEY "${key}" CACHE INTERNAL "What SDL3's configure answers depend on, now")
    set(first OFF)
    if(NOT DEFINED CACHE{DQ8_SDL3_CHECKED_FOR})
        set(first ON)
        set(DQ8_SDL3_CHECKED_FOR "${key}" CACHE INTERNAL "What SDL3's configure answers depended on in this folder's first configure")
    endif()
    if(NOT DQ8_SDL3_PRESEED)
        return()
    endif()
    if(NOT key STREQUAL DQ8_SDL3_PRESEED_FOR)
        if(first)
            # The installer's own toolchain (tools/install/install.ps1) should
            # match: the file is out of date, or the build adds flags.
            string(TOLOWER "${CMAKE_C_COMPILER}" compiler)
            string(TOLOWER "${CMAKE_SOURCE_DIR}/toolchain/" installer)
            string(FIND "${compiler}" "${installer}" at)
            if(at EQUAL 0)
                message(WARNING "SDL3: cmake/sdl3-preseed.cmake has no answers for the installer's "
                    "toolchain as configured here (another SDL3 release or patch, other flags or "
                    "headers), so SDL3 checks its configuration, about a minute longer. Regenerate "
                    "the file with tools/install/sdl3_preseed.py.")
            else()
                message(STATUS "SDL3: no answers recorded for this toolchain; it checks its configuration")
            endif()
        endif()
        return()
    endif()
    set(count 0)
    foreach(entry IN LISTS DQ8_SDL3_PRESEED_ANSWERS)
        string(REGEX MATCH "^([^=]+)=(.*)$" entry "${entry}")
        if(NOT DEFINED CACHE{${CMAKE_MATCH_1}})
            set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" CACHE INTERNAL "")
            math(EXPR count "${count} + 1")
        endif()
    endforeach()
    if(count GREATER 0)
        set(DQ8_SDL3_PRESEEDED ON CACHE INTERNAL "Answers from cmake/sdl3-preseed.cmake were loaded into this cache")
        message(STATUS "SDL3: ${count} configure checks answered from cmake/sdl3-preseed.cmake")
    endif()
endfunction()

# After SDL3 configures, given the cache's variables from before: the check
# answers SDL3 added (INTERNAL, 1 or empty), kept from the first configure that
# added any, for the generator.
function(dq8_sdl3_record_answers before)
    get_cmake_property(after CACHE_VARIABLES)
    list(REMOVE_ITEM after ${before})
    set(names "")
    foreach(name IN LISTS after)
        get_property(type CACHE "${name}" PROPERTY TYPE)
        if(type STREQUAL "INTERNAL" AND ("$CACHE{${name}}" STREQUAL "1" OR "$CACHE{${name}}" STREQUAL ""))
            list(APPEND names "${name}")
        endif()
    endforeach()
    if(names AND NOT DEFINED CACHE{DQ8_SDL3_ANSWER_NAMES})
        list(SORT names)
        set(DQ8_SDL3_ANSWER_NAMES "${names}" CACHE INTERNAL "The check answers SDL3 added to this cache")
    endif()
endfunction()
