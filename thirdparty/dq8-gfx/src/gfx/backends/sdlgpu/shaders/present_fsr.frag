#version 450

// AMD FidelityFX Super Resolution 1 for the window's picture (Rogue Galaxy:
// added): EASU, the edge-adaptive upscale, then RCAS, the contrast-adaptive
// sharpen, one pass each (mode 0 and 1), ported from ffx_fsr1.h. The source
// is read by texel, inside its region only; the output is the viewport the
// pass draws.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform FsrParams {
    vec4 output_; // the viewport's x, y, width, height in the target
    vec4 region;  // the source region's x, y, width, height
    vec4 mode;    // x: 0 EASU, 1 RCAS; y: RCAS sharpness in stops (0 sharpest)
} params;
layout(location = 0) out vec4 outColor;

vec3 fetch(ivec2 p) {
    ivec2 lo = ivec2(params.region.xy);
    ivec2 hi = lo + ivec2(params.region.zw) - 1;
    return texelFetch(source, clamp(p + lo, lo, hi), 0).rgb;
}

float luma(vec3 c) { return c.g + 0.5 * (c.r + c.b); }

// One of the four bilinear neighbours' share of the edge direction and
// length: a above, b left, c centre, d right, e below.
void setDir(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e) {
    float dirX = d - b;
    float lenX = max(abs(d - c), abs(c - b));
    lenX = clamp(abs(dirX) / max(lenX, 1.0 / 65536.0), 0.0, 1.0);
    dir.x += dirX * w;
    len += lenX * lenX * w;
    float dirY = e - a;
    float lenY = max(abs(e - c), abs(c - a));
    lenY = clamp(abs(dirY) / max(lenY, 1.0 / 65536.0), 0.0, 1.0);
    dir.y += dirY * w;
    len += lenY * lenY * w;
}

void tap(inout vec3 sum, inout float total, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    sum += c * w;
    total += w;
}

vec3 easu(vec2 pixel) {
    // The output pixel's centre in source texels, less half a texel: f is
    // the texel at its top left.
    vec2 pp = (pixel + 0.5) * params.region.zw / params.output_.zw - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 f0 = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = fetch(f0 + ivec2(0, -1)), c = fetch(f0 + ivec2(1, -1));
    vec3 e = fetch(f0 + ivec2(-1, 0)), f = fetch(f0), g = fetch(f0 + ivec2(1, 0)), h = fetch(f0 + ivec2(2, 0));
    vec3 i = fetch(f0 + ivec2(-1, 1)), j = fetch(f0 + ivec2(0, 1)), k = fetch(f0 + ivec2(1, 1)),
         l = fetch(f0 + ivec2(2, 1));
    vec3 n = fetch(f0 + ivec2(0, 2)), o = fetch(f0 + ivec2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    setDir(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    setDir(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    setDir(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    setDir(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    float dirR = dot(dir, dir);
    bool zero = dirR < 1.0 / 32768.0;
    dir = zero ? vec2(1.0, 0.0) : dir * inversesqrt(dirR);
    len = 0.5 * len;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 sum = vec3(0.0);
    float total = 0.0;
    tap(sum, total, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    tap(sum, total, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    tap(sum, total, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    tap(sum, total, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    tap(sum, total, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    tap(sum, total, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    tap(sum, total, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    tap(sum, total, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    tap(sum, total, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    tap(sum, total, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    tap(sum, total, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    tap(sum, total, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    // No ringing past the four nearest texels.
    vec3 lo = min(min(f, g), min(j, k));
    vec3 hi = max(max(f, g), max(j, k));
    return clamp(sum / total, lo, hi);
}

vec3 rcas(vec2 pixel) {
    //   b
    // d e f
    //   h
    ivec2 p = ivec2(pixel * params.region.zw / params.output_.zw);
    vec3 b = fetch(p + ivec2(0, -1)), d = fetch(p + ivec2(-1, 0)), e = fetch(p), f = fetch(p + ivec2(1, 0)),
         h = fetch(p + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * exp2(-params.mode.y);
    return clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0);
}

void main() {
    vec2 pixel = floor(gl_FragCoord.xy - params.output_.xy);
    outColor = vec4(params.mode.x == 0.0 ? easu(pixel) : rcas(pixel), 1.0);
}
