# Hardware GS backend (SDL3 GPU) from DQ8-Recompiled, see README.md.
# Built only when RG_GS_SDLGPU is on; the game then picks it at run time.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8HostSimd.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8Shaders.cmake")

# SDL3, built from source and linked statically: nothing to install.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# The backend uses SDL's GPU API, not SDL_render. Its Direct3D and Vulkan
# renderers bring SDL_d3dmath.c, whose global MatrixIdentity/MatrixMultiply
# clash with raylib's at link time.
set(SDL_RENDER_D3D OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_D3D11 OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_D3D12 OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)
# Patched (Rogue Galaxy: added): dual-source blend factors
# (SDL_GPU_BLENDFACTOR_SRC1_ALPHA), so a source-alpha blend stores its exact
# alpha in the same pass as its colour; and, with DQ8_GFX_GPU_TIMES=1, a GPU
# timestamp at each debug label, the GPU's time logged by label
# (cmake/patches). Its configure checks come answered ahead for the
# installer's toolchain (Rogue Galaxy: added; cmake/Dq8Sdl3Preseed.cmake):
# a new tag or patch needs them regenerated (tools/install/sdl3_preseed.py).
set(DQ8_SDL3_TAG release-3.2.24)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8Sdl3Preseed.cmake")
dq8_sdl3_preseed("${DQ8_SDL3_TAG}")
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG ${DQ8_SDL3_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}"
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/sdl3-dual-source-blend.patch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8ApplyPatch.cmake"
    COMMAND "${CMAKE_COMMAND}"
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/sdl3-gpu-timestamps.patch
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8ApplyPatch.cmake")
get_cmake_property(DQ8_CACHE_BEFORE_SDL3 CACHE_VARIABLES)
FetchContent_MakeAvailable(SDL3)
dq8_sdl3_record_answers("${DQ8_CACHE_BEFORE_SDL3}")

# Backend-neutral GS support: local memory, swizzled access, transfers.
add_library(dq8_gfx_gs STATIC
    src/gfx/gs/gs_vram.cpp
    src/gfx/gs/gs_transfer.cpp
    src/gfx/gs/gs_state.cpp
)
target_include_directories(dq8_gfx_gs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dq8_gfx_gs PUBLIC dq8_host_simd ps2_runtime)

# The native pipeline (Rogue Galaxy: added; the game's docs/NATIVE-PIPELINE.md):
# records and the renderer that takes them. It links SDL3 alone, not the
# runtime, so nothing of the GS compiles here: the runtime's GS headers are
# not on its include path. Keep it that way; that is the guarantee.
add_library(dq8_gfx_native STATIC
    src/gfx/native/native_pipeline.cpp
    src/gfx/native/native_meshes.cpp
)
target_include_directories(dq8_gfx_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dq8_gfx_native PUBLIC SDL3::SDL3-static)
foreach(shader IN ITEMS
        "native_view.vert;vert;kNativeViewVertSpirv"
        "native_view.frag;frag;kNativeViewFragSpirv"
        "native_mesh.vert;vert;kNativeMeshVertSpirv"
        "native_mesh.frag;frag;kNativeMeshFragSpirv")
    list(GET shader 0 file)
    list(GET shader 1 stage)
    list(GET shader 2 symbol)
    dq8_add_shader(dq8_gfx_native
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/native/shaders/${file}"
        STAGE ${stage} SYMBOL ${symbol})
endforeach()

add_library(dq8_gfx_sdlgpu STATIC
    src/gfx/backends/sdlgpu/sdlgpu_backend.cpp
    src/gfx/backends/sdlgpu/sdlgpu_device.cpp
    src/gfx/backends/sdlgpu/sdlgpu_display.cpp
    src/gfx/backends/sdlgpu/sdlgpu_input.cpp
    src/gfx/backends/sdlgpu/sdlgpu_targets.cpp
    src/gfx/backends/sdlgpu/sdlgpu_textures.cpp
    window_icon.cpp
)
target_link_libraries(dq8_gfx_sdlgpu PUBLIC dq8_gfx_gs dq8_gfx_native SDL3::SDL3-static)

foreach(shader IN ITEMS
        "gs_draw.vert;vert;kGsDrawVertSpirv"
        "gs_native.vert;vert;kGsNativeVertSpirv"
        "gs_draw.frag;frag;kGsDrawFragSpirv"
        "gs_reinterpret.vert;vert;kGsReinterpretVertSpirv"
        "gs_reinterpret.frag;frag;kGsReinterpretFragSpirv"
        "gs_display.frag;frag;kGsDisplayFragSpirv"
        "gs_index8.frag;frag;kGsIndex8FragSpirv"
        "gs_local_copy.frag;frag;kGsLocalCopyFragSpirv"
        "present_sides.frag;frag;kPresentSidesFragSpirv")
    list(GET shader 0 file)
    list(GET shader 1 stage)
    list(GET shader 2 symbol)
    dq8_add_shader(dq8_gfx_sdlgpu
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/backends/sdlgpu/shaders/${file}"
        STAGE ${stage} SYMBOL ${symbol})
