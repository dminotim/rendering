#version 450

// GLSL/SPIR-V port of `cube_fragment_shader` from shaders/metal/Cube.metal.
//
// Samples two image kinds that only became usable once ImageDesc grew depth and arrayLayers:
// a 3D volume texture addressed by the object-space position, and a cubemap addressed by the
// same position treated as a direction. Mixing them makes both visibly contribute.

layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    float volumeMix;
    float pad0;
    float pad1;
    float pad2;
} constants;

// Textures live in descriptor set 1; see Composite.frag. sampler3D and samplerCube are ordinary
// combined image samplers — the binding model needed no extension, only the image behind it.
layout(set = 1, binding = 0) uniform sampler3D volumeTexture;
layout(set = 1, binding = 1) uniform samplerCube environmentTexture;

layout(location = 0) in vec3 inObjectPosition;
layout(location = 0) out vec4 outColor;

void main() {
    // The cube spans -0.5..0.5 in object space, so this maps it onto the volume's 0..1 range.
    vec3 volumeCoord = inObjectPosition + 0.5;
    vec3 volume = texture(volumeTexture, volumeCoord).rgb;

    // The same position read as a direction from the cube's centre selects a cubemap face,
    // which is exactly how an environment reflection is looked up.
    vec3 environment = texture(environmentTexture, normalize(inObjectPosition)).rgb;

    outColor = vec4(mix(environment, volume, constants.volumeMix), 1.0);
}
