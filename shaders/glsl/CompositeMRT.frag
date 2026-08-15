#version 450

// GLSL/SPIR-V port of `composite_fragment_shader_mrt` from shaders/metal/Composite.metal.
//
// The two-source variant: render target 0 on the left of the split, render target 1 on the
// right, with a thin divider. Both targets were written by a single MRT pass earlier in the
// same command buffer; on Vulkan that pass left them in SHADER_READ_ONLY_OPTIMAL and its
// outgoing subpass dependency made the writes visible here, so no barrier is needed.
//
// See Composite.frag for why textures sit in descriptor set 1.

layout(std140, set = 0, binding = 1) uniform CompositeUniforms {
    float viewportSizeX;
    float viewportSizeY;
    float splitX;
} uniforms;

layout(set = 1, binding = 0) uniform sampler2D sceneTarget;
layout(set = 1, binding = 1) uniform sampler2D distanceTarget;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 scene = texture(sceneTarget, inUv);
    vec4 dist = texture(distanceTarget, inUv);

    float x = gl_FragCoord.x;
    if (abs(x - uniforms.splitX) < 1.0) {
        outColor = vec4(0.15, 0.18, 0.22, 1.0);
        return;
    }
    outColor = (x < uniforms.splitX) ? scene : dist;
}
