#version 450

// GLSL/SPIR-V port of `mesh_fragment_shader` / `mesh_fragment_masked` from
// shaders/metal/Mesh.metal.
//
// Compiled twice: once as-is for opaque and blended geometry, and once with ALPHA_TEST defined
// for masked geometry. One source rather than two, because the only difference is four lines and
// keeping them in sync by hand is exactly the sort of thing that quietly diverges.

const float PI = 3.14159265359;

const int kCascadeCount = 4;

layout(std140, set = 0, binding = 1) uniform FrameUniforms {
    mat4 viewProjection;
    vec3 cameraPosition;   float exposure;
    vec3 sunDirection;     float sunIntensity;
    vec3 sunColor;         float ambientIntensity;
    vec3 skyColor;         float shadowSoftness;
    vec3 groundColor;      float shadowNormalBias;
    vec3 cameraForward;    float shadowConstantBias;
    vec4 cascadeSplit;              // view depth at which each cascade ends
    vec4 cascadeTexelWorldSize;     // world units covered by one shadow texel
    vec4 cascadeDepthRange;         // world units spanned by each cascade's depth axis
    mat4 cascadeMatrix[kCascadeCount];
    float shadowStrength;
    float shadowEnabled;
    float cascadeDebug;
    float pad;
} frame;

// Per-draw data, 48 bytes of the guaranteed 128.
layout(push_constant) uniform DrawConstants {
    vec4 baseColor;   // rgb material colour, a opacity
    vec4 material;    // x roughness, y metallic, z alphaCutoff, w hasNormalMap
    vec4 emissive;
} draw;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2DArray shadowMaps;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 outColor;

// ── BRDF ────────────────────────────────────────────────────────────────────

// D: how many microfacets point along the half vector. GGX has a long tail, which is what gives
// a bright core with a soft halo instead of Phong's hard blob.
float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// V: Smith height-correlated masking, already divided by 4(n·l)(n·v). Folding the denominator in
// removes a division and the numerical blow-up as NdotV approaches zero.
float visibilitySmith(float NdotV, float NdotL, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float v = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float l = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-7);
}

// F: Schlick. Reflectance rises to 1 at grazing angles for every material — this is why a wet
// street is mirror-like in the distance and dull underfoot.
vec3 fresnelSchlick(float VdotH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// ── Display mapping ─────────────────────────────────────────────────────────
// The swapchain is BGRA8_UNORM rather than an sRGB format, so the encode happens here. That is
// a deliberate choice: ImGui writes colours that are already in sRGB, and an sRGB framebuffer
// would encode them a second time and wash the interface out.

// ACES approximation (Narkowicz). Compact, and its shoulder keeps highlights from flattening
// into white the way a plain Reinhard curve does.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92,
               lessThanEqual(c, vec3(0.0031308)));
}

// ── Shadows ─────────────────────────────────────────────────────────────────

// Which cascade covers this fragment. Compared against distance along the view axis rather than
// distance to the eye, so the boundaries are planes parallel to the screen and do not bulge at
// the corners of the frame.
int selectCascade(float viewDepth) {
    for (int i = 0; i < kCascadeCount - 1; ++i) {
        if (viewDepth < frame.cascadeSplit[i]) return i;
    }
    return kCascadeCount - 1;
}

/**
 * Returns 1 where the sun reaches, 0 where it does not.
 *
 * The bias is applied by moving the sample point along the surface normal rather than by adding
 * a constant to the comparison. A shadow texel covers an area of surface, and on a slope the
 * distances across that area differ — which is what produces the striped self-shadowing known as
 * shadow acne. The size of that spread depends on the angle, so the correction has to as well. A
 * constant large enough to cure the worst slope detaches every shadow from its object; moving
 * along the normal by about one texel's width cures the slope and cancels itself where the
 * surface faces the light head-on.
 */
float sampleShadow(vec3 worldPosition, vec3 N, float viewDepth) {
    if (frame.shadowEnabled < 0.5) return 1.0;

    int cascade = selectCascade(viewDepth);
    float texelWorldSize = frame.cascadeTexelWorldSize[cascade];

    vec3 offsetPosition = worldPosition + N * (texelWorldSize * frame.shadowNormalBias);
    vec4 lightClip = frame.cascadeMatrix[cascade] * vec4(offsetPosition, 1.0);

    // Orthographic, so w is 1; the divide is kept because it costs nothing and keeps the code
    // correct if a perspective light is ever added.
    vec3 ndc = lightClip.xyz / lightClip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    // NDC to texture coordinates. Y inverts: clip space points up, texture space down.
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;

    float currentDistance = ndc.z * frame.cascadeDepthRange[cascade];

    // Percentage-closer filtering. Each tap is compared on its own and the *results* are
    // averaged — never the distances. Averaging two distances from either side of a silhouette
    // yields a value no surface has, and comparing against it is meaningless. This is also why
    // the sampler must be Nearest: linear filtering would do exactly that averaging in hardware.
    vec2 texelStep = 1.0 / vec2(textureSize(shadowMaps, 0).xy);
    float radius = frame.shadowSoftness;

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(shadowMaps,
                                     vec3(uv + vec2(x, y) * texelStep * radius, cascade)).r;
            lit += (currentDistance - frame.shadowConstantBias <= occluder) ? 1.0 : 0.0;
        }
    }
    lit /= 9.0;

    // Fade the last cascade out at its far edge instead of ending abruptly.
    float fadeStart = frame.cascadeSplit[kCascadeCount - 1] * 0.93;
    float fade = clamp((viewDepth - fadeStart) /
                       max(frame.cascadeSplit[kCascadeCount - 1] - fadeStart, 1e-4), 0.0, 1.0);
    lit = mix(lit, 1.0, fade);

    return mix(1.0, lit, frame.shadowStrength);
}

