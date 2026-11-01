#version 450

// GS pixel pipeline: texture lookup, TFX combine, fog, alpha test.
//
// Apple GPUs use framebuffer fetch for GS integer blending and destination
// tests. Other devices use the fixed-function mapping in sdlgpu_device.cpp,
// and for the draws it cannot express (a destination alpha test, a blend
// weighted by destination alpha) the DQ8_DESTINATION_COPY variant, which
// reads the destination from a snapshot of the target taken before the draw
// (Rogue Galaxy: added).
//
// Filtering and wrapping are done by hand rather than by the sampler because
// GS wrap modes REGION_CLAMP and REGION_REPEAT have no sampler equivalent:
// REGION_REPEAT is a bitwise and/or on the texel index.
//
// SDL3 GPU's SPIR-V binding convention: fragment textures live in set 2 and
// fragment uniform buffers in set 3.

layout(location = 0) noperspective in vec2 vTexCoord;
layout(location = 1) noperspective in float vQ;
layout(location = 2) noperspective in vec4 vColor;
layout(location = 3) noperspective in float vFog;

layout(set = 2, binding = 0) uniform sampler2D gsTexture;
#ifdef DQ8_FRAMEBUFFER_FETCH
layout(input_attachment_index = 0, set = 2, binding = 1) uniform subpassInput gsDestination;
#elif defined(DQ8_DESTINATION_COPY)
layout(set = 2, binding = 1) uniform sampler2D gsDestinationCopy;
#endif

layout(set = 3, binding = 0) uniform FragmentParams {
    // x: feature bits (FLAG_*)
    // y: alpha-test comparison (GsAlphaTest)
    // z: texture function (GsTextureFunction)
    // w: wrap modes, U in bits 0-1 and V in bits 2-3
    uvec4 control;
    // xyz: FOGCOL. w: fixed alpha reference for the alpha test, 0..1.
    vec4 fog;
    // xy: texture size in texels. zw: its reciprocal.
    vec4 textureSize;
    // REGION_* bounds in texels: minU, maxU, minV, maxV.
    vec4 region;
    // x: factor applied to the emitted alpha so fixed-function blending sees
    //    the GS's /128 scale. 1.0 when the alpha channel must stay exact.
    // y: physical texels per GS texel. Above 1 when the bound texture is a
    //    render target being sampled directly, which is stored at the render
    //    scale rather than the GS's own resolution.
    // zw: live-target format (1=CT24, 2=CT16) and TA0 | TA1<<8 | AEM<<16.
    vec4 misc;
    // ALPHA operands/FIX, FBMSK, TEST, ABE/COLCLAMP/PABE/FBA and framebuffer PSM.
    uvec4 blend;
} params;

layout(location = 0) out vec4 outColor;
#ifdef DQ8_DUAL_SOURCE
// The source-alpha blend's factor, A/128, for SDL_GPU_BLENDFACTOR_SRC1_ALPHA;
// outColor keeps the exact alpha the GS stores (Rogue Galaxy: added).
layout(location = 0, index = 1) out vec4 outFactor;
#endif

const uint FLAG_TME = 1u << 0u;
const uint FLAG_FST = 1u << 1u;
const uint FLAG_TCC = 1u << 2u;
const uint FLAG_FGE = 1u << 3u;
const uint FLAG_ATE = 1u << 4u;
const uint FLAG_LINEAR = 1u << 5u;
const uint FLAG_NATIVE_GRID = 1u << 6u;

const uint WRAP_REPEAT = 0u;
const uint WRAP_CLAMP = 1u;
const uint WRAP_REGION_CLAMP = 2u;
const uint WRAP_REGION_REPEAT = 3u;

const uint TFX_MODULATE = 0u;
const uint TFX_DECAL = 1u;
const uint TFX_HIGHLIGHT = 2u;
const uint TFX_HIGHLIGHT2 = 3u;

