//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANPIPELINE_HPP
#define RENDERING_VULKANPIPELINE_HPP

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

#include "Pipeline.hpp"
#include "ShaderFunction.hpp"

namespace dmrender {

    class Device;
    struct VulkanPipelineNativeData;

    /**
     * @class VulkanPipeline
     * @brief VkPipeline built from the same inputs MTLRenderPipelineDescriptor takes.
     *
     * @par Why there is no vertex input state
     * The abstract `createPipeline()` receives shaders and render target formats only — no vertex
     * layout — because Metal does not need one here: `plane_vertex_shader` reads its geometry
     * straight out of `const device VertexData* [[buffer(0)]]` indexed by `vertex_id`. The GLSL
     * shaders mirror that exactly: geometry comes from a read-only storage buffer indexed by
     * `gl_VertexIndex`, so the pipeline declares zero vertex bindings and zero attributes and the
     * two backends stay byte-for-byte equivalent without widening the public API.
     *
     * @par Binding model
     * A slot number in `CommandBuffer::setVertexBuffer()` / `setUniformBuffer()` / `setTexture()`
     * *is* the Vulkan binding number, the same way it is the Metal `[[buffer(n)]]` or
     * `[[texture(n)]]` index. Because Metal numbers buffers and textures independently, the two
     * live in separate descriptor sets here:
     *
     *   set 0, binding 0        : storage buffer, vertex stage    <- setVertexBuffer(0, ...)
     *   set 0, binding 1 .. N-1 : uniform buffer, vertex+fragment <- setUniformBuffer(n, ...)
     *   set 1, binding 0 .. N-1 : combined image sampler          <- setTexture(n, ...)
     *
     * Every slot is declared in the layout whether or not a shader uses it, so one layout serves
     * any shader pair that respects the convention. Unused bindings simply go unwritten, and a
     * set with nothing bound is never allocated.
     *
     * @par Multiple render targets
     * `RenderTargetFormat::colorFormats` drives both the blend state (Vulkan wants one entry per
     * attachment) and the render pass the pipeline is validated against. A pipeline built for two
     * colour formats can only run inside a render pass with two colour attachments, and its
     * fragment shader must write exactly two outputs.
     *
     * @par Fixed state
     * The remaining state is pinned to Metal's defaults so both backends rasterise identically:
     * triangle list, filled polygons, no culling, blending disabled, full write mask, and
     * viewport/scissor left dynamic so a resize needs no pipeline rebuild.
     */
    class VulkanPipeline : public Pipeline {
    public:
        VulkanPipeline(const std::shared_ptr<Device>& device,
                       const std::shared_ptr<ShaderFunction>& vertexFunction,
                       const std::shared_ptr<ShaderFunction>& fragmentFunction,
                       const RenderTargetFormat& targetFormat,
                       const std::string& debugName = "");

        ~VulkanPipeline() override;

        /// @return Pointer to the VkPipeline handle.
        void* nativeHandle() const override;

        const std::string& debugName() const override;

        VkPipeline pipeline() const;
        VkPipelineLayout pipelineLayout() const;
        /// @brief Layout of descriptor set 0 (buffers).
        VkDescriptorSetLayout bufferSetLayout() const;
        /// @brief Layout of descriptor set 1 (combined image samplers).
        VkDescriptorSetLayout textureSetLayout() const;

    private:
        std::unique_ptr<VulkanPipelineNativeData> m_data;
        std::string m_debugName;
    };

} // namespace dmrender

#endif //RENDERING_VULKANPIPELINE_HPP
