#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────────────────────
// Scene shading: microfacet BRDF with one directional sun plus a hemisphere
// ambient term, and an alpha-masked variant for foliage.
//
// Everything here follows the derivation in the book's chapter 6: (n·l) is
// geometry, 1/pi is energy conservation, and D·V·F is the microfacet model.
// ─────────────────────────────────────────────────────────────────────────────

constant float PI = 3.14159265359;

// 24 bytes. Must match MeshVertex in mesh/Mesh.hpp byte for byte. Every member is
// a four-byte scalar, which is what lets the struct pack tightly — a float3 in an
// array would be padded to 16 bytes and cost twice the memory on a scene this size.
struct MeshVertex {
    packed_float3 position;
    uint          packedNormal;   // octahedral, two signed 16-bit halves
    packed_float2 uv;
};

// One entry per drawn copy. A storage buffer, so the count is bounded by memory
// rather than by the 16 KiB a uniform block would allow.
struct InstanceData {
    float4x4 model;
    float4   tint;
};

// Per-pass: identical for every draw in the frame, so a uniform block is the
// right home — every thread reads the same address, which is what the hardware
// caches best.
constant int kCascadeCount = 4;

struct FrameUniforms {
    float4x4 viewProjection;
    packed_float3 cameraPosition;   float exposure;
    packed_float3 sunDirection;     float sunIntensity;   // direction TOWARDS the sun
    packed_float3 sunColor;         float ambientIntensity;
    packed_float3 skyColor;         float shadowSoftness;
    packed_float3 groundColor;      float shadowNormalBias;
    packed_float3 cameraForward;    float shadowConstantBias;
    float4   cascadeSplit;          // view depth at which each cascade ends
    float4   cascadeTexelWorldSize; // world units covered by one shadow texel
    float4   cascadeDepthRange;     // world units spanned by each cascade's depth axis
    float4x4 cascadeMatrix[kCascadeCount];
    float    shadowStrength;
    float    shadowEnabled;
    float    cascadeDebug;
    float    _pad;
};

// Per-draw: 48 bytes of the guaranteed 128.
struct DrawConstants {
    float4   baseColor;      // rgb material colour, a opacity
    float4   material;       // x roughness, y metallic, z alphaCutoff, w hasNormalMap
    float4   emissive;       // rgb emissive colour
};

struct VertexOut {
    float4 clipPosition [[position]];
    float3 worldPosition;
    float3 worldNormal;
    float2 uv;
};

// ── Normal unpacking ────────────────────────────────────────────────────────
// Inverse of packNormal() in mesh/Mesh.cpp. as_type reinterprets the 32 bits as
// two signed shorts, which is exactly how they were written.
float3 unpackOctNormal(uint packed)
{
    const float2 e = float2(as_type<short2>(packed)) / 32767.0;

    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        // Fold the lower hemisphere back from the corners of the square.
        // select() rather than sign(): sign(0) is 0, which would collapse the vector.
        n.xy = (1.0 - abs(n.yx)) * select(float2(-1.0), float2(1.0), n.xy >= 0.0);
    }
    return normalize(n);
}

vertex VertexOut mesh_vertex_shader(
        const device MeshVertex*   vertices  [[buffer(0)]],
        const device InstanceData* instances [[buffer(2)]],
        constant FrameUniforms&    frame     [[buffer(1)]],
        uint vertexId   [[vertex_id]],
        uint instanceId [[instance_id]])
{
    // `vertex` is a Metal function qualifier and cannot name a local, so this is `v`.
    MeshVertex   v = vertices[vertexId];
    InstanceData instance = instances[instanceId];

    const float4 world = instance.model * float4(float3(v.position), 1.0);

    VertexOut out;
    out.clipPosition  = frame.viewProjection * world;
    out.worldPosition = world.xyz;
    // Rotation and uniform scale only, so the upper 3x3 carries the normal
    // correctly; it is renormalised in the fragment stage regardless.
    out.worldNormal   = (instance.model * float4(unpackOctNormal(v.packedNormal), 0.0)).xyz;
    out.uv            = float2(v.uv);
    return out;
}

