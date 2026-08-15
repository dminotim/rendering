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
    // This is the one line where the GLSL and the MSL genuinely have to differ, so it is worth
    // being explicit about why. The two APIs disagree on the sign of NDC Y:
    //
    //   Vulkan  ndc.y = -1 is the TOP of the viewport
    //   Metal   ndc.y = +1 is the TOP of the viewport
    //
    // Both sample textures with v = 0 at the top row, and both give the fragment stage a
    // framebuffer position with its origin at the top-left — which is why PlaneShader.frag needs
    // no adjustment at all. Only code converting a *clip space* position into a texture
    // coordinate has to care, and it does so in opposite directions:
    //
    //   here (Vulkan)      uv.y = position.y * 0.5 + 0.5
    //   Composite.metal    uv.y = 0.5 - position.y * 0.5
    //
    // Copying the MSL expression into this file would flip the composited image vertically —
    // and, because the grid is periodic, would look almost right while placing every pan offset
    // and the distance-field target upside down.
    outUv = position * 0.5 + 0.5;
}
