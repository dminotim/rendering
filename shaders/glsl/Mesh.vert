#version 450

// GLSL/SPIR-V port of `mesh_vertex_shader` from shaders/metal/Mesh.metal.

// 24 bytes, matching MeshVertex in mesh/Mesh.hpp exactly.
//
// Every member is a scalar on purpose. A `vec3` has 16-byte alignment even in std430, so
// `vec3 position; uint normal; vec2 uv;` would lay out as 32 bytes per element and silently read
// every vertex from the wrong offset. Scalars align to 4 and pack tightly, which is the only way
// to hit 24.
struct MeshVertex {
    float positionX;
    float positionY;
    float positionZ;
    uint  packedNormal;   // octahedral, two signed 16-bit halves
    float u;
    float v;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    MeshVertex vertices[];
} vertexBuffer;

// One entry per drawn copy, 80 bytes. A storage buffer rather than a uniform block, so the
// count is bounded by memory instead of by the 16 KiB a uniform block portably allows.
struct InstanceData {
    mat4 model;
    vec4 tint;
};

layout(std430, set = 0, binding = 2) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

// Per-pass data: the same for every draw in the frame, and read at the same address by every
// thread, which is exactly what a uniform block is optimised for. 144 bytes under std140.
layout(std140, set = 0, binding = 1) uniform FrameUniforms {
    mat4 viewProjection;
    vec3 cameraPosition;   float exposure;
    vec3 sunDirection;     float sunIntensity;
    vec3 sunColor;         float ambientIntensity;
    vec3 skyColor;         float shadowSoftness;
    vec3 groundColor;      float shadowNormalBias;
    vec3 cameraForward;    float shadowConstantBias;
    vec4 cascadeSplit;
    vec4 cascadeTexelWorldSize;
    vec4 cascadeDepthRange;
    mat4 cascadeMatrix[4];
    float shadowStrength;
    float shadowEnabled;
    float cascadeDebug;
    float pad;
} frame;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

// Inverse of packNormal() in mesh/Mesh.cpp. unpackSnorm2x16 uses the same /32767 convention
// the packer does, so the two are exact counterparts.
vec3 unpackOctNormal(uint packed) {
    vec2 e = unpackSnorm2x16(packed);

    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        // Fold the lower hemisphere back from the corners of the square.
        // Explicit comparison rather than sign(): sign(0) is 0, which would collapse the vector.
        vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

void main() {
    MeshVertex v = vertexBuffer.vertices[gl_VertexIndex];
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];

    vec4 world = instance.model * vec4(v.positionX, v.positionY, v.positionZ, 1.0);

    gl_Position      = frame.viewProjection * world;
    outWorldPosition = world.xyz;
    // Rotation and uniform scale only, so the upper 3x3 carries the normal correctly; it is
    // renormalised in the fragment stage regardless.
    outWorldNormal   = (instance.model * vec4(unpackOctNormal(v.packedNormal), 0.0)).xyz;
    outUv            = vec2(v.u, v.v);
}
