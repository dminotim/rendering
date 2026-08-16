#version 450

// GLSL/SPIR-V port of `mesh_vertex_shader` from shaders/metal/Mesh.metal.
//
// Geometry comes from a read-only storage buffer indexed by gl_VertexIndex rather than through
// vertex attributes — the same model the rest of this project uses. The struct below must match
// MeshVertex in mesh/Mesh.hpp byte for byte, which is why it carries explicit padding: a vec3
// inside a std430 array occupies 16 bytes, not 12.

struct MeshVertex {
    vec3 position;
    float pad0;
    vec3 normal;
    float pad1;
    vec2 uv;
    vec2 pad2;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    MeshVertex vertices[];
} vertexBuffer;

// 64 bytes of matrix + 64 of parameters = the full 128 bytes push constants guarantee.
layout(push_constant) uniform SceneConstants {
    mat4 modelViewProjection;
    vec4 lightDirection;    // xyz: направление на источник, w не используется
    vec4 baseColor;         // rgb: цвет материала, a: 1 если есть текстура
    vec4 cameraParams;      // x: аммбиентная доля, yzw свободны
} constants;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUv;

void main() {
    MeshVertex vertex = vertexBuffer.vertices[gl_VertexIndex];

    gl_Position = constants.modelViewProjection * vec4(vertex.position, 1.0);

    // Модель вращается только вокруг своей оси и не масштабируется неравномерно, поэтому
    // нормаль можно передать как есть — обратная транспонированная матрица здесь совпала бы
    // с обычной. Для произвольного преобразования её пришлось бы считать отдельно.
    outNormal = vertex.normal;
    outUv = vertex.uv;
}
