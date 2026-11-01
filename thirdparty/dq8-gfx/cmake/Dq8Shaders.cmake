# Compile GLSL to embedded SPIR-V and, on macOS, cross-compile it to MSL.
# Require a compiler so shader edits cannot silently use stale bytecode.
# Compiler search order: system glslc, system glslangValidator/glslang, then a
# FetchContent build of glslang. Set DQ8_SHADER_COMPILER to force one.

include_guard(GLOBAL)

# Resolves the compiler once per configure, into global properties rather than
# cache variables. A fetched glslang is referenced by generator expression, and
# that target has to be created on every configure -- caching the expression
# instead would leave later runs pointing at a target nothing defined.
function(dq8_find_shader_compiler)
    get_property(ALREADY_RESOLVED GLOBAL PROPERTY DQ8_SHADER_COMPILER_RESOLVED)
    if(ALREADY_RESOLVED)
        return()
    endif()
    set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_RESOLVED ON)

    if(DQ8_SHADER_COMPILER)
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_PATH "${DQ8_SHADER_COMPILER}")
        if(NOT DQ8_SHADER_COMPILER_KIND)
            set(DQ8_SHADER_COMPILER_KIND "glslc")
        endif()
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_DIALECT "${DQ8_SHADER_COMPILER_KIND}")
        message(STATUS "dq8 shaders: using ${DQ8_SHADER_COMPILER} (${DQ8_SHADER_COMPILER_KIND})")
        return()
    endif()

    find_program(DQ8_SYSTEM_GLSLC NAMES glslc)
    if(DQ8_SYSTEM_GLSLC)
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_PATH "${DQ8_SYSTEM_GLSLC}")
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_DIALECT "glslc")
        message(STATUS "dq8 shaders: using system glslc (${DQ8_SYSTEM_GLSLC})")
        return()
    endif()

    find_program(DQ8_SYSTEM_GLSLANG NAMES glslangValidator glslang)
    if(DQ8_SYSTEM_GLSLANG)
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_PATH "${DQ8_SYSTEM_GLSLANG}")
        set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_DIALECT "glslang")
        message(STATUS "dq8 shaders: using system glslang (${DQ8_SYSTEM_GLSLANG})")
        return()
    endif()

    message(STATUS "dq8 shaders: no system GLSL compiler found; fetching glslang")
    include(FetchContent)
    # ENABLE_OPT pulls in SPIRV-Tools; the optimiser is not needed to emit valid
    # SPIR-V and skipping it keeps this to a single dependency.
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG 15.4.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(glslang)
    if(NOT TARGET glslang-standalone)
        message(FATAL_ERROR
            "dq8 shaders: fetched glslang but its standalone compiler target is "
            "missing. Install glslc or glslangValidator and re-run CMake.")
    endif()
    set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_PATH "$<TARGET_FILE:glslang-standalone>")
    set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_DIALECT "glslang")
    set_property(GLOBAL PROPERTY DQ8_SHADER_COMPILER_TARGET glslang-standalone)
endfunction()

