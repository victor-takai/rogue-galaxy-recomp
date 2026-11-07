#version 450

// The native pipeline's debug view (Rogue Galaxy: added): a texture as the
// renderer holds it. Mode 0: RGBA. Mode 1: Index8 through its palette (a
// 16- or 256-wide row), fetched by index. Mode 2: Index8 with no palette, as
// grey. Shown opaque: alpha is the game's, 0x80 for opaque, and not what
// this view is for.
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 2, binding = 1) uniform sampler2D palette;
layout(set = 3, binding = 0) uniform ViewMode {
    uvec4 mode; // x: mode, y: palette entries
} params;
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(image, inUv);
    if (params.mode.x == 1u) {
        int index = int(texel.r * 255.0 + 0.5);
        texel = texelFetch(palette, ivec2(min(index, int(params.mode.y) - 1), 0), 0);
    } else if (params.mode.x == 2u) {
        texel = vec4(texel.rrr, 1.0);
    }
    outColor = vec4(texel.rgb, 1.0);
}
