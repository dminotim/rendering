#version 450

// GLSL/SPIR-V port of `composite_fragment_shader` from shaders/metal/Composite.metal.
//
// The single-source variant: shows render target 0 only. Used when MRT is switched off.
//
// Textures live in descriptor set 1 because Metal numbers `[[texture(n)]]` independently of
// `[[buffer(n)]]`; `setTexture(0, …)` therefore lands on set 1 binding 0, not set 0 binding 0.

layout(set = 1, binding = 0) uniform sampler2D sceneTarget;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sceneTarget, inUv);
}
