#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────
// Финальный проход: читает результаты MRT-прохода из текстур и сводит их
// в изображение свопчейна.
//
// Геометрия — тот же полноэкранный квад, что и в PlaneShader, и приходит тем же
// способом: указателем на буфер, индексируемым vertex_id.
// ─────────────────────────────────────────────────────────────

struct VertexData {
    float2 position [[attribute(0)]];
};

struct CompositeUniforms {
    float viewportSize[2];
    float splitX;      // положение разделителя в пикселях
};

struct VertexOut {
    float4 clipSpacePosition [[position]];
    float2 uv;
};

vertex VertexOut composite_vertex_shader(
        const device VertexData* vertex_array [[buffer(0)]],
uint vertex_id [[vertex_id]]
) {
float2 position = vertex_array[vertex_id].position;
VertexOut out;
out.clipSpacePosition = float4(position, 0.0, 1.0);
// NDC (-1..1) в координаты текстуры (0..1). У текстур Metal начало отсчёта в верхнем
// левом углу, а в NDC +Y смотрит вверх, поэтому y инвертируется.
//
// Это единственная строка, которая обязана отличаться от GLSL-версии: в Vulkan
// ndc.y = -1 — это ВЕРХ вьюпорта, а в Metal верх — это ndc.y = +1. Подробности
// в комментарии в shaders/glsl/Composite.vert.
out.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
return out;
}

// Один источник: показываем только цель 0. Используется, когда MRT выключен.
fragment float4 composite_fragment_shader(
        VertexOut in [[stage_in]],
texture2d<float> sceneTarget [[texture(0)]],
sampler samplerState [[sampler(0)]]
) {
return sceneTarget.sample(samplerState, in.uv);
}

// Два источника: слева цель 0, справа цель 1, с тонкой разделительной линией.
fragment float4 composite_fragment_shader_mrt(
        VertexOut in [[stage_in]],
constant CompositeUniforms& uniforms [[buffer(1)]],
texture2d<float> sceneTarget [[texture(0)]],
texture2d<float> distanceTarget [[texture(1)]],
sampler samplerState [[sampler(0)]]
) {
float4 scene = sceneTarget.sample(samplerState, in.uv);
float4 dist  = distanceTarget.sample(samplerState, in.uv);

float x = in.clipSpacePosition.x;
if (abs(x - uniforms.splitX) < 1.0) {
    return float4(0.15, 0.18, 0.22, 1.0);
}
return (x < uniforms.splitX) ? scene : dist;
}