// ── BRDF ────────────────────────────────────────────────────────────────────

// D: how many microfacets point along the half vector. GGX has a long tail,
// which is what gives a bright core with a soft halo instead of Phong's hard blob.
float distributionGGX(float NdotH, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;
    const float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// V: Smith height-correlated masking, already divided by 4(n·l)(n·v). Folding the
// denominator in removes a division and the numerical blow-up as NdotV goes to zero.
float visibilitySmith(float NdotV, float NdotL, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;
    const float v = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float l = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-7);
}

// F: Schlick. Reflectance rises to 1 at grazing angles for every material —
// this is why a wet street is mirror-like in the distance and dull underfoot.
float3 fresnelSchlick(float VdotH, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - VdotH), 5.0);
}

float3 shadeDirectional(float3 N, float3 V, float3 L, float3 radiance,
                        float3 diffuseColor, float3 F0, float roughness)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0.0);   // light below the horizon: nothing to compute

    const float3 H = normalize(V + L);
    const float NdotV = saturate(dot(N, V)) + 1e-5;   // avoids a bright rim on the silhouette
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));

    const float  D = distributionGGX(NdotH, roughness);
    const float  Vis = visibilitySmith(NdotV, NdotL, roughness);
    const float3 F = fresnelSchlick(VdotH, F0);

    // What reflected specularly cannot also reflect diffusely.
    return ((1.0 - F) * diffuseColor / PI + D * Vis * F) * radiance * NdotL;
}

// ── Display mapping ─────────────────────────────────────────────────────────
// The swapchain is BGRA8_UNORM rather than an sRGB format, so the encode happens
// here. That is a deliberate choice: ImGui writes colours that are already in sRGB,
// and an sRGB framebuffer would encode them a second time and wash the interface out.

// ACES approximation (Narkowicz). Compact, and its shoulder keeps highlights from
// flattening into white the way a plain Reinhard curve does.
float3 tonemapACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linearToSrgb(float3 c)
{
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);
}

// ── Shadows ─────────────────────────────────────────────────────────────────

// Which cascade covers this fragment. Compared against distance along the view
// axis rather than distance to the eye, so the boundaries are planes parallel to
// the screen and do not bulge at the corners.
int selectCascade(constant FrameUniforms& frame, float viewDepth)
{
    for (int i = 0; i < kCascadeCount - 1; ++i) {
        if (viewDepth < frame.cascadeSplit[i]) return i;
    }
    return kCascadeCount - 1;
}

/**
 * Returns 1 where the sun reaches, 0 where it does not.
 *
 * The bias is applied by moving the sample point along the surface normal rather
 * than by adding a constant to the comparison. A shadow texel covers an area of
 * surface, and on a slope the distances across that area differ — which is what
 * produces the striped self-shadowing known as shadow acne. The size of that
 * spread depends on the angle, so the correction has to as well. A constant large
 * enough to cure the worst slope detaches every shadow from its object; moving
 * along the normal by roughly one texel's width cures the slope and cancels
 * itself where the surface faces the light head-on.
 */
