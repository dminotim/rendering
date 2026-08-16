//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_SHADERFUNCTION_HPP
#define RENDERING_SHADERFUNCTION_HPP
namespace dmrender {

    /**
     * @page shader_conventions Coordinate conventions for shaders
     *
     * A shader written against this library sees one set of conventions regardless of which
     * backend compiles it, so an MSL shader and its GLSL counterpart can be literal
     * translations of each other. Where the underlying APIs disagree, the backend adapts.
     *
     * @section clip Clip space: +X right, +Y up, Z in [0, 1]
     *
     * This is Metal's convention. Vulkan's native clip space has +Y pointing *down*, so the
     * Vulkan backend renders with a negative-height viewport to match. Without that, any shader
     * computing a position in clip space — placing a quad, billboarding, projecting a point —
     * would render mirrored vertically on one backend, and the periodic or symmetric content
     * that such code often draws hides the mistake until something asymmetric appears.
     *
     * @section frag Fragment position: pixels, origin top-left
     *
     * `gl_FragCoord` and Metal's `[[position]]` in a fragment function already agree: both are
     * framebuffer coordinates in pixels with the origin at the top-left corner and samples taken
     * at pixel centres. Nothing is adjusted, and shaders that read them need no care.
     *
     * @section tex Texture coordinates: (0, 0) is the top-left texel
     *
     * True on both APIs. Combined with clip-space +Y being up, converting a clip-space position
     * into a texture coordinate always inverts Y:
     *
     *     uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
     *
     * @section winding Winding: identical on both
     *
     * Because clip space matches, identical vertices produce identical framebuffer positions, so
     * `FrontFace::CounterClockwise` selects the same triangles on both backends and CullMode
     * behaves consistently.
     *
     * @section pushc Push constants are the one construct that differs in source
     *
     * GLSL declares `layout(push_constant) uniform Block { ... };` while MSL takes an ordinary
     * `constant Block&` at buffer index @c kPushConstantBufferSlot, because Metal has no push
     * constants. The C++ call, `CommandBuffer::setPushConstants()`, is identical.
     *
     * @section slots Buffer slots: the type is declared, not inferred
     *
     * A buffer slot carries either a uniform block or a storage buffer, and which one is fixed by
     * PipelineDesc::bufferSlots — it cannot be worked out from what happens to be bound at draw
     * time, because Vulkan bakes each binding's descriptor type into the pipeline layout the
     * shader is compiled against.
     *
     * The shader must declare the same thing:
     *
     * | Slot type | GLSL | MSL |
     * |---|---|---|
     * | Uniform | `layout(std140, set=0, binding=n) uniform B { … }` | `constant T& b [[buffer(n)]]` |
     * | Storage | `layout(std430, set=0, binding=n) readonly buffer B { T v[]; }` | `const device T* b [[buffer(n)]]` |
     *
     * Textures are a separate numbering space and always combined image samplers:
     * `layout(set=1, binding=n) uniform sampler2D` in GLSL, `texture2d<float> [[texture(n)]]` plus
     * `sampler [[sampler(n)]]` with the same index in MSL.
     *
     * @section layout Struct layout is the caller's responsibility
     *
     * A `vec3` inside an array occupies 16 bytes, not 12, under both std140/std430 and MSL's
     * rules — a C++ struct of three `float[3]` members will not match a shader struct of three
     * `float3` members. Either pad explicitly to a multiple of 16, or use only scalar members
     * (which align to 4 and therefore pack tightly), or use MSL's `packed_*` types. Whichever you
     * choose, assert the size: `static_assert(sizeof(Vertex) == 48, "")` catches at compile time
     * what otherwise shows up as sheared or exploded geometry.
     */

    /**
     * @class ShaderFunction
     * @brief An abstract interface for a single, compiled shader function.
     *
     * This class represents a programmable stage of the graphics pipeline, such as a
     * vertex shader or a fragment (pixel) shader. It is typically created by compiling
     * shader source code and referencing a specific entry point function within that code.
     */
    class ShaderFunction {
    public:
        virtual ~ShaderFunction() = default;

        /**
         * @brief Retrieves the native, backend-specific handle for the compiled shader function.
         *
         * This handle can be used to construct a Pipeline state object.
         *
         * @return A void pointer to the native object (e.g., id<MTLFunction>, VkShaderModule).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the name of the entry point function for this shader.
         *
         * This is the function name specified when the shader was compiled (e.g., "vertex_main").
         *
         * @return A C-style string representing the entry point name.
         *         The returned pointer is valid for the lifetime of the ShaderFunction object.
         */
        virtual const char* entryPoint() const = 0;
    };
}
#endif //RENDERING_SHADERFUNCTION_HPP
