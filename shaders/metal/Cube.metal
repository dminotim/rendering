#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────
// Первый по-настоящему трёхмерный шейдер в проекте.
//
// Матрица MVP приходит через push-константы (в Metal — setVertexBytes: по слоту 8),
// а фрагментной стадии передаётся позиция в объектном пространстве: она же служит
// координатой для объёмной текстуры и направлением для кубической карты.
// ─────────────────────────────────────────────────────────────

struct VertexData {
    // Выравнивание до 16 байт: в std430 массив vec3 имеет шаг 16, и C++-сторона
    // добивает вершину так же, поэтому раскладки совпадают побайтово.
    packed_float3 position;
    float pad;
};

struct SceneConstants {
    float4x4 modelViewProjection;
    float volumeMix;
    float pad0;
    float pad1;
    float pad2;
};

struct VertexOut {
    float4 clipSpacePosition [[position]];
    float3 objectPosition;
};

// Смещения экземпляров выбираются по instance_id. Непрямая отрисовка выдаёт много
// экземпляров из одной команды, поэтому всё, что меняется от экземпляра к экземпляру,
// обязано лежать в буфере: CPU не получает шанса подменить push-константы между ними.
vertex VertexOut cube_vertex_shader(
        const device VertexData* vertex_array [[buffer(0)]],
constant float4* instanceOffsets [[buffer(1)]],
constant SceneConstants& constants [[buffer(8)]],
uint vertex_id [[vertex_id]],
uint instance_id [[instance_id]]
) {
float3 position = float3(vertex_array[vertex_id].position);
float3 offset = instanceOffsets[instance_id].xyz;

VertexOut out;
out.clipSpacePosition = constants.modelViewProjection * float4(position + offset, 1.0);
// Объектное пространство намеренно без смещения экземпляра: каждый куб сэмплит объём
// и кубическую карту по своему полному диапазону, а не по срезу общего.
out.objectPosition = position;
return out;
}

fragment float4 cube_fragment_shader(
        VertexOut in [[stage_in]],
constant SceneConstants& constants [[buffer(8)]],
texture3d<float> volumeTexture [[texture(0)]],
texturecube<float> environmentTexture [[texture(1)]],
sampler samplerState [[sampler(0)]]
) {
// Куб занимает -0.5..0.5 в объектном пространстве — переводим в 0..1 объёма.
float3 volumeCoord = in.objectPosition + 0.5;
float3 volume = volumeTexture.sample(samplerState, volumeCoord).rgb;

// Та же позиция, прочитанная как направление из центра куба, выбирает грань
// кубической карты — ровно так ищется отражение окружения.
float3 environment = environmentTexture.sample(samplerState, normalize(in.objectPosition)).rgb;

return float4(mix(environment, volume, constants.volumeMix), 1.0);
}