const uint ATST_NEVER = 0u;
const uint ATST_ALWAYS = 1u;
const uint ATST_LESS = 2u;
const uint ATST_LEQUAL = 3u;
const uint ATST_EQUAL = 4u;
const uint ATST_GEQUAL = 5u;
const uint ATST_GREATER = 6u;
const uint ATST_NOTEQUAL = 7u;

int wrapAxis(int texel, uint mode, int size, int regionMin, int regionMax) {
    if (mode == WRAP_CLAMP)
        return clamp(texel, 0, size - 1);
    if (mode == WRAP_REGION_CLAMP)
        return clamp(texel, regionMin, regionMax);
    if (mode == WRAP_REGION_REPEAT)
        // MINU is a mask and MAXU the bits or-ed back in, not a range.
        return (texel & regionMin) | regionMax;
    // REPEAT wraps to the power-of-two texture size.
    return texel & (size - 1);
}

// Where inside its GS texel a tap lands once a clamp moved it: at the edge's
// outermost physical texel. Keeping the tap's place inside the clamped GS
// texel read one well inside it: at 4x a bilinear tap one physical texel left
// of a glyph's host version read three texels in, the glyph's own stroke,
// and drew it again as a light line left of its border (Rogue Galaxy: added).
float clampedSubTexel(uint mode, int texel, int wrapped, float subTexel) {
    if (mode != WRAP_CLAMP && mode != WRAP_REGION_CLAMP)
        return subTexel;
    return texel < wrapped ? 0.0 : texel > wrapped ? 0.999 : subTexel;
}

// Wrapping is defined on GS texel indices, so it is applied in that space and
// the result scaled afterwards. `subTexel` selects a point inside the GS texel
// when the bound texture holds several physical texels per GS one, which is
// what keeps a render target's extra detail instead of averaging it away.
// For a native-grid draw from an upscaled source at the target's own scale:
// the sub-sample plane this fragment writes, so a bit-exact copy keeps each
// plane separate. Negative when unused.
vec2 g_plane = vec2(-1.0);

// A live target's texel as its view's format reads it: CT24 and CT16 take
// their alpha from TEXA (params.misc.z: 1 CT24, 2 CT16).
vec4 convertTexel(vec4 color) {
    const uint format = uint(params.misc.z);
    if (format != 0u) {
        uvec4 raw = uvec4(round(color * 255.0));
        const uint texa = uint(params.misc.w);
        const bool alphaBit = format == 2u && raw.a >= 128u;
        if (format == 2u)
            raw.rgb = (raw.rgb >> 3u) << 3u;
        const bool transparent = !alphaBit && (texa & 65536u) != 0u &&
                                 all(equal(raw.rgb, uvec3(0u)));
        raw.a = transparent ? 0u : alphaBit ? ((texa >> 8u) & 255u) : (texa & 255u);
        color = vec4(raw) / 255.0;
    }
    return color;
}

vec4 fetchTexelAt(ivec2 texel, vec2 subTexel) {
    const ivec2 size = ivec2(params.textureSize.xy);
    const uint wrapU = params.control.w & 3u;
    const uint wrapV = (params.control.w >> 2u) & 3u;
    const ivec4 region = ivec4(params.region);
    ivec2 wrapped = ivec2(wrapAxis(texel.x, wrapU, size.x, region.x, region.y),
                          wrapAxis(texel.y, wrapV, size.y, region.z, region.w));
    wrapped = clamp(wrapped, ivec2(0), size - 1);
    const vec2 sub = vec2(clampedSubTexel(wrapU, texel.x, wrapped.x, subTexel.x),
                          clampedSubTexel(wrapV, texel.y, wrapped.y, subTexel.y));

    const int scale = max(int(params.misc.y), 1);
    ivec2 physical = wrapped * scale;
    if (scale > 1)
        physical += g_plane.x >= 0.0 ? ivec2(g_plane)
                                     : ivec2(clamp(sub, vec2(0.0), vec2(0.999)) * float(scale));
    // Within the bound texture too: an upscaled target's sub-pixels past the
    // GS's last sample may reach a texel beyond the target, which the CPU
    // side does not pad when the GS itself never reads it.
    const ivec2 bound = min(size * scale, textureSize(gsTexture, 0)) - 1;
    return convertTexel(texelFetch(gsTexture, clamp(physical, ivec2(0), bound), 0));
}

