#version 450

// GLSL/SPIR-V port of `overlay_vertex_shader` from shaders/metal/Overlay.metal.
//
// Places the shared unit quad into an axis-aligned rectangle given in NDC, and hands the
// fragment stage a texture coordinate. Geometry arrives through the same storage buffer the
// other shaders use.

struct VertexData {
    vec2 position;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    VertexData vertices[];
} vertexBuffer;

// Push constants rather than a uniform buffer: this is 20 bytes that change per draw, which is
// exactly what the mechanism exists for — no buffer, no descriptor, no allocation.
//
// This is the one construct that differs in source between the backends. Overlay.metal receives
// the same struct as an ordinary constant reference at buffer index 8, because Metal has no
// push constants; the C++ call is setPushConstants() either way.
//
// Five scalar floats, matching the C++ struct byte for byte — see PlaneShader.frag for why the
// members are not grouped into vectors.
layout(push_constant) uniform OverlayConstants {
    float centerX;
    float centerY;
    float halfWidth;
    float halfHeight;
    float opacity;
} uniforms;

layout(location = 0) out vec2 outUv;

void main() {
    vec2 position = vertexBuffer.vertices[gl_VertexIndex].position;
    gl_Position = vec4(position * vec2(uniforms.halfWidth, uniforms.halfHeight) +
                       vec2(uniforms.centerX, uniforms.centerY),
                       0.0, 1.0);

    // Identical to Overlay.metal: clip-space +Y points up on both backends, and v = 0 is the top
    // texture row on both, so Y inverts here. See Composite.vert.
    outUv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
}
