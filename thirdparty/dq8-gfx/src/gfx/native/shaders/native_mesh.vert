#version 450

// The native pipeline's mesh draw (Rogue Galaxy: added; the game's
// docs/NATIVE-PIPELINE.md step 4b). Object space through model x view x
// projection to clip space (the game's projection: GL's -w..w in z), then
// the camera's viewport to the target's pixels. Depth is written reversed,
// 1 at the near plane and 0 at the far, as a float.
//
// Matrices are the game's row vectors, sent as they are: GLSL's column-major
// reading makes `m * v` the game's `v * m`.
layout(set = 1, binding = 0) uniform Draw {
    mat4 clip;     // model x view x projection
    mat4 viewport; // clip to the target's pixels (w kept)
    vec4 target;   // the target's width and height in game pixels; z: fog per vertex (a window-space draw)
    vec4 fog;      // F = clamp(fog.x + fog.w * w, at most fog.y, at least fog.z)
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inFog;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outFog;

void main() {
    vec4 clip = draw.clip * vec4(inPosition, 1.0);
    vec4 window = draw.viewport * clip;
    outUv = inUv;
    outColor = inColor;
    outFog = draw.target.z != 0.0 ? inFog : clamp(draw.fog.x + draw.fog.w * clip.w, draw.fog.z, draw.fog.y);
    // Window pixels to SDL's normalized coordinates (y up), times w so the
    // GPU's divide gives them back.
    gl_Position = vec4(window.x * 2.0 / draw.target.x - window.w,
                       window.w - window.y * 2.0 / draw.target.y,
                       (clip.w - clip.z) * 0.5,
                       clip.w);
}
