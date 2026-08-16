#version 450

// GLSL/SPIR-V port of `mesh_vertex_shader` from shaders/metal/Mesh.metal.
//
// Geometry comes from a read-only storage buffer indexed by gl_VertexIndex rather than through
// vertex attributes — the same model the rest of this project uses. The struct below must match
// MeshVertex in mesh/Mesh.hpp byte for byte, which is why it carries explicit padding: a vec3
// inside a std430 array occupies 16 bytes, not 12.

struct MeshVertex {
    vec3 position;
    float pad0;
    vec3 normal;
    float pad1;
    vec2 uv;
    vec2 pad2;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    MeshVertex vertices[];
} vertexBuffer;

// 64 bytes of matrix plus 48 of parameters, inside the 128 push constants guarantee.
layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    vec4 lightDirection;    // xyz: direction towards the light, w unused
    vec4 baseColor;         // rgb: material colour, a: 1 when a texture is bound
    vec4 cameraParams;      // x: ambient amount, yzw unused
} constants;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUv;

void main() {
    // Named `v` rather than `vertex` to mirror Mesh.metal, where `vertex` is a reserved
    // function qualifier and will not compile as a local.
    MeshVertex v = vertexBuffer.vertices[gl_VertexIndex];

    gl_Position = constants.modelViewProjection * vec4(v.position, 1.0);

    // The model only rotates and scales uniformly, so the normal passes through unchanged —
    // an inverse-transpose matrix would equal the ordinary one here. An arbitrary transform
    // would need that computed separately.
    outNormal = v.normal;
    outUv = v.uv;
}
