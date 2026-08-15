#version 450

// GLSL/SPIR-V port of `plane_fragment_shader` from shaders/metal/PlaneShader.metal.
//
// The Metal shader reads `in.clipSpacePosition.xy`, which in a fragment function is not clip
// space at all but the framebuffer position in pixels, with the origin at the top-left corner
// and samples taken at pixel centres. gl_FragCoord.xy has exactly those semantics in Vulkan,
// so the grid lands on the same pixels on both backends.

// The C++ side declares:
//
//     struct Uniforms { float viewportSize[2]; float scale; float pan[2]; };
//
// which is five tightly packed floats at offsets 0, 4, 8, 12 and 16. Declaring the block as
// five scalar floats reproduces that layout under std140, where a scalar float has an alignment
// of 4. Writing `vec2 viewportSize; float scale; vec2 pan;` instead would push `pan` to offset
// 16 (vec2 aligns to 8) and silently read the wrong values.
layout(std140, set = 0, binding = 1) uniform Uniforms {
    float viewportSizeX;
    float viewportSizeY;
    float scale;
    float panX;
    float panY;
} uniforms;

layout(location = 0) out vec4 outColor;

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

    outColor = isLine ? lineColor : paperColor;
}