vec4 fetchTexel(ivec2 texel) {
    // Centre of the GS texel: the right point for a corner of a bilinear tap.
    return fetchTexelAt(texel, vec2(0.5));
}

// The GS interpolates texture coordinates in fixed point, so a 1:1 copy lands
// exactly on each texel; the host interpolates in float and can land a hair
// below, where floor() picks the previous texel. DQ8 copies every frame to
// its display buffer in 16-pixel strips, and that pulled 4-pixel groups of
// dialog borders one row out of line. Far below the GS's 1/16 texel
// precision, so no coordinate the GS could produce rounds differently.
const float POINT_SAMPLE_BIAS = 1.0 / 256.0;

vec4 sampleTexture() {
    vec2 texel;
    if ((params.control.x & FLAG_FST) != 0u) {
        // UV is already in texels, at 1/16 precision.
        texel = vTexCoord;
    } else {
        // STQ: the GS divides per pixel, which is why Q is interpolated
        // separately rather than folded into gl_Position.
        const float q = abs(vQ) > 1.0e-8 ? vQ : 1.0;
        texel = (vTexCoord / q) * params.textureSize.xy;
    }

    if ((params.control.x & FLAG_NATIVE_GRID) != 0u) {
        // Move the coordinate to where the GS samples this native pixel:
        // GS pixel p covers host pixels [p*scale, (p+1)*scale) and is sampled
        // at the centre of the first. UVs are affine in a UV draw, so the
        // derivatives carry it exactly.
        // Integer arithmetic: a float divide by 3 lands just under a whole
        // number (600 / 3 -> 199.99998), so mod() returned 3 for plane 0 and
        // every copy at an odd scale read the next GS pixel there.
        const int scale = int((params.control.x >> 24u) & 15u);
        const ivec2 host = ivec2(gl_FragCoord.xy);
        const ivec2 plane = host % scale;
        const vec2 offset = -vec2(plane);
        texel += dFdx(texel) * offset.x + dFdy(texel) * offset.y;
        if (int(params.misc.y) == scale)
            g_plane = vec2(plane);
    }

    if ((params.control.x & FLAG_LINEAR) == 0u) {
        const vec2 base = floor(texel + POINT_SAMPLE_BIAS);
        return fetchTexelAt(ivec2(base), texel - base);
    }

    // An upscaled render target filtered across its own physical texels, as
    // a GS drawing at the higher resolution would: averaging native texels
    // instead brings every copy of the frame back down to native detail, and
    // DQ8 copies the whole frame to its display buffer bilinearly. Native-grid
    // copies keep their sub-sample plane and stay on the GS path below.
    const int sourceScale = max(int(params.misc.y), 1);
    if (sourceScale > 1 && g_plane.x < 0.0) {
        const vec2 physical = texel * float(sourceScale) - 0.5;
        const ivec2 first = ivec2(floor(physical));
        const vec2 weight = physical - vec2(first);
        // A target clamped in both axes, all four taps inside the clamp
        // (Rogue Galaxy: added): each tap's physical texel is itself,
        // as the general path below works out the long way, so the taps are
        // read directly. Textured draws were a few times an untextured one's
        // cost a pixel at 8x, most of it this arithmetic.
        const uint wrapU = params.control.w & 3u;
        const uint wrapV = (params.control.w >> 2u) & 3u;
        if ((wrapU == WRAP_CLAMP || wrapU == WRAP_REGION_CLAMP) &&
            (wrapV == WRAP_CLAMP || wrapV == WRAP_REGION_CLAMP)) {
            const ivec2 size = ivec2(params.textureSize.xy);
            const ivec4 region = ivec4(params.region);
            const ivec2 low = clamp(ivec2(wrapU == WRAP_CLAMP ? 0 : region.x, wrapV == WRAP_CLAMP ? 0 : region.z),
                                    ivec2(0), size - 1);
            const ivec2 high = clamp(ivec2(wrapU == WRAP_CLAMP ? size.x - 1 : region.y,
                                           wrapV == WRAP_CLAMP ? size.y - 1 : region.w),
                                     ivec2(0), size - 1);
            if (all(greaterThanEqual(first, low * sourceScale)) &&
                all(lessThan(first + 1, (high + 1) * sourceScale))) {
                const ivec2 bound = min(size * sourceScale, textureSize(gsTexture, 0)) - 1;
                const vec4 c00 = convertTexel(texelFetch(gsTexture, clamp(first, ivec2(0), bound), 0));
                const vec4 c10 = convertTexel(texelFetch(gsTexture, clamp(first + ivec2(1, 0), ivec2(0), bound), 0));
                const vec4 c01 = convertTexel(texelFetch(gsTexture, clamp(first + ivec2(0, 1), ivec2(0), bound), 0));
                const vec4 c11 = convertTexel(texelFetch(gsTexture, clamp(first + ivec2(1, 1), ivec2(0), bound), 0));
                return mix(mix(c00, c10, weight.x), mix(c01, c11, weight.x), weight.y);
            }
        }
        const float inverse = 1.0 / float(sourceScale);
        vec4 taps[4];
        for (int i = 0; i < 4; ++i) {
            const ivec2 p = first + ivec2(i & 1, i >> 1);
            // floor(p / scale), texels left of zero included, as the GS
            // wraps them. A float divide by 3 can land just below a whole
            // number; (p + 0.5) / scale is never within 0.5 / scale of one,
            // far past the float's error, so its floor is exact without the
            // integer divides, which were most of a textured draw's cost at
            // 8x (Rogue Galaxy: changed).
            const ivec2 gsTexel = ivec2(floor((vec2(p) + 0.5) * inverse));
            const vec2 sub = (vec2(p - gsTexel * sourceScale) + 0.5) / float(sourceScale);
            taps[i] = fetchTexelAt(gsTexel, sub);
        }
        return mix(mix(taps[0], taps[1], weight.x), mix(taps[2], taps[3], weight.x), weight.y);
    }

    // Bilinear about the texel centre, matching the GS's half-texel offset.
    vec2 base = texel - 0.5;
    // The GS weighs the four texels with 4 fractional bits. Copies through a
    // reinterpreted format depend on that: DQ8 moves a shadow mask into
    // alpha by copying CT16 halves with coordinates that drift by up to 1/16
    // texel, and a float weight of a few percent on the next row flips the
    // 1-bit alpha it carries. UV (2D) draws only; 3D keeps full precision.
    if ((params.control.x & FLAG_FST) != 0u)
        base = floor(base * 16.0 + 0.002) / 16.0;
    const vec2 fraction = fract(base);
    const ivec2 corner = ivec2(floor(base));
    // A texture at its own size (the cache's, not an upscaled target):
    // each axis wraps its two texels once, rather than each of the four
    // taps wrapping both (Rogue Galaxy: added); the same texels as
    // fetchTexel's.
    if (int(params.misc.y) <= 1) {
        const ivec2 size = ivec2(params.textureSize.xy);
        const ivec4 region = ivec4(params.region);
        const uint wrapU = params.control.w & 3u;
        const uint wrapV = (params.control.w >> 2u) & 3u;
        const ivec2 bound = min(size, textureSize(gsTexture, 0)) - 1;
        const int x0 = clamp(clamp(wrapAxis(corner.x, wrapU, size.x, region.x, region.y), 0, size.x - 1), 0, bound.x);
        const int x1 = clamp(clamp(wrapAxis(corner.x + 1, wrapU, size.x, region.x, region.y), 0, size.x - 1), 0, bound.x);
        const int y0 = clamp(clamp(wrapAxis(corner.y, wrapV, size.y, region.z, region.w), 0, size.y - 1), 0, bound.y);
        const int y1 = clamp(clamp(wrapAxis(corner.y + 1, wrapV, size.y, region.z, region.w), 0, size.y - 1), 0, bound.y);
        const vec4 d00 = convertTexel(texelFetch(gsTexture, ivec2(x0, y0), 0));
        const vec4 d10 = convertTexel(texelFetch(gsTexture, ivec2(x1, y0), 0));
        const vec4 d01 = convertTexel(texelFetch(gsTexture, ivec2(x0, y1), 0));
        const vec4 d11 = convertTexel(texelFetch(gsTexture, ivec2(x1, y1), 0));
        return mix(mix(d00, d10, fraction.x), mix(d01, d11, fraction.x), fraction.y);
    }
    const vec4 c00 = fetchTexel(corner);
    const vec4 c10 = fetchTexel(corner + ivec2(1, 0));
    const vec4 c01 = fetchTexel(corner + ivec2(0, 1));
    const vec4 c11 = fetchTexel(corner + ivec2(1, 1));
    return mix(mix(c00, c10, fraction.x), mix(c01, c11, fraction.x), fraction.y);
}

