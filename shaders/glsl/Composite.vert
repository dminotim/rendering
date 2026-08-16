#version 450

// GLSL/SPIR-V port of `composite_vertex_shader` from shaders/metal/Composite.metal.
//
// Geometry arrives the same way it does in PlaneShader.vert: out of a read-only storage buffer
// indexed by gl_VertexIndex, mirroring Metal's buffer-pointer model, so the pipeline declares
// no vertex attributes.

struct VertexData {
    vec2 position;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    VertexData vertices[];
} vertexBuffer;

layout(location = 0) out vec2 outUv;

void main() {
    vec2 position = vertexBuffer.vertices[gl_VertexIndex].position;
    gl_Position = vec4(position, 0.0, 1.0);

    // NDC (-1..1) to texture coordinates (0..1).
    //
    // Identical to the expression in Composite.metal, and deliberately so: the Vulkan backend
    // renders with a flipped viewport so that +Y points up in clip space, exactly as it does on
    // Metal. Y is inverted here because both APIs place v = 0 at the *top* texture row while
    // clip-space +Y points at the top of the screen.
    //
    // This file used to carry the opposite expression, back when the backends disagreed about
    // the sign of clip-space Y. If you ever find yourself needing to differ from the MSL again,
    // the convention has been broken somewhere rather than the shader being wrong.
    outUv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
}