float sampleShadow(constant FrameUniforms& frame,
                   texture2d_array<float> shadowMaps,
                   sampler shadowSampler,
                   float3 worldPosition, float3 N, float viewDepth)
{
    if (frame.shadowEnabled < 0.5) return 1.0;

    const int cascade = selectCascade(frame, viewDepth);
    const float texelWorldSize = frame.cascadeTexelWorldSize[cascade];

    const float3 offsetPosition = worldPosition + N * (texelWorldSize * frame.shadowNormalBias);
    const float4 lightClip = frame.cascadeMatrix[cascade] * float4(offsetPosition, 1.0);

    // Orthographic, so w is 1; the divide is kept because it costs nothing and
    // makes the code correct if a perspective light is ever added.
    const float3 ndc = lightClip.xyz / lightClip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    // NDC to texture coordinates. Y inverts: clip space points up, texture space down.
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    const float currentDistance = ndc.z * frame.cascadeDepthRange[cascade];

    // Percentage-closer filtering. Each tap is compared on its own and the
    // *results* are averaged — never the distances. Averaging two distances from
    // either side of a silhouette yields a value no surface has, and the
    // comparison against it is meaningless. This is also why the sampler must be
    // Nearest: linear filtering would do that averaging in hardware.
    const float2 texelStep = 1.0 / float2(shadowMaps.get_width(), shadowMaps.get_height());
    const float radius = frame.shadowSoftness;

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float occluder =
                shadowMaps.sample(shadowSampler,
                                  uv + float2(x, y) * texelStep * radius, cascade).r;
            lit += (currentDistance - frame.shadowConstantBias <= occluder) ? 1.0 : 0.0;
        }
    }
    lit /= 9.0;

    // Fade the last cascade out at its far edge instead of ending abruptly.
    const float fadeStart = frame.cascadeSplit[kCascadeCount - 1] * 0.93;
    const float fade = saturate((viewDepth - fadeStart) /
                                max(frame.cascadeSplit[kCascadeCount - 1] - fadeStart, 1e-4));
    lit = mix(lit, 1.0, fade);

    return mix(1.0, lit, frame.shadowStrength);
}

// ── Fragment ────────────────────────────────────────────────────────────────