// TFX. The GS computes Ct * Cv >> 7 on 0..255 integers, so a vertex colour of
// 0x80 is unity. Both operands arrive here divided by 255, which leaves the
// product short by exactly 255/128 -- not 2, which would over-brighten every
// textured pixel by one part in 255.
const float TFX_SCALE = 255.0 / 128.0;

vec4 applyTextureFunction(vec4 texel, vec4 vertexColor) {
    const uint tfx = params.control.z;
    const bool useTextureAlpha = (params.control.x & FLAG_TCC) != 0u;
    vec4 result;

    if (tfx == TFX_DECAL) {
        result.rgb = texel.rgb;
        result.a = useTextureAlpha ? texel.a : vertexColor.a;
    } else if (tfx == TFX_HIGHLIGHT) {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE + vertexColor.a;
        result.a = useTextureAlpha ? texel.a + vertexColor.a : vertexColor.a;
    } else if (tfx == TFX_HIGHLIGHT2) {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE + vertexColor.a;
        result.a = useTextureAlpha ? texel.a : vertexColor.a;
    } else {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE;
        result.a = useTextureAlpha ? texel.a * vertexColor.a * TFX_SCALE : vertexColor.a;
    }
    return clamp(result, 0.0, 1.0);
}

bool alphaTestPasses(float alpha) {
    if ((params.control.x & FLAG_ATE) == 0u)
        return true;

    // Compared as the GS does, on 0..255 integers.
    const int value = int(round(alpha * 255.0));
    const int reference = int(round(params.fog.w * 255.0));
    switch (params.control.y) {
    case ATST_NEVER: return false;
    case ATST_ALWAYS: return true;
    case ATST_LESS: return value < reference;
    case ATST_LEQUAL: return value <= reference;
    case ATST_EQUAL: return value == reference;
    case ATST_GEQUAL: return value >= reference;
    case ATST_GREATER: return value > reference;
    case ATST_NOTEQUAL: return value != reference;
    }
    return true;
}

