#version 450

// GLSL/SPIR-V port of `mesh_fragment_shader` from shaders/metal/Mesh.metal.
//
// Simple directional lighting with a hemisphere ambient term: enough for the model's shape to
// read clearly, and little enough not to distract from what is being demonstrated, which is
// geometry loading.

layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    vec4 lightDirection;
    vec4 baseColor;
    vec4 cameraParams;
} constants;

// Textures live in descriptor set 1; see the conventions in ShaderFunction.hpp.
layout(set = 1, binding = 0) uniform sampler2D albedoTexture;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal = normalize(inNormal);
    vec3 lightDirection = normalize(constants.lightDirection.xyz);

    // Hemisphere ambient: cooler from above, warmer from below. It stops the shadowed side
    // dropping to black, which with a single light would read as flat.
    float hemisphere = normal.y * 0.5 + 0.5;
    vec3 ambient = mix(vec3(0.16, 0.15, 0.20), vec3(0.34, 0.36, 0.42), hemisphere);

    float diffuse = max(dot(normal, lightDirection), 0.0);

    // A soft Blinn-Phong specular term with the view direction approximated by the Z axis.
    // A precise camera vector would add nothing visible here.
    vec3 halfway = normalize(lightDirection + vec3(0.0, 0.0, 1.0));
    float specular = pow(max(dot(normal, halfway), 0.0), 48.0) * 0.25;

    vec3 albedo = constants.baseColor.rgb;
    if (constants.baseColor.a > 0.5) {
        albedo *= texture(albedoTexture, inUv).rgb;
    }

    vec3 lit = albedo * (ambient * constants.cameraParams.x + diffuse) + vec3(specular);
    outColor = vec4(lit, 1.0);
}
