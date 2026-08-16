#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────
// Draws a loaded model: one directional light plus a hemisphere ambient term.
// Geometry is read from a buffer indexed by vertex id — the same model used
// everywhere else in this project.
//
// The struct below must match MeshVertex in mesh/Mesh.hpp byte for byte, hence
// the explicit padding: a vec3 inside an array occupies 16 bytes, not 12.
// ─────────────────────────────────────────────────────────────

struct MeshVertex {
    packed_float3 position;
    float pad0;
    packed_float3 normal;
    float pad1;
    packed_float2 uv;
    packed_float2 pad2;
};

struct SceneConstants {
    float4x4 modelViewProjection;
    float4 lightDirection;   // xyz: direction towards the light
    float4 baseColor;        // rgb: material colour, a: 1 when a texture is bound
    float4 cameraParams;     // x: ambient amount
};

struct VertexOut {
    float4 clipSpacePosition [[position]];
    float3 normal;
    float2 uv;
};

vertex VertexOut mesh_vertex_shader(
        const device MeshVertex* vertex_array [[buffer(0)]],
constant SceneConstants& constants [[buffer(8)]],
uint vertex_id [[vertex_id]]
) {
// `vertex` is a Metal function qualifier and cannot name a local, so this is `v`.
// GLSL has no such restriction, but Mesh.vert uses the same name to keep the two readable
// side by side.
MeshVertex v = vertex_array[vertex_id];

VertexOut out;
out.clipSpacePosition = constants.modelViewProjection * float4(float3(v.position), 1.0);
// The model only rotates and scales uniformly, so the normal passes through unchanged;
// an arbitrary transform would need an inverse-transpose matrix.
out.normal = float3(v.normal);
out.uv = float2(v.uv);
return out;
}

fragment float4 mesh_fragment_shader(
        VertexOut in [[stage_in]],
constant SceneConstants& constants [[buffer(8)]],
texture2d<float> albedoTexture [[texture(0)]],
sampler samplerState [[sampler(0)]]
) {
float3 normal = normalize(in.normal);
float3 lightDirection = normalize(constants.lightDirection.xyz);

// Hemisphere ambient: cooler from above, warmer from below. Stops the shadowed side
// dropping to black.
float hemisphere = normal.y * 0.5 + 0.5;
float3 ambient = mix(float3(0.16, 0.15, 0.20), float3(0.34, 0.36, 0.42), hemisphere);

float diffuse = max(dot(normal, lightDirection), 0.0);

// A soft Blinn-Phong specular term with the view direction approximated by the Z axis.
float3 halfway = normalize(lightDirection + float3(0.0, 0.0, 1.0));
float specular = pow(max(dot(normal, halfway), 0.0), 48.0) * 0.25;

float3 albedo = constants.baseColor.rgb;
if (constants.baseColor.a > 0.5) {
    albedo *= albedoTexture.sample(samplerState, in.uv).rgb;
}

float3 lit = albedo * (ambient * constants.cameraParams.x + diffuse) + float3(specular);
return float4(lit, 1.0);
}
