#version 450

// GLSL/SPIR-V port of `plane_fragment_shader_mrt` from shaders/metal/PlaneShader.metal.
//
// The MRT variant of the plane shader: it writes two colour attachments in one pass.
// `layout(location = i) out` is the direct equivalent of Metal's `[[color(i)]]`, and the
// number of outputs must match both the render pass's attachment count and the number of
// entries in the pipeline's RenderTargetFormat::colorFormats.
//
// See PlaneShader.frag for why the uniform block is declared as five scalar floats.

layout(std140, set = 0, binding = 1) uniform Uniforms {
    float viewportSizeX;
    float viewportSizeY;
    float scale;
    float panX;
    float panY;
} uniforms;

layout(location = 0) out vec4 outGrid;
layout(location = 1) out vec4 outDistance;

void main() {
    vec2 screen = gl_FragCoord.xy;
    vec2 world = screen - vec2(uniforms.panX, uniforms.panY);

    const float cellSizePx = 50.0;
    const float lineWidthPx = 2.0;

    float worldCellSize = cellSizePx * uniforms.scale;
    float worldLineWidth = lineWidthPx;
    vec2 cell = fract(world / worldCellSize) * worldCellSize;

    bool isLine = (cell.x < worldLineWidth) || (cell.y < worldLineWidth);

    const vec4 lineColor = vec4(0.60, 0.75, 0.95, 1.0);
    const vec4 paperColor = vec4(0.98, 0.98, 0.96, 1.0);

    // Normalised distance to the nearest grid line: 0 on a line, 1 at a cell centre.
    vec2 toEdge = min(cell, vec2(worldCellSize) - cell);
    float nearest = min(toEdge.x, toEdge.y) / max(worldCellSize * 0.5, 1e-4);

    outGrid = isLine ? lineColor : paperColor;
    outDistance = vec4(nearest, 1.0 - nearest, 0.35, 1.0);
}