endforeach()
# The draw shader reading the destination from a snapshot, for the draws the
# fixed-function blend cannot express without framebuffer fetch (Rogue
# Galaxy: added).
dq8_add_shader(dq8_gfx_sdlgpu
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/backends/sdlgpu/shaders/gs_draw.frag"
    STAGE frag SYMBOL kGsDrawDstCopyFragSpirv DESTINATION_COPY)
# The draw shader giving the blend its source-alpha factor as a second
# output, so the first keeps the exact alpha (Rogue Galaxy: added).
dq8_add_shader(dq8_gfx_sdlgpu
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/backends/sdlgpu/shaders/gs_draw.frag"
    STAGE frag SYMBOL kGsDrawDualFragSpirv DUAL_SOURCE)

# The in-game menu (src/ui), drawn with Dear ImGui over the backend's window.
# Dear ImGui is the submodule at thirdparty/imgui, the commit DQ8 pins.
set(DQ8_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui")
if(NOT EXISTS "${DQ8_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found under ${DQ8_IMGUI_DIR}. "
        "Run: git submodule update --init thirdparty/imgui")
endif()

add_library(dq8_imgui STATIC
    "${DQ8_IMGUI_DIR}/imgui.cpp"
    "${DQ8_IMGUI_DIR}/imgui_draw.cpp"
    "${DQ8_IMGUI_DIR}/imgui_tables.cpp"
    "${DQ8_IMGUI_DIR}/imgui_widgets.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${DQ8_IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
)
target_include_directories(dq8_imgui SYSTEM PUBLIC "${DQ8_IMGUI_DIR}" "${DQ8_IMGUI_DIR}/backends")
target_compile_definitions(dq8_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(dq8_imgui PUBLIC SDL3::SDL3-static)

# Roboto Medium from Dear ImGui's own fonts (Apache 2.0), compiled in so the
# menu needs no files beside the executable.
set(DQ8_UI_FONT "${DQ8_IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf")
set(DQ8_UI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_font_data.cpp")
# Depending only on what it names, as the shaders do (cmake/Dq8Shaders.cmake).
add_custom_command(
    OUTPUT "${DQ8_UI_FONT_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${DQ8_UI_FONT}" "-DOUTPUT=${DQ8_UI_FONT_SOURCE}"
            -DSYMBOL=kUiFontData -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
    DEPENDS "${DQ8_UI_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
    ${DQ8_DEPENDS_EXPLICIT_ONLY}
    COMMENT "Embedding the menu font"
    VERBATIM)

# The home screen's pictures (Steam Deck fork: added), embedded the same way:
# the background and the logo. A tree without a file gets an empty array, and
# the screen draws a sky / the title in text.
function(dq8_embed_art INPUT OUTPUT SYMBOL)
    if(EXISTS "${INPUT}")
        add_custom_command(
            OUTPUT "${OUTPUT}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${INPUT}" "-DOUTPUT=${OUTPUT}"
                    "-DSYMBOL=${SYMBOL}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
            DEPENDS "${INPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dq8EmbedBinary.cmake"
            ${DQ8_DEPENDS_EXPLICIT_ONLY}
            COMMENT "Embedding ${SYMBOL}"
            VERBATIM)
    else()
        file(WRITE "${OUTPUT}"
            "extern const unsigned char ${SYMBOL}[] = {0};\nextern const unsigned int ${SYMBOL}_size = 0u;\n")
    endif()
endfunction()
set(DQ8_HOME_ART_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/home_art_data.cpp")
set(DQ8_HOME_LOGO_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/home_logo_data.cpp")
dq8_embed_art("${CMAKE_CURRENT_SOURCE_DIR}/../../installer/art/home.jpg" "${DQ8_HOME_ART_SOURCE}" kHomeArtData)
dq8_embed_art("${CMAKE_CURRENT_SOURCE_DIR}/../../installer/art/logo-home.png" "${DQ8_HOME_LOGO_SOURCE}" kHomeLogoData)

add_library(dq8_ui STATIC
    src/ui/ui_art.cpp
    src/ui/ui_glyphs.cpp
    src/ui/ui_overlay.cpp
    src/ui/ui_pages.cpp
    src/ui/ui_script.cpp
    src/ui/ui_settings.cpp
    src/ui/ui_style.cpp
    src/ui/ui_widgets.cpp
    "${DQ8_UI_FONT_SOURCE}"
    "${DQ8_HOME_ART_SOURCE}"
    "${DQ8_HOME_LOGO_SOURCE}"
)
target_link_libraries(dq8_ui PUBLIC dq8_gfx_sdlgpu dq8_imgui)
