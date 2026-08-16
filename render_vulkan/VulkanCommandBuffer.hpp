//
// Created by Artem Avdoshkin on 12.07.2025.
//
#ifndef RENDERING_VULKANCOMMANDBUFFER_HPP
#define RENDERING_VULKANCOMMANDBUFFER_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "Commandbuffer.hpp"
#include "CommandQueue.hpp"

namespace dmrender {

    struct VulkanCommandBufferData;

    /**
     * @class VulkanCommandBuffer
     * @brief Records one frame into the VkCommandBuffer owned by the current frame slot.
     *
     * Unlike MetalCommandBuffer this object does not own its native handle — the queue does, and
     * recycles it every @c kFramesInFlight frames. Constructing this class opens recording;
     * `commit()` closes it, submits it and presents, then hands the slot back to the queue.
     *
     * @par Deferred binding
     * `setVertexBuffer()`, `setUniformBuffer()` and `setTexture()` only remember what was bound.
     * The actual descriptor sets are allocated, written and bound lazily on the next draw call,
     * because Vulkan needs the pipeline's descriptor set layouts — only known once a pipeline is
     * set — and because one `vkUpdateDescriptorSets` can then cover every slot at once. Buffers
     * land in set 0 and textures in set 1; a set with nothing bound is never allocated.
     */
    class VulkanCommandBuffer : public CommandBuffer {
    public:
        explicit VulkanCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue);
        ~VulkanCommandBuffer() override;

        void beginRenderPass(std::shared_ptr<RenderPassDescriptor> pass) override;
        void setRenderPipeline(std::shared_ptr<Pipeline> pipeline) override;
        void setVertexBuffer(
            uint32_t slot, const std::shared_ptr<GBuffer>& buffer, size_t offset) override;
        void setUniformBuffer(
            uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset) override;
        void setTexture(
            uint32_t slot, ShaderStage stage, const std::shared_ptr<GImage>& image,
            const std::shared_ptr<GSampler>& sampler) override;
        void setPushConstants(
            ShaderStage stage, const void* data, size_t size, size_t offset = 0) override;

        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(const std::shared_ptr<GBuffer>& indexBuffer,
            IndexType indexType,
            uint32_t indexCount,
            uint32_t instanceCount,
            uint32_t firstIndexOffsetBytes,
            int32_t vertexOffset,
            uint32_t firstInstance) override;

        void drawIndirect(const std::shared_ptr<GBuffer>& argumentBuffer,
                          uint32_t drawCount, size_t offset = 0, uint32_t stride = 0) override;
        void drawIndexedIndirect(const std::shared_ptr<GBuffer>& indexBuffer,
                                 IndexType indexType,
                                 const std::shared_ptr<GBuffer>& argumentBuffer,
                                 uint32_t drawCount, size_t offset = 0, uint32_t stride = 0) override;

        void endRenderPass() override;

        void present(const std::shared_ptr<GImage>& image) override;
        void commit() override;

        /// @return The VkCommandBuffer handle itself (not a pointer to it), as ImGui expects.
        void* nativeHandle() override;

        /**
         * @brief Vulkan has no separate encoder object.
         * @return The same VkCommandBuffer as nativeHandle() while a render pass is active,
         *         nullptr otherwise, so the contract "valid only inside a render pass" holds.
         */
        void* nativeEncoder() override;

    private:
        /// Allocates, writes and binds the descriptor set for whatever is currently bound.
        void flushDescriptorSet();

        std::unique_ptr<VulkanCommandBufferData> m_data;
    };

} // namespace dmrender

#endif
