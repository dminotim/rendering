#version 450

// GLSL/SPIR-V port of `shadow_vertex_shader` from shaders/metal/MeshShadow.metal.

// 24 bytes, matching MeshVertex in mesh/Mesh.hpp. Scalar members only: a vec3 would force a
// 32-byte stride even in std430 and read every vertex from the wrong offset.
struct MeshVertex {
    float positionX;
    float positionY;
    float positionZ;
    uint  packedNormal;
    float u;
    float v;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    MeshVertex vertices[];
} vertexBuffer;

struct InstanceData {
    mat4 model;
    vec4 tint;
};

layout(std430, set = 0, binding = 2) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

// One per cascade, so it belongs to the pass rather than to the draw.
layout(std140, set = 0, binding = 1) uniform ShadowPassUniforms {
    mat4  lightViewProjection;
    float depthRange;      // world units spanned by the orthographic volume
    float pad0, pad1, pad2;
} pass;

layout(location = 0) out vec2 outUv;

void main() {
    MeshVertex v = vertexBuffer.vertices[gl_VertexIndex];
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];

    gl_Position = pass.lightViewProjection *
                  (instance.model * vec4(v.positionX, v.positionY, v.positionZ, 1.0));
    outUv = vec2(v.u, v.v);
}