float4 shadeSurface(VertexOut in,
                    constant FrameUniforms& frame,
                    constant DrawConstants& draw,
                    texture2d<float> albedoMap,
                    texture2d<float> normalMap,
                    texture2d_array<float> shadowMaps,
                    sampler smp,
                    sampler shadowSampler,
                    bool frontFacing,
                    bool alphaTest)
{
    const float4 albedoSample = albedoMap.sample(smp, in.uv);

    // Alpha test first: a discarded fragment should not pay for anything after it.
    // The cutoff comes from the material rather than being hardcoded, so foliage and
    // wire mesh can be tuned separately.
    if (alphaTest && albedoSample.a < draw.material.z) discard_fragment();

    // The albedo texture is in an sRGB format, so the hardware already decoded it
    // to linear — before filtering, which is where a shader-side pow() gets it wrong.
    float3 albedo = albedoSample.rgb * draw.baseColor.rgb;

    float3 N = normalize(in.worldNormal);
    // Single-sided geometry viewed from behind: flip the normal, or the whole
    // back face of every leaf and curtain lights as though it faced away.
    if (!frontFacing) N = -N;
    const float3 geometricNormal = N;

    if (draw.material.w > 0.5) {
        // No tangents in the vertex format, so build a basis from screen-space
        // derivatives of position and uv. Costs a few instructions and works on any
        // mesh, including the ones in this archive that have no tangent data at all.
        const float3 dp1 = dfdx(in.worldPosition);
        const float3 dp2 = dfdy(in.worldPosition);
        const float2 duv1 = dfdx(in.uv);
        const float2 duv2 = dfdy(in.uv);

        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (abs(determinant) > 1e-12) {
            const float3 tangent = normalize((dp1 * duv2.y - dp2 * duv1.y) / determinant);
            const float3 bitangent = normalize(cross(N, tangent));
            const float3 orthoTangent = normalize(cross(bitangent, N));

            float3 sampled = normalMap.sample(smp, in.uv).xyz * 2.0 - 1.0;
            N = normalize(orthoTangent * sampled.x + bitangent * sampled.y + N * sampled.z);
        }
    }

    const float3 V = normalize(float3(frame.cameraPosition) - in.worldPosition);

    const float roughness = clamp(draw.material.x, 0.045, 1.0);
    const float metallic  = saturate(draw.material.y);

    // The metallic workflow: a dielectric reflects about 4% head-on and keeps its
    // colour in the diffuse term; a metal has no diffuse term and tints its specular.
    const float3 F0 = mix(float3(0.04), albedo, metallic);
    const float3 diffuseColor = albedo * (1.0 - metallic);

    float3 result = float3(0.0);

    // Sun, attenuated by whatever the shadow pass found in the way. The geometric
    // normal is used for the bias, not the normal-mapped one: the shadow map
    // records the geometry that was rasterised, and offsetting along a perturbed
    // normal would push the sample off the surface that actually cast it.
    const float viewDepth = dot(in.worldPosition - float3(frame.cameraPosition),
                                float3(frame.cameraForward));
    const float shadow = sampleShadow(frame, shadowMaps, shadowSampler,
                                      in.worldPosition, geometricNormal, viewDepth);

    const float3 L = normalize(float3(frame.sunDirection));
    result += shadeDirectional(N, V, L,
                               float3(frame.sunColor) * frame.sunIntensity,
                               diffuseColor, F0, roughness) * shadow;

    // Hemisphere ambient: sky above, bounced ground light below. A crude stand-in
    // for global illumination, but without something in this role the shadowed side
    // of everything goes black and the scene reads as a cut-out.
    const float hemisphere = N.y * 0.5 + 0.5;
    const float3 ambient = mix(float3(frame.groundColor), float3(frame.skyColor), hemisphere);
    result += diffuseColor * ambient * frame.ambientIntensity;

    // A cheap grazing-angle reflection of the sky, so metal and polished stone are
    // not black where no light source happens to point.
    const float fresnel = pow(saturate(1.0 - saturate(dot(N, V))), 5.0);
    result += F0 * float3(frame.skyColor) * frame.ambientIntensity * (0.25 + 0.75 * fresnel);

    result += draw.emissive.rgb;

    if (frame.cascadeDebug > 0.5) {
        // Tints each cascade so its extent and the seams between them are visible.
        const float3 tints[4] = { float3(1.0, 0.55, 0.55), float3(0.55, 1.0, 0.55),
                                  float3(0.55, 0.7, 1.0),  float3(1.0, 1.0, 0.55) };
        const float debugDepth = dot(in.worldPosition - float3(frame.cameraPosition),
                                     float3(frame.cameraForward));
        result *= tints[selectCascade(frame, debugDepth)];
    }

    // Exposure, then tone map, then encode. The order is not interchangeable: exposure and
    // tone mapping are operations on light, gamma encoding is an operation on the display.
    result = tonemapACES(result * frame.exposure);
    result = linearToSrgb(result);

    // Opacity only matters for the blended pass; the cutout pass writes 1.
    return float4(result, alphaTest ? 1.0 : (albedoSample.a * draw.baseColor.a));
}

fragment float4 mesh_fragment_shader(
        VertexOut in [[stage_in]],
        constant FrameUniforms& frame [[buffer(1)]],
        constant DrawConstants& draw  [[buffer(8)]],
        texture2d<float>       albedoMap  [[texture(0)]],
        texture2d<float>       normalMap  [[texture(1)]],
        texture2d_array<float> shadowMaps [[texture(2)]],
        sampler smp           [[sampler(0)]],
        sampler shadowSampler [[sampler(2)]],
        bool frontFacing [[front_facing]])
{
    return shadeSurface(in, frame, draw, albedoMap, normalMap, shadowMaps,
                        smp, shadowSampler, frontFacing, false);
}

fragment float4 mesh_fragment_masked(
        VertexOut in [[stage_in]],
        constant FrameUniforms& frame [[buffer(1)]],
        constant DrawConstants& draw  [[buffer(8)]],
        texture2d<float>       albedoMap  [[texture(0)]],
        texture2d<float>       normalMap  [[texture(1)]],
        texture2d_array<float> shadowMaps [[texture(2)]],
        sampler smp           [[sampler(0)]],
        sampler shadowSampler [[sampler(2)]],
        bool frontFacing [[front_facing]])
{
    return shadeSurface(in, frame, draw, albedoMap, normalMap, shadowMaps,
                        smp, shadowSampler, frontFacing, true);
}
