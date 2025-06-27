//
// Created by Artem Avdoshkin on 16.06.2025.
//

#ifndef RENDERING_METALCOMMANDBUFFER_HPP
#define RENDERING_METALCOMMANDBUFFER_HPP
#include "Commandbuffer.hpp"
#import "CommandQueue.hpp"
#include <memory>

namespace dmrender {

    struct MetalCommandBufferData;

    class MetalCommandBuffer : public CommandBuffer {
    public:
        explicit MetalCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue); // id<MTLCommandQueue>
        ~MetalCommandBuffer() override;

        void beginRenderPass(std::shared_ptr<RenderPassDescriptor> pass) override;
        void setRenderPipeline(std::shared_ptr<Pipeline> pipeline) override;
        void setVertexBuffer(
                uint32_t slot, const std::shared_ptr<GBuffer>& buffer, size_t offset) override;
        void setUniformBuffer(
                uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset) override;

        // --- Команды отрисовки ---
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(const std::shared_ptr<GBuffer>& indexBuffer,
                         IndexType indexType,
                         uint32_t indexCount,
                         uint32_t instanceCount,
                         uint32_t firstIndexOffsetBytes,
                         int32_t vertexOffset,
                         uint32_t firstInstance) override;

        void endRenderPass() override;

        void present(const std::shared_ptr<GImage>& image) override;
        void commit() override;

        void* nativeHandle() override;
        void* nativeEncoder() override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalCommandBufferData> m_data;
    };

} // namespace dmrender
#endif //RENDERING_METALCOMMANDBUFFER_HPP
