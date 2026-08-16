#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────────────────────
// Shadow pass: the scene seen from the sun, recording how far the light travels
// before it hits something.
//
// The obvious implementation — render depth only, then sample the depth buffer
// with a comparison sampler — is unavailable here twice over: this wrapper rests
// depth images in an attachment layout that cannot be sampled, and SamplerDesc
// has no comparison mode. So distance goes into an ordinary colour target and
// the comparison happens by hand in the lighting shader.
//
// That turns out to have a compensating advantage. What is stored is a *linear
// distance in world units*, not a non-linear depth value, so the bias constants
// in the lighting shader are measured in centimetres and stay meaningful when
// the cascade size changes.
// ─────────────────────────────────────────────────────────────────────────────

struct MeshVertex {
    packed_float3 position;
    uint          packedNormal;
    packed_float2 uv;
};

struct InstanceData {
    float4x4 model;
    float4   tint;
};

// One per cascade, so it belongs to the pass rather than to the draw.
struct ShadowPassUniforms {
    float4x4 lightViewProjection;
    float    depthRange;      // world units spanned by the orthographic volume
    float    pad0, pad1, pad2;
};

// Shared with the main pass so one C++ struct feeds both. Only material.z
// (the alpha cutoff) is read here.
struct DrawConstants {
    float4 baseColor;
    float4 material;      // x roughness, y metallic, z alphaCutoff, w hasNormalMap
    float4 emissive;
};

struct ShadowVertexOut {
    float4 clipPosition [[position]];
    float2 uv;
};

vertex ShadowVertexOut shadow_vertex_shader(
        const device MeshVertex*   vertices  [[buffer(0)]],
        const device InstanceData* instances [[buffer(2)]],
        constant ShadowPassUniforms& pass    [[buffer(1)]],
        uint vertexId   [[vertex_id]],
        uint instanceId [[instance_id]])
{
    MeshVertex   v = vertices[vertexId];
    InstanceData instance = instances[instanceId];

    ShadowVertexOut out;
    out.clipPosition = pass.lightViewProjection * (instance.model * float4(float3(v.position), 1.0));
    out.uv = float2(v.uv);
    return out;
}

// [[position]] on the way *in* is window space, so .z is already the depth the
// rasteriser computed, in [0, 1] across the orthographic volume. Scaling by the
// volume's extent turns it into world units.
fragment float shadow_fragment_shader(ShadowVertexOut in [[stage_in]],
                                      constant ShadowPassUniforms& pass [[buffer(1)]])
{
    return in.clipPosition.z * pass.depthRange;
}

// Foliage must cast the shape of its leaves, not the rectangle they are drawn on.
// Without this variant every tree in the scene throws a solid slab of shadow.
fragment float shadow_fragment_masked(ShadowVertexOut in [[stage_in]],
                                      constant ShadowPassUniforms& pass [[buffer(1)]],
                                      constant DrawConstants& draw [[buffer(8)]],
                                      texture2d<float> albedoMap [[texture(0)]],
                                      sampler smp [[sampler(0)]])
{
    if (albedoMap.sample(smp, in.uv).a < draw.material.z) discard_fragment();
    return in.clipPosition.z * pass.depthRange;
}
