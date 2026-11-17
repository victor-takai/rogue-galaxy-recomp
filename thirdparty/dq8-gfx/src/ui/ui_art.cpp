#include "ui/ui_art.h"

#include <imgui_internal.h> // RegisterUserTexture

#include <cstdio>
#include <cstring>

// Static: raylib, linked in the same executable, has its own copy of stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "ui/stb_image.h"

// Generated from installer/art/home.jpg and logo-home.png; empty when absent.
extern const unsigned char kHomeArtData[];
extern const unsigned int kHomeArtData_size;
extern const unsigned char kHomeLogoData[];
extern const unsigned int kHomeLogoData_size;

namespace dq8::ui {

namespace {
struct Picture {
    ImTextureData *data = nullptr;
    bool tried = false;
};

void load(Picture &picture, const unsigned char *bytes, unsigned int size, const char *name) {
    picture.tried = true;
    if (size == 0u)
        return;
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 4);
    if (pixels && width > 0 && height > 0) {
        // Kept for the life of the process: the backend frees its copy of
        // the texture when the context goes.
        picture.data = IM_NEW(ImTextureData)();
        picture.data->Create(ImTextureFormat_RGBA32, width, height);
        std::memcpy(picture.data->GetPixels(), pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        ImGui::RegisterUserTexture(picture.data);
        std::fprintf(stderr, "[ui] home %s %dx%d\n", name, width, height);
    } else {
        std::fprintf(stderr, "[ui] home %s could not be decoded\n", name);
    }
    if (pixels)
        stbi_image_free(pixels);
}
} // namespace

bool homeArt(HomePicture which, ImTextureRef &texture, ImVec2 &size) {
    static Picture background, logo;
    Picture &picture = which == HomePicture::Logo ? logo : background;
    if (!picture.tried) {
        if (which == HomePicture::Logo)
            load(picture, kHomeLogoData, kHomeLogoData_size, "logo");
        else
            load(picture, kHomeArtData, kHomeArtData_size, "artwork");
    }
    if (!picture.data)
        return false;
    texture = picture.data->GetTexRef();
    size = ImVec2(static_cast<float>(picture.data->Width), static_cast<float>(picture.data->Height));
    return true;
}

} // namespace dq8::ui
