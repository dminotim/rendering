#version 450

// GLSL/SPIR-V port of `overlay_fragment_shader` from shaders/metal/Overlay.metal.
//
// Samples a CPU-uploaded, mipmapped texture and scales its alpha. The pipeline that uses this
// shader enables source-alpha blending, so the result composites over whatever the pass already
// drew rather than replacing it.

// The same push constant block the vertex stage declares; see Overlay.vert.
layout(push_constant) uniform OverlayConstants {
    float centerX;
    float centerY;
    float halfWidth;
    float halfHeight;
    float opacity;
} uniforms;

// Textures live in descriptor set 1; see Composite.frag.
layout(set = 1, binding = 0) uniform sampler2D overlayTexture;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Drawn smaller than the texture is stored, so the sampler selects a lower mip level. With
    // a single-level texture the fine checker pattern would alias into moire instead.
    vec4 texel = texture(overlayTexture, inUv);
    outColor = vec4(texel.rgb, texel.a * uniforms.opacity);
}
