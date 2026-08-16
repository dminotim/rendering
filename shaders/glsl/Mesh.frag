#version 450

// GLSL/SPIR-V port of `mesh_fragment_shader` from shaders/metal/Mesh.metal.
//
// Простое направленное освещение с полусферическим окружением: достаточно, чтобы форма модели
// читалась, и достаточно мало, чтобы не отвлекать от того, что демонстрируется — загрузку
// геометрии.

layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    vec4 lightDirection;
    vec4 baseColor;
    vec4 cameraParams;
} constants;

// Текстуры живут в наборе 1; см. соглашения в ShaderFunction.hpp.
layout(set = 1, binding = 0) uniform sampler2D albedoTexture;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal = normalize(inNormal);
    vec3 lightDirection = normalize(constants.lightDirection.xyz);

    // Полусферический окружающий свет: сверху холоднее, снизу теплее. Он не даёт теневой
    // стороне провалиться в чёрное, что при одном источнике выглядело бы плоско.
    float hemisphere = normal.y * 0.5 + 0.5;
    vec3 ambient = mix(vec3(0.16, 0.15, 0.20), vec3(0.34, 0.36, 0.42), hemisphere);

    float diffuse = max(dot(normal, lightDirection), 0.0);

    // Мягкий бликовый член по модели Блинна — Фонга, с направлением взгляда, приближённым
    // осью Z. Точная камера здесь ничего бы не добавила.
    vec3 halfway = normalize(lightDirection + vec3(0.0, 0.0, 1.0));
    float specular = pow(max(dot(normal, halfway), 0.0), 48.0) * 0.25;

    vec3 albedo = constants.baseColor.rgb;
    if (constants.baseColor.a > 0.5) {
        albedo *= texture(albedoTexture, inUv).rgb;
    }

    vec3 lit = albedo * (ambient * constants.cameraParams.x + diffuse) + vec3(specular);
    outColor = vec4(lit, 1.0);
}
