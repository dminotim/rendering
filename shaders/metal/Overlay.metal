#include <metal_stdlib>
using namespace metal;

// ─────────────────────────────────────────────────────────────
// Полупрозрачный оверлей: сэмплит загруженную с CPU текстуру с мипмапами
// и рисует её поверх уже собранного кадра. Смешивание включено в пайплайне
// (BlendState::alphaBlend), поэтому фрагменты складываются с фоном, а не заменяют его.
// ─────────────────────────────────────────────────────────────

struct VertexData {
    float2 position [[attribute(0)]];
};

// Приходит через setPushConstants(). В Metal нет push-констант, их роль играет
// setVertexBytes:/setFragmentBytes: — данные копируются прямо в командный буфер и
// привязываются к зарезервированному слоту 8 (kPushConstantBufferSlot), которого
// не касается setUniformBuffer(). В GLSL это же объявлено как layout(push_constant).
struct OverlayUniforms {
    float centerX;
    float centerY;
    float halfWidth;
    float halfHeight;
    float opacity;
};

struct VertexOut {
    float4 clipSpacePosition [[position]];
    float2 uv;
};

vertex VertexOut overlay_vertex_shader(
        const device VertexData* vertex_array [[buffer(0)]],
constant OverlayUniforms& uniforms [[buffer(8)]],
uint vertex_id [[vertex_id]]
) {
float2 position = vertex_array[vertex_id].position;

VertexOut out;
out.clipSpacePosition = float4(
        position * float2(uniforms.halfWidth, uniforms.halfHeight) +
        float2(uniforms.centerX, uniforms.centerY),
        0.0, 1.0);
// y инвертируется, потому что в Metal верх экрана — это ndc.y = +1, а v = 0 у текстур
// это верхняя строка. Подробности в shaders/glsl/Composite.vert.
out.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
return out;
}

fragment float4 overlay_fragment_shader(
        VertexOut in [[stage_in]],
constant OverlayUniforms& uniforms [[buffer(8)]],
texture2d<float> overlayTexture [[texture(0)]],
sampler samplerState [[sampler(0)]]
) {
// Оверлей рисуется меньше, чем хранится текстура, поэтому сэмплер выбирает младший
// мип-уровень. Без мипмапов мелкая клетка превратилась бы в муар.
float4 texel = overlayTexture.sample(samplerState, in.uv);
return float4(texel.rgb, texel.a * uniforms.opacity);
}
