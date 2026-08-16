#version 450

// GLSL/SPIR-V port of `shadow_fragment_shader` / `shadow_fragment_masked` from
// shaders/metal/MeshShadow.metal. Compiled twice, the second time with ALPHA_TEST defined.

layout(std140, set = 0, binding = 1) uniform ShadowPassUniforms {
    mat4  lightViewProjection;
    float depthRange;
    float pad0, pad1, pad2;
} pass;

#ifdef ALPHA_TEST
// Shared with the main pass so one C++ struct feeds both. Only material.z is read here.
layout(push_constant) uniform DrawConstants {
    vec4 baseColor;
    vec4 material;    // x roughness, y metallic, z alphaCutoff, w hasNormalMap
    vec4 emissive;
} draw;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 0) in vec2 inUv;
#else
layout(location = 0) in vec2 inUv;   // unused, but the vertex shader writes it
#endif

layout(location = 0) out float outDistance;

void main() {
#ifdef ALPHA_TEST
    // Foliage must cast the shape of its leaves, not the rectangle they are drawn on. Without
    // this every tree in the scene throws a solid slab of shadow.
    if (texture(albedoMap, inUv).a < draw.material.z) discard;
#endif

    // gl_FragCoord.z is the depth the rasteriser computed, already in [0, 1] across the
    // orthographic volume. Scaling by the volume's extent turns it into world units, which is
    // what lets the bias constants in the lighting shader be expressed in centimetres.
    outDistance = gl_FragCoord.z * pass.depthRange;
}
