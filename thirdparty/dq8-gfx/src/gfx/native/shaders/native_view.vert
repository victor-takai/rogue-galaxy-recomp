#version 450

// The native pipeline's debug view (Rogue Galaxy: added): one quad per draw,
// four vertices with no vertex buffer, placed by `rect` (x0, y0, x1, y1 in
// clip space).
layout(set = 1, binding = 0) uniform ViewRect {
    vec4 rect;
} view;
layout(location = 0) out vec2 outUv;

void main() {
    vec2 corner = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
    outUv = corner;
    gl_Position = vec4(mix(view.rect.xy, view.rect.zw, corner), 0.0, 1.0);
}