# dq8_add_shader(<target> SOURCE <file> STAGE <vert|frag|comp> SYMBOL <c-identifier>)
#
# Compiles one GLSL file to SPIR-V and embeds it as a constexpr byte array in a
# generated header, which <target> can then #include. Embedding rather than
# loading at runtime keeps a backend a single self-contained object.
function(dq8_add_shader TARGET)
    cmake_parse_arguments(ARG "FRAMEBUFFER_FETCH;DESTINATION_COPY;DUAL_SOURCE;MSL_DECORATION_BINDING" "SOURCE;STAGE;SYMBOL" ""
                          ${ARGN})
    if(NOT ARG_SOURCE OR NOT ARG_STAGE OR NOT ARG_SYMBOL)
        message(FATAL_ERROR "dq8_add_shader: SOURCE, STAGE and SYMBOL are required")
    endif()

    dq8_find_shader_compiler()
    get_property(SHADER_COMPILER GLOBAL PROPERTY DQ8_SHADER_COMPILER_PATH)
    get_property(SHADER_DIALECT GLOBAL PROPERTY DQ8_SHADER_COMPILER_DIALECT)
    get_property(SHADER_COMPILER_TARGET GLOBAL PROPERTY DQ8_SHADER_COMPILER_TARGET)
    if(NOT SHADER_COMPILER)
        message(FATAL_ERROR "dq8_add_shader: no GLSL compiler available")
    endif()

    get_filename_component(STEM "${ARG_SOURCE}" NAME)
    set(SHADER_DEFINES)
    set(MSL_OPTIONS)
    set(MSL_VERSION 20100)
    if(ARG_MSL_DECORATION_BINDING)
        list(APPEND MSL_OPTIONS --msl-decoration-binding)
    endif()
    if(ARG_FRAMEBUFFER_FETCH)
        string(APPEND STEM ".fetch")
        list(APPEND SHADER_DEFINES -DDQ8_FRAMEBUFFER_FETCH=1)
        list(APPEND MSL_OPTIONS --msl-framebuffer-fetch)
        set(MSL_VERSION 20300)
    endif()
    # The destination read through a snapshot texture (Rogue Galaxy: added).
    if(ARG_DESTINATION_COPY)
        string(APPEND STEM ".dstcopy")
        list(APPEND SHADER_DEFINES -DDQ8_DESTINATION_COPY=1)
    endif()
    # The blend factor as a second output (Rogue Galaxy: added).
    if(ARG_DUAL_SOURCE)
        string(APPEND STEM ".dual")
        list(APPEND SHADER_DEFINES -DDQ8_DUAL_SOURCE=1)
    endif()
    set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
    set(SPIRV "${GENERATED_DIR}/${STEM}.spv")
    set(HEADER "${GENERATED_DIR}/${STEM}.spv.h")

    if(SHADER_DIALECT STREQUAL "glslc")
        set(COMPILE_COMMAND "${SHADER_COMPILER}"
            "-fshader-stage=${ARG_STAGE}" --target-env=vulkan1.0 -O ${SHADER_DEFINES}
            -MD -MF "${SPIRV}.d" -o "${SPIRV}" "${ARG_SOURCE}")
        set(DEPFILE_ARG DEPFILE "${SPIRV}.d")
    else()
        # glslang infers the stage from the file extension, so the sources are
        # named .vert/.frag/.comp and no stage flag is passed.
        set(COMPILE_COMMAND "${SHADER_COMPILER}"
            -V --target-env vulkan1.0 ${SHADER_DEFINES} -o "${SPIRV}" "${ARG_SOURCE}")
        set(DEPFILE_ARG)
    endif()

    set(COMPILER_DEPENDENCY)
    if(SHADER_COMPILER_TARGET)
        set(COMPILER_DEPENDENCY "${SHADER_COMPILER_TARGET}")
    endif()

    add_custom_command(
        OUTPUT "${SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
        COMMAND ${COMPILE_COMMAND}
        DEPENDS "${ARG_SOURCE}" ${COMPILER_DEPENDENCY}
        ${DEPFILE_ARG}
        COMMENT "Compiling shader ${STEM}"
        VERBATIM COMMAND_EXPAND_LISTS)

    add_custom_command(
        OUTPUT "${HEADER}"
        COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${SPIRV}" "-DOUTPUT=${HEADER}" "-DSYMBOL=${ARG_SYMBOL}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedSpirv.cmake"
        DEPENDS "${SPIRV}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedSpirv.cmake"
        COMMENT "Embedding shader ${STEM}"
        VERBATIM)

    target_sources(${TARGET} PRIVATE "${HEADER}")
    target_include_directories(${TARGET} PRIVATE "${GENERATED_DIR}")

    if(APPLE)
        find_program(DQ8_SPIRV_CROSS NAMES spirv-cross)
        if(DQ8_SPIRV_CROSS)
            set(CROSS_COMMAND "${DQ8_SPIRV_CROSS}")
            set(CROSS_DEPENDENCY)
        else()
            if(NOT TARGET spirv-cross)
                include(FetchContent)
                set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
                set(SPIRV_CROSS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
                FetchContent_Declare(spirv_cross
                    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
                    GIT_TAG vulkan-sdk-1.4.321.0
                    GIT_SHALLOW TRUE)
                FetchContent_MakeAvailable(spirv_cross)
            endif()
            set(CROSS_COMMAND "$<TARGET_FILE:spirv-cross>")
            set(CROSS_DEPENDENCY spirv-cross)
        endif()
        set(MSL "${GENERATED_DIR}/${STEM}.msl")
        set(MSL_HEADER "${MSL}.h")
        add_custom_command(OUTPUT "${MSL}"
            COMMAND "${CROSS_COMMAND}" "${SPIRV}" --msl --msl-version ${MSL_VERSION}
                    ${MSL_OPTIONS} --output "${MSL}"
            DEPENDS "${SPIRV}" ${CROSS_DEPENDENCY}
            COMMENT "Cross-compiling ${STEM} for Metal" VERBATIM)
        add_custom_command(OUTPUT "${MSL_HEADER}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DINPUT=${MSL}" "-DOUTPUT=${MSL_HEADER}" "-DSYMBOL=${ARG_SYMBOL}Msl"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedText.cmake"
            DEPENDS "${MSL}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Dq8EmbedText.cmake"
            VERBATIM)
        target_sources(${TARGET} PRIVATE "${MSL_HEADER}")
        target_compile_definitions(${TARGET} PRIVATE DQ8_GFX_HAS_MSL=1)
    endif()
endfunction()
