//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_SHADERFUNCTION_HPP
#define RENDERING_SHADERFUNCTION_HPP
namespace dmrender {

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