#if defined(DQ8_FRAMEBUFFER_FETCH) || defined(DQ8_DESTINATION_COPY)
ivec3 blendOperand(uint selector, ivec3 source, ivec3 destination) {
    return selector == 0u ? source : selector == 1u ? destination : ivec3(0);
}

vec4 loadDestination() {
#ifdef DQ8_FRAMEBUFFER_FETCH
    return subpassLoad(gsDestination);
#else
    // The snapshot is the whole target at its scale, as the attachment is.
    return texelFetch(gsDestinationCopy, ivec2(gl_FragCoord.xy), 0);
#endif
}

vec4 blendPixel(vec4 color) {
    ivec4 source = ivec4(round(color * 255.0));
    ivec4 destination = ivec4(round(loadDestination() * 255.0));
    const uint flags = params.blend.w;
    const uint psm = flags >> 8u;
    const bool ct16 = psm == 2u || psm == 10u;
    if (ct16) {
        destination.rgb = (destination.rgb >> 3) << 3;
        destination.a = destination.a >= 128 ? 128 : 0;
    } else if (psm == 1u) {
        destination.a = 128;
    }
    const uint test = params.blend.z;
    if (psm != 1u && (test & 16384u) != 0u &&
        ((destination.a >= 128) != ((test & 32768u) != 0u)))
        discard;
    if ((flags & 1u) != 0u && ((flags & 4u) == 0u || source.a >= 128)) {
        const uint equation = params.blend.x;
        const uint c = (equation >> 4u) & 3u;
        const int factor = c == 0u ? source.a : c == 1u ? destination.a : int(equation >> 8u);
        const ivec3 a = blendOperand(equation & 3u, source.rgb, destination.rgb);
        const ivec3 b = blendOperand((equation >> 2u) & 3u, source.rgb, destination.rgb);
        const ivec3 d = blendOperand((equation >> 6u) & 3u, source.rgb, destination.rgb);
        source.rgb = (((a - b) * factor) >> 7) + d;
    }
    source.rgb = (flags & 2u) != 0u ? clamp(source.rgb, ivec3(0), ivec3(255)) : (source.rgb & 255);
    if ((flags & 8u) != 0u && psm != 1u)
        source.a |= 128;
    const uvec4 mask = (uvec4(params.blend.y) >> uvec4(0u, 8u, 16u, 24u)) & 255u;
    source = ivec4((uvec4(source) & ~mask) | (uvec4(destination) & mask));
    if (ct16) {
        source.rgb = (source.rgb >> 3) << 3;
        source.a = source.a >= 128 ? 128 : 0;
    }
    return vec4(source) / 255.0;
}
#endif

