#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────
// Отрисовка загруженной модели: направленный источник плюс полусферическое
// окружение. Геометрия читается из буфера по индексу вершины — той же моделью,
// что и в остальном проекте.
//
// Структура ниже обязана совпадать с MeshVertex из mesh/Mesh.hpp побайтово,
// отсюда явные поля выравнивания: vec3 в массиве занимает 16 байт, а не 12.
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
    float4 lightDirection;   // xyz: направление на источник
    float4 baseColor;        // rgb: цвет материала, a: 1 если есть текстура
    float4 cameraParams;     // x: аммбиентная доля
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
MeshVertex vertex = vertex_array[vertex_id];

VertexOut out;
out.clipSpacePosition = constants.modelViewProjection * float4(float3(vertex.position), 1.0);
// Модель только вращается и масштабируется равномерно, поэтому нормаль передаётся
// как есть; для произвольного преобразования понадобилась бы обратная транспонированная.
out.normal = float3(vertex.normal);
out.uv = float2(vertex.uv);
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

// Полусферическое окружение: сверху холоднее, снизу теплее. Не даёт теневой стороне
// провалиться в чёрное.
float hemisphere = normal.y * 0.5 + 0.5;
float3 ambient = mix(float3(0.16, 0.15, 0.20), float3(0.34, 0.36, 0.42), hemisphere);

float diffuse = max(dot(normal, lightDirection), 0.0);

float3 halfway = normalize(lightDirection + float3(0.0, 0.0, 1.0));
float specular = pow(max(dot(normal, halfway), 0.0), 48.0) * 0.25;

float3 albedo = constants.baseColor.rgb;
if (constants.baseColor.a > 0.5) {
    albedo *= albedoTexture.sample(samplerState, in.uv).rgb;
}

float3 lit = albedo * (ambient * constants.cameraParams.x + diffuse) + float3(specular);
return float4(lit, 1.0);
}