vec3 shadeDirectional(vec3 N, vec3 V, vec3 L, vec3 radiance,
                      vec3 diffuseColor, vec3 F0, float roughness) {
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    if (NdotL <= 0.0) return vec3(0.0);   // light below the horizon: nothing to compute

    vec3  H = normalize(V + L);
    float NdotV = clamp(dot(N, V), 0.0, 1.0) + 1e-5;   // avoids a bright rim on the silhouette
    float NdotH = clamp(dot(N, H), 0.0, 1.0);
    float VdotH = clamp(dot(V, H), 0.0, 1.0);

    float D   = distributionGGX(NdotH, roughness);
    float Vis = visibilitySmith(NdotV, NdotL, roughness);
    vec3  F   = fresnelSchlick(VdotH, F0);

    // What reflected specularly cannot also reflect diffusely.
    return ((1.0 - F) * diffuseColor / PI + D * Vis * F) * radiance * NdotL;
}

void main() {
    vec4 albedoSample = texture(albedoMap, inUv);

#ifdef ALPHA_TEST
    // First thing in the shader: a discarded fragment should not pay for anything after it.
    // The cutoff comes from the material rather than being hardcoded, so foliage and wire mesh
    // can be tuned separately.
    if (albedoSample.a < draw.material.z) discard;
#endif

    // The albedo texture is in an sRGB format, so the hardware already decoded it to linear —
    // before filtering, which is where a shader-side pow() gets it subtly wrong.
    vec3 albedo = albedoSample.rgb * draw.baseColor.rgb;

    vec3 N = normalize(inWorldNormal);
    // Single-sided geometry seen from behind: flip the normal, or the back of every leaf and
    // curtain lights as though it faced away.
    if (!gl_FrontFacing) N = -N;
    vec3 geometricNormal = N;

    if (draw.material.w > 0.5) {
        // No tangents in the vertex format, so build a basis from screen-space derivatives of
        // position and uv. Costs a few instructions and works on any mesh, including the ones in
        // this archive that carry no tangent data at all.
        vec3 dp1 = dFdx(inWorldPosition);
        vec3 dp2 = dFdy(inWorldPosition);
        vec2 duv1 = dFdx(inUv);
        vec2 duv2 = dFdy(inUv);

        float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (abs(det) > 1e-12) {
            vec3 tangent = normalize((dp1 * duv2.y - dp2 * duv1.y) / det);
            vec3 bitangent = normalize(cross(N, tangent));
            vec3 orthoTangent = normalize(cross(bitangent, N));

            vec3 sampled = texture(normalMap, inUv).xyz * 2.0 - 1.0;
            N = normalize(orthoTangent * sampled.x + bitangent * sampled.y + N * sampled.z);
        }
    }

    vec3 V = normalize(frame.cameraPosition - inWorldPosition);

    float roughness = clamp(draw.material.x, 0.045, 1.0);
    float metallic  = clamp(draw.material.y, 0.0, 1.0);

    // The metallic workflow: a dielectric reflects about 4% head-on and keeps its colour in the
    // diffuse term; a metal has no diffuse term and tints its specular instead.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseColor = albedo * (1.0 - metallic);

    vec3 result = vec3(0.0);

    // Sun, attenuated by whatever the shadow pass found in the way. The geometric normal is used
    // for the bias, not the normal-mapped one: the shadow map records the geometry that was
    // rasterised, and offsetting along a perturbed normal would push the sample off the surface
    // that actually cast it.
    float viewDepth = dot(inWorldPosition - frame.cameraPosition, frame.cameraForward);
    float shadow = sampleShadow(inWorldPosition, geometricNormal, viewDepth);

    vec3 L = normalize(frame.sunDirection);
    result += shadeDirectional(N, V, L, frame.sunColor * frame.sunIntensity,
                               diffuseColor, F0, roughness) * shadow;

    // Hemisphere ambient: sky above, bounced ground light below. A crude stand-in for global
    // illumination, but without something in this role the shadowed side of everything goes
    // black and the scene reads as a cut-out.
    float hemisphere = N.y * 0.5 + 0.5;
    vec3 ambient = mix(frame.groundColor, frame.skyColor, hemisphere);
    result += diffuseColor * ambient * frame.ambientIntensity;

    // A cheap grazing-angle reflection of the sky, so metal and polished stone are not black
    // wherever no light source happens to point.
    float fresnel = pow(clamp(1.0 - clamp(dot(N, V), 0.0, 1.0), 0.0, 1.0), 5.0);
    result += F0 * frame.skyColor * frame.ambientIntensity * (0.25 + 0.75 * fresnel);

    result += draw.emissive.rgb;

    if (frame.cascadeDebug > 0.5) {
        // Tints each cascade so its extent and the seams between them are visible.
        vec3 tints[4] = vec3[4](vec3(1.0, 0.55, 0.55), vec3(0.55, 1.0, 0.55),
                                vec3(0.55, 0.7, 1.0),  vec3(1.0, 1.0, 0.55));
        float debugDepth = dot(inWorldPosition - frame.cameraPosition, frame.cameraForward);
        result *= tints[selectCascade(debugDepth)];
    }

    // Exposure, then tone map, then encode. The order is not interchangeable: exposure and tone
    // mapping are operations on light, gamma encoding is an operation on the display.
    result = tonemapACES(result * frame.exposure);
    result = linearToSrgb(result);

#ifdef ALPHA_TEST
    outColor = vec4(result, 1.0);
#else
    outColor = vec4(result, albedoSample.a * draw.baseColor.a);
#endif
}
