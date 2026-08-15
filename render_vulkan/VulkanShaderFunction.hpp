//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANSHADERFUNCTION_HPP
#define RENDERING_VULKANSHADERFUNCTION_HPP

#include <vulkan/vulkan.h>

#include <filesystem>
#include <memory>
#include <string>

#include "ShaderFunction.hpp"

namespace dmrender {

    class Device;
    struct VulkanShaderFunctionNativeData;

    /**
     * @class VulkanShaderFunction
     * @brief A VkShaderModule loaded from a pre-compiled SPIR-V blob.
     *
     * Metal compiles shader source at runtime and then picks one named function out of the
     * resulting MTLLibrary. SPIR-V has no equivalent runtime compiler in core Vulkan, so the
     * build compiles each entry point ahead of time and this class only has to find the right
     * artifact. The naming convention keeps the *call site identical on both backends*:
     *
     *     createShaderFunction(device, SHADER_DIR/"PlaneShader", "plane_vertex_shader")
     *       Metal  -> compiles SHADER_DIR/PlaneShader.metal, takes function plane_vertex_shader
     *       Vulkan -> loads   SHADER_DIR/PlaneShader.plane_vertex_shader.spv
     *
     * The SPIR-V entry point itself is always `main`, which is what GLSL produces.
     */
    class VulkanShaderFunction : public ShaderFunction {
    public:
        VulkanShaderFunction(const std::shared_ptr<Device>& device,
                             const std::filesystem::path& pathToShaderFile,
                             const std::string& functionName);

        ~VulkanShaderFunction() override;

        /// @return Pointer to the VkShaderModule handle.
        void* nativeHandle() const override;

        /// @return The SPIR-V entry point name, always "main".
        const char* entryPoint() const override;

        VkShaderModule shaderModule() const;

        /// @brief The logical function name this module was requested with, kept for diagnostics.
        const std::string& functionName() const;

    private:
        std::unique_ptr<VulkanShaderFunctionNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANSHADERFUNCTION_HPP
