#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added): the game's texture
// function, alpha test and fog, in its own units (0x80 is 1.0 for colors
// and alphas). Blending is the pipeline's: the alpha written out is As /
// 0x80, the blend's source alpha and what a target stores (clamped at 1).
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 2, binding = 1) uniform sampler2D palette;
layout(set = 3, binding = 0) uniform Material {
    // x: 0 untextured, 1 RGBA, 2 Index8 through the palette, 3 a target
    //    (its alpha stored as A / 0x80, the blend's factor)
    // y: the texture function (0 modulate, 1 decal, 2 highlight, 3 highlight2)
    // z: the texel's alpha is used (TCC)
    // w: the palette's entries
    uvec4 mode;
    // x: the alpha test (0 always ... the Compare enum), y: its reference,
    // z: whether a failing pixel is discarded, w: fog on
    uvec4 test;
    // x, y: the size the uvs are normalized to (the game's 2^TW, 2^TH);
    // z, w: the texture's own, which an upload may leave smaller
    vec4 size;
    // x: a channel shuffle (1: alpha's bits 2-7 from green's), z: clamp u,
    // w: clamp v (y unused)
    vec4 blend;
    vec4 fogColor; // 0-1
    // x: the texel's alpha replaced by this (0-255) when not negative (a
    // CT24 read); y: then 0 where the texel is black (AEM); z: the
    // destination alpha test, 0 none, 1 drawn where the target's alpha is
    // below 0x80, 2 where at least (read from `palette`, the target's copy)
    vec4 texa;
    // The GS's CLAMP: x, y the u region (min, max), z, w the v region;
    // wrap.x, .y the modes (0 repeat, 1 clamp, 2 region clamp, 3 region
    // repeat).
    uvec4 region;
    uvec4 wrap;
} material;

int wrapAxis(int a, int size, uint mode, uint lo, uint hi, bool clampOld) {
    if (mode == 2u)
        a = clamp(a, int(lo), int(hi));
    else if (mode == 3u)
        a = (a & int(lo)) | int(hi);
    else
        return clampOld ? clamp(a, 0, size - 1) : int(mod(float(a), float(size)));
    return clamp(a, 0, size - 1);
}

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inFog;
layout(location = 0) out vec4 outColor;

bool passes(uint compare, float a, float ref) {
    switch (compare) {
    case 0u: return true;
    case 1u: return false;
    case 2u: return a < ref;
    case 3u: return a <= ref;
    case 4u: return a == ref;
    case 5u: return a >= ref;
    case 6u: return a > ref;
    default: return a != ref;
    }
}

void main() {
    if (material.texa.z > 0.0) {
        // Stored as A / 0x80: bit 7 set is 1.0.
        bool high = texelFetch(palette, ivec2(gl_FragCoord.xy), 0).a >= 0.999;
        if (high != (material.texa.z > 1.5))
            discard;
    }
    // The vertex color as the GS takes it: truncated to integers 0-255.
    vec4 f = clamp(floor(inColor), 0.0, 255.0);
    vec4 c = f;
    if (material.mode.x != 0u) {
        vec4 t;
        vec2 texel = inUv * material.size.xy; // in the game's texels
        // A region (a tile of a sheet) is fetched as the GS addresses it.
        bool region = material.mode.x != 3u && (material.wrap.x >= 2u || material.wrap.y >= 2u);
        if (material.mode.x == 2u || region) {
            // Fetched, not filtered: an index is not a color.
            ivec2 size = ivec2(material.size.zw);
            ivec2 at = ivec2(floor(texel));
            at.x = wrapAxis(at.x, size.x, material.wrap.x, material.region.x, material.region.y, material.blend.z != 0.0);
            at.y = wrapAxis(at.y, size.y, material.wrap.y, material.region.z, material.region.w, material.blend.w != 0.0);
            if (material.mode.x == 2u) {
                int index = int(texelFetch(image, at, 0).r * 255.0 + 0.5);
                t = texelFetch(palette, ivec2(min(index, int(material.mode.w) - 1), 0), 0);
            } else {
                t = texelFetch(image, at, 0);
            }
        } else {
            t = texture(image, texel / material.size.zw);
        }
        bool target = material.mode.x == 3u;
        t = vec4(floor(t.rgb * 255.0 + 0.5), floor(t.a * (target ? 128.0 : 255.0) + 0.5));
        if (material.texa.x >= 0.0)
            t.a = (material.texa.y != 0.0 && t.rgb == vec3(0.0)) ? 0.0 : material.texa.x;
        if (material.blend.x == 1.0)
            t.a = float((uint(t.g) & 0xFCu) | (uint(t.a) & 3u));
        bool tcc = material.mode.z != 0u;
        uint fn = material.mode.y;
        if (fn == 1u) {
            c.rgb = t.rgb;
            c.a = tcc ? t.a : f.a;
        } else {
            c.rgb = floor(f.rgb * t.rgb / 128.0);
            if (fn >= 2u)
                c.rgb += f.a;
            if (fn == 0u)
                c.a = tcc ? floor(f.a * t.a / 128.0) : f.a;
            else if (fn == 2u)
                c.a = tcc ? t.a + f.a : f.a;
            else
                c.a = tcc ? t.a : f.a;
        }
        c = min(c, 255.0);
    }
    if (!passes(material.test.x, c.a, float(material.test.y)) && material.test.z != 0u)
        discard;
    if (material.test.w != 0u) {
        float k = clamp(floor(inFog), 0.0, 255.0) / 255.0;
        c.rgb = mix(material.fogColor.rgb * 255.0, c.rgb, k);
    }
    outColor = vec4(c.rgb / 255.0, c.a / 128.0);
}