void main() {
    vec4 color = vColor;
    if ((params.control.x & FLAG_TME) != 0u)
        color = applyTextureFunction(sampleTexture(), vColor);

    if (!alphaTestPasses(color.a))
        discard;

    if ((params.control.x & FLAG_FGE) != 0u)
        color.rgb = mix(params.fog.rgb, color.rgb, vFog);

    // The GS blend factor is A/128, not A/255. Fixed-function blending reads
    // the emitted alpha, so the scale is folded in here; params.misc.x is 1.0
    // whenever the alpha channel is written and must stay exact instead.
#if defined(DQ8_FRAMEBUFFER_FETCH) || defined(DQ8_DESTINATION_COPY)
    outColor = blendPixel(color);
#else
#ifdef DQ8_DUAL_SOURCE
    outColor = color;
    outFactor = vec4(min(color.a * params.misc.x, 1.0));
    bool exactAlpha = true;
#else
    outColor = vec4(color.rgb, min(color.a * params.misc.x, 1.0));
    bool exactAlpha = params.misc.x == 1.0;
#endif
    // FBA: the stored alpha gets its top bit (CT24 stores none). Rogue
    // Galaxy: added; it draws a weapon with alpha 0 and FBA into a layer,
    // then copies only the layer's pixels whose alpha is set. Only an exact
    // alpha is the stored one: a scaled one is a blend factor, and that
    // draw's alpha replay stores it.
    if ((params.blend.w & 8u) != 0u && (params.blend.w >> 8u) != 1u && exactAlpha)
        outColor.a = float(uint(round(outColor.a * 255.0)) | 128u) / 255.0;
#endif
}
