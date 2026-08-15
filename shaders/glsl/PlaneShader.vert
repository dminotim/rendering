#version 450

// GLSL/SPIR-V port of `plane_vertex_shader` from shaders/metal/PlaneShader.metal.
//
// Metal declares the geometry input as `const device VertexData* vertex_array [[buffer(0)]]`
// and indexes it with `vertex_id` — it never uses a vertex descriptor. The direct equivalent
// here is a read-only storage buffer indexed by gl_VertexIndex, which is why the Vulkan
// pipeline declares no vertex bindings and no vertex attributes either.
//
// Binding 0 of descriptor set 0 is the slot `CommandBuffer::setVertexBuffer(0, ...)` writes,
// mirroring Metal's [[buffer(0)]].

struct VertexData {
    vec2 position;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    // std430 gives this array an 8 byte stride, matching `struct Vertex { float position[2]; }`
    // on the C++ side byte for byte.
    VertexData vertices[];
} vertexBuffer;

// Matches Metal's `struct VertexOut { float4 clipSpacePosition [[position]]; }` — the built-in
// gl_Position is the only thing passed down to the fragment stage.

void main() {
    // The positions are already in NDC (-1..1), so a full-screen quad needs no matrices,
    // exactly as in the Metal version.
    gl_Position = vec4(vertexBuffer.vertices[gl_VertexIndex].position, 0.0, 1.0);
}
