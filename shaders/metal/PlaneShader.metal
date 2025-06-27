#include <metal_stdlib>
using namespace metal;

// Структура вершинных данных остается той же
struct VertexData {
    float2 position [[attribute(0)]];
};

// Uniform-структура остается той же
struct Uniforms {
    float viewportSize[2];
    float scale;
    float pan[2];
};

// Структура для передачи данных из вершинного во фрагментный
struct VertexOut {
    float4 clipSpacePosition [[position]]; // <-- Это и есть то, что нам нужно!
};

// Вершинный шейдер становится ОЧЕНЬ простым
vertex VertexOut plane_vertex_shader(
        const device VertexData* vertex_array [[buffer(0)]],
uint vertex_id [[vertex_id]]
) {
VertexOut out;
// Просто передаем позицию из буфера (которая уже в NDC, -1..1)
// в clip space. Никаких матриц не нужно для полноэкранного прямоугольника.
out.clipSpacePosition = float4(vertex_array[vertex_id].position, 0.0, 1.0);
return out;
}

// Фрагментный шейдер теперь использует `in.clipSpacePosition`
fragment float4 plane_fragment_shader(
        VertexOut in [[stage_in]],
constant Uniforms& uniforms [[buffer(1)]]
) {
float2 screen = in.clipSpacePosition.xy;
float2 world = screen - float2(uniforms.pan[0], uniforms.pan[1]);
const float   cellSizePx   = 50.0;
const float   lineWidthPx  = 2.0;

float worldCellSize  = cellSizePx  * uniforms.scale;   // в «мире»
float worldLineWidth = lineWidthPx;   // в «мире»
float2 cell   = fract(world / worldCellSize) * worldCellSize;

bool isLine =
        (cell.x < worldLineWidth) || (cell.y < worldLineWidth);

constexpr float4 lineColor  = float4(0.60, 0.75, 0.95, 1.0);
constexpr float4 paperColor = float4(0.98, 0.98, 0.96, 1.0);

return isLine ? lineColor : paperColor;
}