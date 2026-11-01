#version 450

// A strip a VU1 program would have drawn, from the program's own inputs
// (GSNativeMesh, Rogue Galaxy: added): the program's transform, done here, to
// the GS window pixels, depth, Q, S/T, color and fog gs_draw.vert passes on,
// so gs_draw.frag draws it as it draws the program's triangles. The position
// goes out homogeneous, so the GPU clips the strip where the program would
// have dropped a triangle at its guard band or clipped it in slot E.
//
// The vertex is a GsGpuVertex slot read another way: x, y and z the object-
// space position, q and s the texture coordinates before the divide, t its
// mesh's record, the color before its clamp (0-255), and in the fog slot the
// Z the program computes (GSNativeMesh::Vertex::depth): this arithmetic
// rounds to nearest where the VU truncates, and a unit of Z decides a pass
// drawn at nearly the same depth. A draw holds the meshes of one state, each
// with its own transform (GsNativeMeshRecord).

layout(location = 0) in vec2 inPositionXY;
layout(location = 1) in vec2 inPositionZU;
layout(location = 2) in vec2 inVMesh;      // v, and the record in this flush
layout(location = 3) in vec4 inColor;
layout(location = 4) in float inDepth;     // the program's Z, when not negative

layout(set = 1, binding = 0) uniform VertexParams {
    // As gs_draw.vert's.
    vec4 targetSize;
    vec4 adjust;
    // x: the records earlier flushes put in the buffer.
    vec4 meshes;
} params;

struct Mesh {
    // Rows: clip = x * matrix[0] + y * matrix[1] + z * matrix[2] + matrix[3],
    // in GS window coordinates (XYOFFSET included) once divided by w.
    vec4 matrix[4];
    // F = clamp(fog.x + fog.w * w, at most fog.y, at least fog.z).
    vec4 fog;
    vec4 colorMax;
    // xy: XYOFFSET in pixels. z: 1 / the depth format's largest value.
    vec4 misc;
    // The projected z: x scales it, y caps it.
    vec4 depth;
};

layout(std430, set = 0, binding = 0) readonly buffer Meshes {
    Mesh meshes[];
};

layout(location = 0) noperspective out vec2 vTexCoord;
layout(location = 1) noperspective out float vQ;
layout(location = 2) noperspective out vec4 vColor;
layout(location = 3) noperspective out float vFog;

void main() {
    Mesh mesh = meshes[uint(params.meshes.x) + uint(inVMesh.y)];
    vec4 clip = inPositionXY.x * mesh.matrix[0] + inPositionXY.y * mesh.matrix[1] +
                inPositionZU.x * mesh.matrix[2] + mesh.matrix[3];
    float q = 1.0 / clip.w;
    vec3 projected = clip.xyz * q;
    projected.z = min(projected.z * mesh.depth.x, mesh.depth.y);
    // The program's FTOI4: x and y in 12.4 fixed point, Z in whole units.
    vec2 pixel = trunc(projected.xy * 16.0) / 16.0 - mesh.misc.xy;
    float depth = clamp((inDepth >= 0.0 ? inDepth : trunc(projected.z)) * mesh.misc.z, 0.0, 1.0);

    vec2 normalised = (pixel + params.adjust.xy) * params.targetSize.zw;
    vec2 ndc = normalised * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y * params.adjust.z, depth, 1.0) * clip.w;

    vTexCoord = vec2(inPositionZU.y, inVMesh.x) * q;
    vQ = q;
    vColor = trunc(min(inColor, mesh.colorMax)) / 255.0;
    // In the program's order: the upper bound first.
    float f = max(min(mesh.fog.x + mesh.fog.w * clip.w, mesh.fog.y), mesh.fog.z);
    vFog = trunc(f) / 255.0;
}
