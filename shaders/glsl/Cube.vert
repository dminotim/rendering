#version 450

// GLSL/SPIR-V port of `cube_vertex_shader` from shaders/metal/Cube.metal.
//
// The first genuinely three-dimensional shader in the project: it takes a model-view-projection
// matrix through push constants and hands the fragment stage the object-space position, which
// doubles as both a volume texture coordinate and a cubemap direction.

struct VertexData {
    vec3 position;
};

// std430 gives a vec3 array a 16-byte stride, so the C++ side pads its vertices to match. Using
// vec4 here and ignoring w would work equally well; the padding has to exist either way.
layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    VertexData vertices[];
} vertexBuffer;

// 64 bytes of matrix plus 16 of parameters — half the 128 bytes push constants guarantee.
layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    float volumeMix;
    float pad0;
    float pad1;
    float pad2;
} constants;

// Per-instance offsets, selected by gl_InstanceIndex. Indirect draws issue many instances from
// one command, so anything that varies per instance has to come from a buffer rather than from
// push constants — the CPU never gets a chance to change them between instances.
layout(std140, set = 0, binding = 1) uniform InstanceData {
    vec4 offsets[9];
} instances;

layout(location = 0) out vec3 outObjectPosition;

void main() {
    vec3 position = vertexBuffer.vertices[gl_VertexIndex].position;
    vec3 offset = instances.offsets[gl_InstanceIndex].xyz;
    gl_Position = constants.modelViewProjection * vec4(position + offset, 1.0);

    // Object space, deliberately without the instance offset, so every cube samples the volume
    // and the cubemap over its own full range rather than a slice of a shared one.
    outObjectPosition = position;
}
